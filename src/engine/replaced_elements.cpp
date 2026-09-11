// Shared replaced-element initialization and interaction logic.
// Used by the Engine for both the app document and system panels.

#include "engine/replaced_elements.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/color_picker_overlay.h"
#include "engine/dropdown_overlay.h"
#include "engine/overlay.h"
#include "layout/el_select.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_svg.h"
#include "layout/el_video.h"
#include "platform/sdl_window.h"
#include "svg/svg_renderer.h"
#include "util/object_url.h"
#include "util/string_utils.h"

#include "broimage/decode.h"
#if BRO_WITH_WEBP
#include "render/webp_image.h"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace bro::engine {

// ---------------------------------------------------------------------------
// <img> intrinsic size
// ---------------------------------------------------------------------------

namespace {

// Dimensions of encoded image bytes, without decoding the pixels.
bool probeBytes(const uint8_t* data, size_t len, int& w, int& h) {
    if (!data || len == 0) return false;
    int c = 0;
    if (broimage::probe_dimensions_memory(data, len, &w, &h, &c)) return true;
#if BRO_WITH_WEBP
    // broimage is stb-backed and stb has no WebP, so a .webp needs libwebp's
    // header reader — the same split the decode paths have (render/webp_image.h).
    std::vector<uint8_t> ignored;
    int ww = 0, hh = 0;
    if (render::decodeWebPHeader(data, len, ww, hh)) { w = ww; h = hh; return true; }
#endif
    return false;
}

} // namespace

// Resolve `src` and read enough of it to learn the image's size. Returns
// false (leaving w/h at 0) for a missing file or an unreadable header, which
// leaves the <img> zero-sized — the same as a browser showing a broken image.
//
// Public because the JS `img.src =` setter needs the same answer for an image
// that is never inserted into the document — three.js's ImageLoader builds one,
// sets src, and reads the size off it without ever appending it, so nothing
// here would ever walk to it. One implementation, so a detached image and a
// laid-out one cannot disagree about how big the same file is.
bool probeImageSize(dom::Element* elem, const std::string& src,
                    int& w, int& h) {
    w = 0;
    h = 0;
    if (!elem || src.empty()) return false;

    // data: URLs carry their bytes inline. SVG data URLs are handled in the
    // layout adapter (it parses the <svg> width/height out of the markup), so
    // only raster payloads need probing here.
    if (src.compare(0, 5, "data:") == 0) {
        const auto comma = src.find(',');
        if (comma == std::string::npos) return false;
        const std::string meta = src.substr(5, comma - 5);
        if (meta.find("image/svg+xml") != std::string::npos) return false;
        const std::string body = src.substr(comma + 1);
        if (meta.find(";base64") == std::string::npos) return false;
        const std::vector<uint8_t> bytes = util::base64Decode(body);
        return probeBytes(bytes.data(), bytes.size(), w, h);
    }

    // blob: URL — bytes the page holds, registered when it minted the URL.
    // An SVG object URL answers from its markup, the same as an SVG file does.
    if (util::isObjectURL(src)) {
        auto data = util::lookupObjectURL(src);
        if (!data || data->bytes.empty()) return false;
        const char* chars = reinterpret_cast<const char*>(data->bytes.data());
        if (svg::looksLikeSvg(chars, data->bytes.size())) {
            float sw = 0, sh = 0;
            svg::svgIntrinsicSize(chars, data->bytes.size(), sw, sh);
            w = static_cast<int>(sw);
            h = static_cast<int>(sh);
            return w > 0 && h > 0;
        }
        return probeBytes(data->bytes.data(), data->bytes.size(), w, h);
    }

    // Resolve against the document's base path, matching the rule
    // DrawTraversal::loadImage uses when it later reads the same file.
    std::string clean = src;
    if (const auto q = clean.find_first_of("?#"); q != std::string::npos)
        clean.resize(q);
    std::string path;
    const bool absolute =
        (clean.size() >= 2 && clean[1] == ':') ||
        (!clean.empty() && (clean[0] == '/' || clean[0] == '\\'));
    const std::string& base = elem->document() ? elem->document()->basePath()
                                               : std::string();
    if (absolute || base.empty()) {
        path = clean;
    } else {
        path = base;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += clean;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    // Header only. Every format we probe puts its dimensions in the first few
    // hundred bytes, so a 64 KB ceiling covers them all without reading a
    // multi-megabyte photo just to size its box. The full decode happens later
    // in the draw path, and only for images that are actually painted.
    std::vector<uint8_t> head(64 * 1024);
    ifs.read(reinterpret_cast<char*>(head.data()),
             static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(ifs.gcount()));

    // An SVG carries its size in the root tag, not in a binary header, so the
    // bitmap probe cannot see it. Reading it here is what makes the <img> a
    // replaced element with a real intrinsic size; otherwise it lays out as an
    // empty inline box and the icon has nowhere to paint. Same reader the paint
    // path and the rasterizer use, so all three agree on how big it is.
    if (svg::looksLikeSvg(reinterpret_cast<const char*>(head.data()), head.size())) {
        float sw = 0, sh = 0;
        svg::svgIntrinsicSize(reinterpret_cast<const char*>(head.data()),
                              head.size(), sw, sh);
        w = static_cast<int>(sw);
        h = static_cast<int>(sh);
        return w > 0 && h > 0;
    }

    return probeBytes(head.data(), head.size(), w, h);
}

// ---------------------------------------------------------------------------
// Replaced element initialization
// ---------------------------------------------------------------------------

void ensureReplacedElements(dom::Element* elem, render::Renderer* renderer,
                            broaudio::Engine* audioEngine) {
    if (!elem) return;

    const auto& tag = elem->tagName();

    if (tag == "INPUT" && !elem->inputControl()) {
        auto ctrl = std::make_unique<layout::ElInput>(renderer);
        ctrl->setElement(elem);
        elem->setInputControl(std::move(ctrl));
    } else if (tag == "TEXTAREA" && !elem->textareaControl()) {
        auto ctrl = std::make_unique<layout::ElTextarea>(renderer);
        ctrl->setElement(elem);
        elem->setTextareaControl(std::move(ctrl));
    } else if (tag == "SELECT" && !elem->selectControl()) {
        auto ctrl = std::make_unique<layout::ElSelect>(renderer);
        ctrl->setElement(elem);
        ctrl->initSelectedIndex();
        elem->setSelectControl(std::move(ctrl));
    } else if ((tag == "SVG" || tag == "svg") && !elem->svgControl()) {
        auto ctrl = std::make_unique<layout::ElSvg>(renderer);
        ctrl->setElement(elem);
        ctrl->parseAttributes();
        elem->setSvgControl(std::move(ctrl));
    } else if (tag == "IMG" || tag == "img") {
        // Give layout the image's intrinsic size. Without it an <img> is not a
        // replaced element, so it lays out as an empty inline box and never
        // appears — see Element::imageNaturalWidth().
        //
        // Re-probed only when `src` changes: this runs on every DOM-dirty
        // pass, and reading a header per pass per image would put file I/O on
        // the layout path.
        const std::string src = elem->getAttribute("src");
        if (!src.empty() && src != elem->imageProbedSrc()) {
            int w = 0, h = 0;
            probeImageSize(elem, src, w, h);
            elem->setImageNaturalSize(src, w, h);
        } else if (src.empty() && !elem->imageProbedSrc().empty()) {
            // src removed: drop the stale size rather than keep sizing the
            // box from an image that is no longer referenced.
            elem->setImageNaturalSize("", 0, 0);
        }
    } else if ((tag == "VIDEO" || tag == "video") && !elem->videoControl()) {
        auto ctrl = std::make_unique<layout::ElVideo>(renderer);
        ctrl->setElement(elem);
        ctrl->setAudioEngine(audioEngine);
        // If the element already has a src attribute, load it now.
        // Otherwise the binding will trigger load when src is set.
        std::string src = elem->getAttribute("src");
        if (!src.empty()) ctrl->load(src);
        elem->setVideoControl(std::move(ctrl));
    }

    // Recurse into children
    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element) {
            ensureReplacedElements(static_cast<dom::Element*>(child), renderer,
                                   audioEngine);
        }
    }

    // Recurse into shadow DOM
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == dom::NodeType::Element) {
                ensureReplacedElements(static_cast<dom::Element*>(child),
                                       renderer, audioEngine);
            }
        }
    }
}

} // namespace bro::engine

