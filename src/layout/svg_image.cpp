#include "layout/svg_image.h"
#include "layout/svg_common.h"
#include "layout/svg_paint.h"
#include "dom/document.h"
#include "dom/element.h"
#include "render/image_store.h"
#include "render/renderer.h"
#include "render/svg_image.h"
#include "util/string_utils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace bro::layout {

namespace {

// One parsed SVG image: its document, the box mapping, and the data: images
// its <image>s decoded.
struct SvgImageDoc {
    std::unique_ptr<dom::Document> doc;
    const dom::Element* root = nullptr;
    float intrinsicW = 0, intrinsicH = 0;
    bool hasViewBox = false;
    std::unordered_map<std::string, std::shared_ptr<const render::DecodedImage>> images;
};

std::shared_ptr<SvgImageDoc> parseSvgImage(const char* data, size_t len) {
    auto out = std::make_shared<SvgImageDoc>();
    out->doc = std::make_unique<dom::Document>();
    if (!out->doc->parseXml(std::string(data, len))) return nullptr;
    dom::Element* root = out->doc->documentElement();
    if (!root || svgcommon::lower(root->tagName()) != "svg") return nullptr;
    // Its <style> sheets join the cascade, then every element is styled.
    out->doc->markElementStructureDirty();
    out->doc->resolveStyles();
    out->root = root;
    render::svgIntrinsicSize(data, len, out->intrinsicW, out->intrinsicH);
    float vb[4];
    out->hasViewBox = render::svgViewBox(data, len, vb);
    return out;
}

// A data: URI's bytes (base64 or percent-encoded); empty for anything else.
std::vector<uint8_t> dataUriBytes(const std::string& uri) {
    if (uri.compare(0, 5, "data:") != 0) return {};
    const size_t comma = uri.find(',');
    if (comma == std::string::npos) return {};
    const std::string meta = uri.substr(5, comma - 5);
    const std::string body = uri.substr(comma + 1);
    if (meta.find(";base64") != std::string::npos) return util::base64Decode(body);
    std::vector<uint8_t> out;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '%' && i + 2 < body.size()) {
            out.push_back(static_cast<uint8_t>(std::strtol(body.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(static_cast<uint8_t>(body[i]));
        }
    }
    return out;
}

void paintDoc(render::Renderer* r, SvgImageDoc& d, float x, float y, float w, float h) {
    if (!d.root || w <= 0 || h <= 0) return;
    SvgImageDoc* dp = &d;
    SvgImageResolver images = [dp](const std::string& href) -> std::shared_ptr<const render::DecodedImage> {
        auto it = dp->images.find(href);
        if (it != dp->images.end()) return it->second;
        std::shared_ptr<const render::DecodedImage> img;
        std::vector<uint8_t> bytes = dataUriBytes(href);
        if (!bytes.empty()) {
            auto decoded = std::make_shared<render::DecodedImage>();
            std::string err;
            // An SVG inside an SVG image would recurse without bound; SVG
            // images do not nest here.
            if (!render::looksLikeSvg(reinterpret_cast<const char*>(bytes.data()), bytes.size()) &&
                render::decodeImageData(bytes.data(), bytes.size(), true, *decoded, err))
                img = decoded;
        }
        dp->images[href] = img;
        return img;
    };
    r->save();
    r->setClip(x, y, w, h);
    if (d.hasViewBox) {
        paintSvgSubtree(r, d.root, x, y, w, h, images);
    } else {
        // No viewBox: the content is in px of the intrinsic size (or of the
        // box when it has none), scaled into the box.
        const float iw = d.intrinsicW > 0 ? d.intrinsicW : w;
        const float ih = d.intrinsicH > 0 ? d.intrinsicH : h;
        r->translate(x, y);
        r->scale(w / iw, h / ih);
        paintSvgSubtree(r, d.root, 0, 0, iw, ih, images);
    }
    r->restore();
}

bool painter(render::Renderer* r, const char* data, size_t len, float w, float h) {
    auto doc = parseSvgImage(data, len);
    if (!doc) return false;
    paintDoc(r, *doc, 0, 0, w, h);
    return true;
}

} // namespace

void paintSvgImage(render::Renderer* r, const render::DecodedImage& img, float x, float y, float w, float h) {
    if (!r || img.svgMarkup.empty()) return;
    // Parsed once per image (the page thread's), bounded: an evicted image
    // reparses if it paints again.
    thread_local std::unordered_map<uint64_t, std::shared_ptr<SvgImageDoc>> cache;
    std::shared_ptr<SvgImageDoc> doc;
    if (auto it = cache.find(img.id); it != cache.end()) {
        doc = it->second;
    } else {
        doc = parseSvgImage(img.svgMarkup.data(), img.svgMarkup.size());
        if (cache.size() >= 256) cache.clear();
        cache[img.id] = doc;
    }
    if (doc) paintDoc(r, *doc, x, y, w, h);
}

void installSvgImagePainter() { render::setSvgPainter(&painter); }

} // namespace bro::layout
