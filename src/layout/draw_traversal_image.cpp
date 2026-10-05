// The image cache: loadImage resolves data:, blob: and file URLs into
// CachedImage entries (raster bytes or SVG markup, plus intrinsic size).

#include "layout/draw_traversal.h"
#include "svg/svg_renderer.h"
#include "util/log.h"
#include "util/object_url.h"
#include "util/string_utils.h"
#include "broimage/decode.h"

#include <atomic>
#include <fstream>

namespace bro::layout {

namespace {

// Hand out a process-unique id for each newly cached image. The renderer's
// DecodedImageCache keys on this so an image is decoded once, not every frame.
// Ids are process-global (not per-DrawTraversal) so a rebuilt traversal after a
// document reload can never collide with a stale renderer-side cache entry.
uint64_t nextImageId() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

// Decode a single percent-encoded (URL-encoded) string in place semantics.
// SVG/utf8 data URLs may percent-encode the markup (e.g. %3C for '<').
static std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int hi = hex(s[i+1]);
            int lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Decode standard base64 (RFC 4648). Tolerates whitespace and missing padding.
// Base64 lives in util so the <img> intrinsic-size probe in
// engine/replaced_elements.cpp decodes data: URLs the same way this does.
static std::vector<uint8_t> base64Decode(const std::string& s) {
    return bro::util::base64Decode(s);
}

void DrawTraversal::loadImage(const std::string& url, const std::string& basePath) {
    if (imageCache_.count(url)) return;

    // data: URL — inline image content. Three forms we care about:
    //   data:image/svg+xml,<svg...>            (utf8 percent-encoded, with or without explicit ;utf8)
    //   data:image/svg+xml;utf8,<svg...>
    //   data:image/<type>;base64,<bytes>       (PNG/JPG/etc, base64-encoded)
    if (url.compare(0, 5, "data:") == 0) {
        auto comma = url.find(',');
        if (comma == std::string::npos) {
            imageCache_[url] = CachedImage{};
            return;
        }
        std::string meta = url.substr(5, comma - 5);   // e.g. "image/svg+xml;utf8"
        std::string body = url.substr(comma + 1);
        bool isBase64 = meta.find(";base64") != std::string::npos;
        bool isSvgXml = meta.find("image/svg+xml") != std::string::npos;

        CachedImage img;
        img.id = nextImageId();
        if (isSvgXml) {
            std::string markup = isBase64
                ? std::string(reinterpret_cast<const char*>(base64Decode(body).data()),
                              base64Decode(body).size())
                : urlDecode(body);
            img.isSvg = true;
            img.data.assign(markup.begin(), markup.end());
            // Parse intrinsic size from <svg width=... height=...>
            auto svgPos = markup.find("<svg");
            if (svgPos != std::string::npos) {
                auto endPos = markup.find('>', svgPos);
                if (endPos != std::string::npos) {
                    std::string tag = markup.substr(svgPos, endPos - svgPos);
                    auto attr = [&](const char* name) -> int {
                        std::string needle = std::string(" ") + name + "=";
                        auto p = tag.find(needle);
                        if (p == std::string::npos) return 0;
                        p += needle.size();
                        if (p >= tag.size()) return 0;
                        char q = tag[p];
                        if (q != '"' && q != '\'') return 0;
                        ++p;
                        auto eq = tag.find(q, p);
                        if (eq == std::string::npos) return 0;
                        return (int)std::strtof(tag.substr(p, eq - p).c_str(), nullptr);
                    };
                    img.width = attr("width");
                    img.height = attr("height");
                }
            }
            imageCache_[url] = std::move(img);
            return;
        }
        // Raster data URL — base64 or percent-encoded body
        std::vector<uint8_t> bytes = isBase64
            ? base64Decode(body)
            : [&]() {
                std::string d = urlDecode(body);
                return std::vector<uint8_t>(d.begin(), d.end());
            }();
        int w = 0, h = 0, comp = 0;
        if (!bytes.empty() &&
            broimage::probe_dimensions_memory(bytes.data(), bytes.size(), &w, &h, &comp)) {
            img.width = w;
            img.height = h;
        }
        img.data = std::move(bytes);
        imageCache_[url] = std::move(img);
        return;
    }

    // blob: URL — bytes the page holds, copied into the object-URL table when
    // it minted the URL (util/object_url.h). No file to open, and no JSContext
    // to ask on this thread, which is exactly why the bytes live there.
    if (bro::util::isObjectURL(url)) {
        auto data = bro::util::lookupObjectURL(url);
        CachedImage img;
        img.id = nextImageId();
        if (!data || data->bytes.empty()) {
            imageCache_[url] = CachedImage{};   // revoked or never registered
            return;
        }
        img.data = data->bytes;
        const char* chars = reinterpret_cast<const char*>(img.data.data());
        if (bro::svg::looksLikeSvg(chars, img.data.size())) {
            img.isSvg = true;
            float sw = 0, sh = 0;
            bro::svg::svgIntrinsicSize(chars, img.data.size(), sw, sh);
            img.width = static_cast<int>(sw);
            img.height = static_cast<int>(sh);
        } else {
            int w = 0, h = 0, comp = 0;
            if (broimage::probe_dimensions_memory(img.data.data(), img.data.size(),
                                                  &w, &h, &comp)) {
                img.width = w;
                img.height = h;
            }
        }
        imageCache_[url] = std::move(img);
        return;
    }

    // Strip URL query/fragment so `thumbnails/foo.png?v=12345` (standard
    // cache-bust) resolves to the file on disk.
    std::string cleanUrl = url;
    auto qPos = cleanUrl.find_first_of("?#");
    if (qPos != std::string::npos) cleanUrl.resize(qPos);

    std::string path;
    if (cleanUrl.size() >= 2 && cleanUrl[1] == ':') {
        path = cleanUrl;
    } else if (!cleanUrl.empty() && (cleanUrl[0] == '/' || cleanUrl[0] == '\\')) {
        path = cleanUrl;
    } else if (!basePath.empty()) {
        path = basePath;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += cleanUrl;
    } else {
        path = cleanUrl;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG_WARN("loadImage: failed to open '%s'", path.c_str());
        imageCache_[url] = CachedImage{};  // negative-cache so we don't re-warn each frame
        return;
    }
    auto fileSize = ifs.tellg();
    ifs.seekg(0);
    CachedImage img;
    img.id = nextImageId();
    img.data.resize(static_cast<size_t>(fileSize));
    ifs.read(reinterpret_cast<char*>(img.data.data()), fileSize);

    // An SVG file is markup, not a raster payload: keep the bytes and let the
    // paint branch hand them to drawSvgMarkup, exactly as an `image/svg+xml`
    // data: URL already does above. Without this an `<img src="icon.svg">` —
    // how essentially every toolbar on the web draws its icons — reaches
    // broimage, which decodes bitmaps, and comes back sizeless and unpaintable.
    if (bro::svg::looksLikeSvg(reinterpret_cast<const char*>(img.data.data()),
                               img.data.size())) {
        img.isSvg = true;
        float sw = 0, sh = 0;
        bro::svg::svgIntrinsicSize(reinterpret_cast<const char*>(img.data.data()),
                                   img.data.size(), sw, sh);
        img.width = static_cast<int>(sw);
        img.height = static_cast<int>(sh);
        imageCache_[url] = std::move(img);
        return;
    }

    int w = 0, h = 0, comp = 0;
    if (broimage::probe_dimensions_memory(img.data.data(), img.data.size(), &w, &h, &comp)) {
        img.width = w;
        img.height = h;
    }
    imageCache_[url] = std::move(img);
}

} // namespace bro::layout
