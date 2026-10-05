// Backgrounds: background-color plus the layered background-image list —
// url() images (size / position / repeat) and linear / radial / conic
// gradients, repeating forms included — with background-blend-mode.

#include "layout/draw_traversal_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace bro::layout {

void DrawTraversal::drawBackground(dom::Element* elem, float x, float y, float w, float h) {
    auto& style = elem->computedStyle();
    render::Radii radii = getRadii(style, w, h);
    bool rounded = !radii.isZero();

    // Background color
    auto bgIt = style.find("background-color");
    if (bgIt != style.end() && !bgIt->second.empty()) {
        bromath::Color c;
        if (tryParseColor(bgIt->second, c) && c.a > 0) {
            if (rounded)
                renderer_->fillRoundRectRadii(x, y, w, h, radii, c);
            else
                renderer_->fillRect(x, y, w, h, c);
        }
    }

    // For background image / gradient, clip to rounded bounds. Save the
    // canvas state and pop after drawing the image/gradient.
    bool clipped = false;
    auto imgItCheck = style.find("background-image");
    if (rounded && imgItCheck != style.end() && !imgItCheck->second.empty() &&
        imgItCheck->second != "none") {
        renderer_->save();
        renderer_->setClipRRect(x, y, w, h, radii);
        clipped = true;
    }

    // Helper: split CSS multi-value at top-level commas (paren/quote-aware).
    auto splitLayers = [](const std::string& v) {
        std::vector<std::string> out;
        std::string cur;
        int depth = 0;
        bool inQ = false;
        char qc = 0;
        for (char c : v) {
            if (inQ) { cur += c; if (c == qc) inQ = false; continue; }
            if (c == '"' || c == '\'') { inQ = true; qc = c; cur += c; continue; }
            if (c == '(') { ++depth; cur += c; continue; }
            if (c == ')') { --depth; cur += c; continue; }
            if (c == ',' && depth == 0) {
                while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
                while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
                out.push_back(cur);
                cur.clear();
                continue;
            }
            cur += c;
        }
        while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
        while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
        if (!cur.empty() || !out.empty()) out.push_back(cur);
        return out;
    };

    // Build per-layer values for each background sub-property.
    auto getLayerProp = [&](const char* name, const std::string& fallback) {
        auto it = style.find(name);
        if (it == style.end() || it->second.empty()) return std::vector<std::string>{fallback};
        auto v = splitLayers(it->second);
        if (v.empty()) v.push_back(fallback);
        return v;
    };

    auto imgIt = style.find("background-image");
    if (imgIt != style.end() && !imgIt->second.empty() && imgIt->second != "none") {
        auto images   = splitLayers(imgIt->second);
        auto positions= getLayerProp("background-position", "0% 0%");
        auto sizes    = getLayerProp("background-size", "auto");
        auto repeats  = getLayerProp("background-repeat", "repeat");
        auto blends   = getLayerProp("background-blend-mode", "normal");

        // Paint layers from last to first (CSS: first listed is on top).
        for (size_t li = images.size(); li-- > 0; ) {
            const std::string& val = images[li];
            if (val.empty() || val == "none") continue;
            const std::string layerPosition = li < positions.size() ? positions[li] : positions.back();
            const std::string layerSize     = li < sizes.size()     ? sizes[li]     : sizes.back();
            const std::string layerRepeat   = li < repeats.size()   ? repeats[li]   : repeats.back();

            // background-blend-mode: composite this layer against the layers
            // (and background color) already painted beneath it. A non-normal
            // mode wraps the layer's paint in a blended group.
            render::BlendMode layerBlend = parseBlendMode(
                li < blends.size() ? blends[li] : blends.back());
            bool layerBlended = (layerBlend != render::BlendMode::Normal);
            if (layerBlended) renderer_->saveLayerWithBlend(layerBlend);

        if (val.substr(0, 4) == "url(") {
            // Extract URL
            size_t start = val.find('(') + 1;
            size_t end = val.rfind(')');
            if (end > start) {
                std::string url = val.substr(start, end - start);
                if (!url.empty() && (url.front() == '"' || url.front() == '\'')) {
                    url = url.substr(1, url.size() - 2);
                }
                loadImage(url, basePath_);
                auto imgCacheIt = imageCache_.find(url);
                if (imgCacheIt != imageCache_.end() && !imgCacheIt->second.data.empty()) {
                    float imgW = static_cast<float>(imgCacheIt->second.width);
                    float imgH = static_cast<float>(imgCacheIt->second.height);
                    float drawW = imgW > 0 ? imgW : w;
                    float drawH = imgH > 0 ? imgH : h;

                    // background-size
                    {
                        const auto& bs = layerSize;
                        if (bs == "cover" && imgW > 0 && imgH > 0) {
                            float scale = std::max(w / imgW, h / imgH);
                            drawW = imgW * scale; drawH = imgH * scale;
                        } else if (bs == "contain" && imgW > 0 && imgH > 0) {
                            float scale = std::min(w / imgW, h / imgH);
                            drawW = imgW * scale; drawH = imgH * scale;
                        } else if (bs != "auto") {
                            // Parse "Wpx Hpx" or "W% H%"
                            std::istringstream bsIss(bs);
                            std::string wStr, hStr;
                            bsIss >> wStr;
                            bsIss >> hStr;
                            if (!wStr.empty() && wStr != "auto")
                                drawW = parseLengthPx(wStr, w);
                            if (!hStr.empty() && hStr != "auto")
                                drawH = parseLengthPx(hStr, h);
                            else if (imgW > 0 && imgH > 0 && !wStr.empty() && wStr != "auto")
                                drawH = drawW * imgH / imgW; // maintain aspect ratio
                        }
                    }

                    // background-position
                    float posX = x, posY = y;
                    if (!layerPosition.empty()) {
                        const auto& bp = layerPosition;
                        if (bp == "center") {
                            posX = x + (w - drawW) / 2;
                            posY = y + (h - drawH) / 2;
                        } else if (bp == "right") {
                            posX = x + w - drawW;
                        } else if (bp == "bottom") {
                            posY = y + h - drawH;
                        } else {
                            std::istringstream bpIss(bp);
                            std::string pxStr, pyStr;
                            bpIss >> pxStr;
                            bpIss >> pyStr;
                            if (!pxStr.empty()) posX = x + parseLengthPx(pxStr, w);
                            if (!pyStr.empty()) posY = y + parseLengthPx(pyStr, h);
                        }
                    }

                    // background-repeat
                    std::string repeat = layerRepeat.empty() ? "repeat" : layerRepeat;

                    if (repeat == "no-repeat") {
                        // Clip to the box so an oversized image (e.g.
                        // background-size: cover, or a natural-size image larger
                        // than the element) doesn't bleed past its bounds.
                        renderer_->save();
                        renderer_->setClip(x, y, w, h);
                        renderer_->drawImage(imgCacheIt->second.data.data(),
                                            imgCacheIt->second.data.size(),
                                            posX, posY, drawW, drawH,
                                            imgCacheIt->second.id);
                        renderer_->restore();
                    } else {
                        // Tile the image
                        renderer_->save();
                        renderer_->setClip(x, y, w, h);
                        bool repeatX = (repeat == "repeat" || repeat == "repeat-x");
                        bool repeatY = (repeat == "repeat" || repeat == "repeat-y");
                        float startX = repeatX ? x - std::fmod(posX - x, drawW) - drawW : posX;
                        float startY = repeatY ? y - std::fmod(posY - y, drawH) - drawH : posY;
                        float endX = repeatX ? x + w : startX + drawW;
                        float endY = repeatY ? y + h : startY + drawH;
                        for (float iy = startY; iy < endY; iy += drawH) {
                            for (float ix = startX; ix < endX; ix += drawW) {
                                renderer_->drawImage(imgCacheIt->second.data.data(),
                                                    imgCacheIt->second.data.size(),
                                                    ix, iy, drawW, drawH,
                                                    imgCacheIt->second.id);
                            }
                        }
                        renderer_->restore();
                    }
                }
            }
        }
        else if (val.find("linear-gradient") != std::string::npos ||
                 val.find("radial-gradient") != std::string::npos ||
                 val.find("conic-gradient") != std::string::npos) {
            // Parse gradient color stops from the CSS value
            auto parenStart = val.find('(');
            auto parenEnd = val.rfind(')');
            if (parenStart != std::string::npos && parenEnd != std::string::npos) {
                std::string inner = val.substr(parenStart + 1, parenEnd - parenStart - 1);
                // Split on commas (respecting nested parens)
                std::vector<std::string> parts;
                int depth = 0;
                std::string cur;
                for (char c : inner) {
                    if (c == '(') ++depth;
                    else if (c == ')') --depth;
                    else if (c == ',' && depth == 0) {
                        parts.push_back(cur);
                        cur.clear();
                        continue;
                    }
                    cur += c;
                }
                if (!cur.empty()) parts.push_back(cur);

                // Gradient kind. `repeating-*` variants contain the base name
                // as a substring, so the base flags stay valid for them.
                bool isRadial = (val.find("radial-gradient") != std::string::npos);
                bool isConic  = (val.find("conic-gradient")  != std::string::npos);
                bool isRepeating = (val.find("repeating-") != std::string::npos);

                // Parse direction/angle for linear-gradient, shape/extent/
                // position prefix for radial-gradient, or "from <angle>" for
                // conic-gradient.
                float angleDeg = 180;    // linear default: to bottom
                float conicFromDeg = 0;  // conic default: 0deg (12 o'clock)
                size_t colorStart = 0;

                // Radial-gradient defaults: ellipse, farthest-corner, center.
                bool radialIsCircle = false;
                enum RadExtent { RAD_FARTHEST_CORNER, RAD_FARTHEST_SIDE,
                                 RAD_CLOSEST_CORNER,  RAD_CLOSEST_SIDE };
                RadExtent radExtent = RAD_FARTHEST_CORNER;
                float radCxFrac = 0.5f, radCyFrac = 0.5f; // fraction of (w, h)

                if (isRadial && !parts.empty()) {
                    std::string first = parts[0];
                    while (!first.empty() && first.front() == ' ') first.erase(first.begin());
                    while (!first.empty() && first.back() == ' ') first.pop_back();
                    // The prefix (if present) ends before the first color stop.
                    // Heuristic: if the first part contains shape/extent keywords
                    // or starts with "at ", treat it as the prefix.
                    bool looksPrefix =
                        first.find("circle") != std::string::npos ||
                        first.find("ellipse") != std::string::npos ||
                        first.find("at ") != std::string::npos ||
                        first.find("closest") != std::string::npos ||
                        first.find("farthest") != std::string::npos;
                    if (looksPrefix) {
                        if (first.find("circle") != std::string::npos) radialIsCircle = true;
                        if (first.find("closest-side") != std::string::npos) radExtent = RAD_CLOSEST_SIDE;
                        else if (first.find("closest-corner") != std::string::npos) radExtent = RAD_CLOSEST_CORNER;
                        else if (first.find("farthest-side") != std::string::npos) radExtent = RAD_FARTHEST_SIDE;
                        else if (first.find("farthest-corner") != std::string::npos) radExtent = RAD_FARTHEST_CORNER;
                        // Position: "at <x> <y>"
                        auto atPos = first.find("at ");
                        if (atPos != std::string::npos) {
                            std::string posStr = first.substr(atPos + 3);
                            while (!posStr.empty() && posStr.front() == ' ') posStr.erase(posStr.begin());
                            // Tokenize on spaces
                            std::vector<std::string> toks;
                            std::string t;
                            for (char c : posStr) {
                                if (c == ' ') { if (!t.empty()) { toks.push_back(t); t.clear(); } }
                                else t += c;
                            }
                            if (!t.empty()) toks.push_back(t);
                            auto resolveAxis = [](const std::string& tok, bool isX, float& outFrac) {
                                if (tok == "left") { if (isX) outFrac = 0.0f; }
                                else if (tok == "right") { if (isX) outFrac = 1.0f; }
                                else if (tok == "top") { if (!isX) outFrac = 0.0f; }
                                else if (tok == "bottom") { if (!isX) outFrac = 1.0f; }
                                else if (tok == "center") { outFrac = 0.5f; }
                                else if (!tok.empty() && tok.back() == '%') {
                                    outFrac = std::strtof(tok.c_str(), nullptr) / 100.0f;
                                }
                            };
                            if (toks.size() == 1) {
                                resolveAxis(toks[0], true, radCxFrac);
                                resolveAxis(toks[0], false, radCyFrac);
                            } else if (toks.size() >= 2) {
                                resolveAxis(toks[0], true, radCxFrac);
                                resolveAxis(toks[1], false, radCyFrac);
                            }
                        }
                        colorStart = 1;
                    }
                }

                if (!isRadial && !isConic && !parts.empty()) {
                    std::string first = parts[0];
                    // Trim
                    while (!first.empty() && first.front() == ' ') first.erase(first.begin());
                    while (!first.empty() && first.back() == ' ') first.pop_back();
                    if (first.find("to ") == 0) {
                        if (first == "to right") angleDeg = 90;
                        else if (first == "to left") angleDeg = 270;
                        else if (first == "to top") angleDeg = 0;
                        else if (first == "to bottom") angleDeg = 180;
                        else if (first == "to top right" || first == "to right top") angleDeg = 45;
                        else if (first == "to bottom right" || first == "to right bottom") angleDeg = 135;
                        else if (first == "to bottom left" || first == "to left bottom") angleDeg = 225;
                        else if (first == "to top left" || first == "to left top") angleDeg = 315;
                        colorStart = 1;
                    } else {
                        char* end = nullptr;
                        float a = std::strtof(first.c_str(), &end);
                        if (end != first.c_str()) {
                            angleDeg = a;
                            colorStart = 1;
                        }
                    }
                }

                // conic-gradient "from <angle> [at <pos>]" prefix. Only the
                // starting angle is honored; the conic center stays the box
                // center.
                if (isConic && !parts.empty()) {
                    std::string first = parts[0];
                    while (!first.empty() && first.front() == ' ') first.erase(first.begin());
                    while (!first.empty() && first.back() == ' ') first.pop_back();
                    auto fromPos = first.find("from ");
                    if (fromPos == 0 || first.find("at ") == 0) {
                        if (fromPos != std::string::npos) {
                            std::string a = first.substr(fromPos + 5);
                            while (!a.empty() && a.front() == ' ') a.erase(a.begin());
                            char* end = nullptr;
                            float av = std::strtof(a.c_str(), &end);
                            if (end != a.c_str()) {
                                std::string unit(end);
                                if (unit == "rad")  av *= 180.0f / 3.14159265f;
                                else if (unit == "grad") av *= 0.9f;
                                else if (unit == "turn") av *= 360.0f;
                                conicFromDeg = av;
                            }
                        }
                        colorStart = 1;
                    }
                }

                // Reference length for resolving <length> stop positions to a
                // fraction of the gradient line. Linear: |W·sin|+|H·cos| (same
                // metric the draw below uses). Radial: the end radius (extent
                // rule applied to the box), matching the ray the stops live
                // on — using anything else skews px-positioned stops (visible
                // as wrong ring spacing in repeating-radial-gradient). Conic
                // uses angle units instead, so length positions there fall
                // back to auto.
                float refLen = 1.0f;
                if (isRadial) {
                    float rcx = radCxFrac * w, rcy = radCyFrac * h;
                    float csX = std::min(rcx, w - rcx), csY = std::min(rcy, h - rcy);
                    float fsX = std::max(rcx, w - rcx), fsY = std::max(rcy, h - rcy);
                    switch (radExtent) {
                        case RAD_CLOSEST_SIDE:
                            refLen = radialIsCircle ? std::min(csX, csY) : csX; break;
                        case RAD_CLOSEST_CORNER:
                            refLen = radialIsCircle ? std::sqrt(csX*csX + csY*csY)
                                                    : csX * std::sqrt(2.0f); break;
                        case RAD_FARTHEST_SIDE:
                            refLen = radialIsCircle ? std::max(fsX, fsY) : fsX; break;
                        case RAD_FARTHEST_CORNER:
                        default:
                            refLen = radialIsCircle ? std::sqrt(fsX*fsX + fsY*fsY)
                                                    : fsX * std::sqrt(2.0f); break;
                    }
                    if (refLen < 1.0f) refLen = 1.0f;
                } else if (!isConic) {
                    float rad0 = angleDeg * 3.14159265f / 180.0f;
                    refLen = std::abs(w * std::sin(rad0)) + std::abs(h * std::cos(rad0));
                    if (refLen < 1.0f) refLen = 1.0f;
                }

                // Parse color stops. Each part is "<color> [<pos1>] [<pos2>]".
                // The CSS double-position form (e.g. "#hex 0 40%") is shorthand
                // for two stops of the same color, producing a hard band. We
                // strip up to two trailing position tokens off the end (the
                // color may itself contain spaces inside rgb()/hsl()), then emit
                // one stop per position found.
                //
                // A position token resolves to a fraction of the gradient:
                //   %                → v/100
                //   <length> (px)    → v/refLen (linear/radial)
                //   <angle> (deg/…)  → v/360    (conic)
                //   bare 0           → 0
                // Anything else (em, unresolvable) is stripped anyway and the
                // stop falls back to an evenly-spaced auto offset — the key is
                // that the position token never leaks into the color string.
                auto parsePosToken = [&](const std::string& t, float& outFrac) -> bool {
                    if (t.empty()) return false;
                    char* end = nullptr;
                    float v = std::strtof(t.c_str(), &end);
                    if (end == t.c_str()) return false;
                    std::string unit(end);
                    if (unit == "%") { outFrac = v / 100.0f; return true; }
                    if (isConic) {
                        if (unit == "deg")  { outFrac = v / 360.0f; return true; }
                        if (unit == "grad") { outFrac = v / 400.0f; return true; }
                        if (unit == "rad")  { outFrac = v / 6.28318530718f; return true; }
                        if (unit == "turn") { outFrac = v; return true; }
                    } else {
                        if (unit == "px") { outFrac = v / refLen; return true; }
                    }
                    if (unit.empty() && v == 0.0f) { outFrac = 0.0f; return true; }
                    return false;  // unresolvable length/angle → auto
                };
                // A trailing token that begins like a number is a position
                // token and must be peeled off the color regardless of whether
                // we can resolve it to a fraction.
                auto looksLikePos = [](const std::string& t) -> bool {
                    if (t.empty()) return false;
                    char c = t[0];
                    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
                };

                std::vector<render::ColorStop> stops;
                size_t numColors = parts.size() - colorStart;
                for (size_t i = colorStart; i < parts.size(); ++i) {
                    std::string part = parts[i];
                    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
                    while (!part.empty() && part.back() == ' ') part.pop_back();

                    // Pull up to two trailing position tokens (kept in order).
                    std::vector<float> positions;
                    // Only a space outside any parens separates a position:
                    // the spaces inside `rgb(20, 40, 90)` precede number
                    // tokens too ("90)"), and peeling those shreds the color.
                    auto lastTopLevelSpace = [](const std::string& s) -> size_t {
                        int d = 0;
                        size_t found = std::string::npos;
                        for (size_t k = 0; k < s.size(); ++k) {
                            if (s[k] == '(') ++d;
                            else if (s[k] == ')') --d;
                            else if (s[k] == ' ' && d == 0) found = k;
                        }
                        return found;
                    };
                    for (int n = 0; n < 2; ++n) {
                        size_t sp = lastTopLevelSpace(part);
                        if (sp == std::string::npos) break;
                        std::string tail = part.substr(sp + 1);
                        if (!looksLikePos(tail)) break;
                        float frac;
                        bool ok = parsePosToken(tail, frac);
                        part.erase(sp);
                        while (!part.empty() && part.back() == ' ') part.pop_back();
                        if (ok) positions.insert(positions.begin(), frac);
                    }

                    bromath::Color sc = cfromColor8({0, 0, 0, 255});
                    tryParseColor(part, sc);

                    if (positions.empty()) {
                        float offset = numColors > 1
                            ? static_cast<float>(i - colorStart) / static_cast<float>(numColors - 1)
                            : 0.0f;
                        stops.push_back({offset, sc});
                    } else {
                        for (float p : positions) stops.push_back({p, sc});
                    }
                }

                // CSS: stop offsets are non-decreasing; clamp each up to the max
                // seen so far (Skia also requires sorted positions).
                {
                    float maxSoFar = 0.0f;
                    bool firstStop = true;
                    for (auto& s : stops) {
                        if (firstStop) { maxSoFar = s.offset; firstStop = false; }
                        else if (s.offset < maxSoFar) s.offset = maxSoFar;
                        else maxSoFar = s.offset;
                    }
                }

                // repeating-*-gradient: tile the resolved stop pattern across
                // [0,1]. The renderer uses a clamp tile mode, so we materialize
                // the repetition as explicit stops here.
                if (isRepeating && stops.size() >= 2) {
                    float firstOff = stops.front().offset;
                    float period = stops.back().offset - firstOff;
                    if (period > 1e-4f && (firstOff > 1e-4f || period < 0.999f)) {
                        std::vector<render::ColorStop> tiled;
                        for (float base = firstOff;
                             base < 1.0f + period && tiled.size() < 8192;
                             base += period) {
                            bool done = false;
                            for (const auto& s : stops) {
                                float off = base + (s.offset - firstOff);
                                if (off < -1e-4f) continue;
                                if (off >= 1.0f) { tiled.push_back({1.0f, s.color}); done = true; break; }
                                tiled.push_back({off, s.color});
                            }
                            if (done) break;
                        }
                        if (tiled.size() >= 2) stops.swap(tiled);
                    }
                }

                if (stops.size() >= 2) {
                    // Compute per-cell box from background-size + position +
                    // repeat. Gradient is drawn into this cell, tiled per
                    // background-repeat.
                    float cellW = w, cellH = h;
                    {
                        const auto& bs = layerSize;
                        if (!bs.empty() && bs != "auto" && bs != "cover" && bs != "contain") {
                            std::istringstream iss(bs);
                            std::string ws, hs;
                            iss >> ws; iss >> hs;
                            if (!ws.empty() && ws != "auto") cellW = parseLengthPx(ws, w);
                            if (!hs.empty() && hs != "auto") cellH = parseLengthPx(hs, h);
                            else if (!ws.empty() && ws != "auto") cellH = cellW;
                        }
                    }
                    if (cellW < 0.5f) cellW = 0.5f;
                    if (cellH < 0.5f) cellH = 0.5f;

                    float originX = x, originY = y;
                    if (!layerPosition.empty() && layerPosition != "0% 0%") {
                        const auto& bp = layerPosition;
                        if (bp == "center") {
                            originX = x + (w - cellW) / 2;
                            originY = y + (h - cellH) / 2;
                        } else {
                            std::istringstream iss(bp);
                            std::string ps1, ps2;
                            iss >> ps1; iss >> ps2;
                            auto resolvePos = [&](const std::string& tok, bool isX, float box, float cell) -> float {
                                if (tok == "left") return 0;
                                if (tok == "right") return box - cell;
                                if (tok == "top") return 0;
                                if (tok == "bottom") return box - cell;
                                if (tok == "center") return (box - cell) / 2;
                                if (!tok.empty() && tok.back() == '%') {
                                    float p = std::strtof(tok.c_str(), nullptr) / 100.0f;
                                    return p * (box - cell);
                                }
                                return parseLengthPx(tok, box);
                            };
                            if (!ps1.empty()) originX = x + resolvePos(ps1, true,  w, cellW);
                            if (!ps2.empty()) originY = y + resolvePos(ps2, false, h, cellH);
                            else originY = y + resolvePos(ps1, false, h, cellH);
                        }
                    }

                    bool tileX = (layerRepeat == "repeat" || layerRepeat == "repeat-x");
                    bool tileY = (layerRepeat == "repeat" || layerRepeat == "repeat-y");
                    bool needTile = tileX || tileY;
                    bool needClip = needTile || cellW < w || cellH < h ||
                                    originX > x || originY > y ||
                                    originX + cellW < x + w || originY + cellH < y + h;

                    // Build the tile list (for repeat, populate spans across the box).
                    struct Cell { float gx, gy, gw, gh; };
                    std::vector<Cell> cells;
                    if (needTile) {
                        float startX = tileX ? x - std::fmod(originX - x, cellW) - cellW : originX;
                        float startY = tileY ? y - std::fmod(originY - y, cellH) - cellH : originY;
                        float endX = tileX ? x + w : originX + cellW;
                        float endY = tileY ? y + h : originY + cellH;
                        for (float iy = startY; iy < endY; iy += cellH)
                            for (float ix = startX; ix < endX; ix += cellW)
                                cells.push_back({ix, iy, cellW, cellH});
                    } else {
                        cells.push_back({originX, originY, cellW, cellH});
                    }

                    if (needClip) {
                        renderer_->save();
                        renderer_->setClip(x, y, w, h);
                    }

                    for (const auto& cl : cells) {
                    float gx = cl.gx, gy = cl.gy, gw = cl.gw, gh = cl.gh;
                    if (val.find("linear-gradient") != std::string::npos) {
                        // CSS linear-gradient: gradient line passes through the
                        // center, with length = |W·sin(angle)| + |H·cos(angle)|.
                        // Direction is (sin(angle), -cos(angle)) so that
                        // 0deg = up, 90deg = right (CSS convention).
                        // Endpoints are in canvas space, so add the box origin.
                        float rad = angleDeg * 3.14159265f / 180.0f;
                        float sa = std::sin(rad), ca = -std::cos(rad);
                        float cx2 = gx + gw / 2, cy2 = gy + gh / 2;
                        float lineLen = std::abs(gw * std::sin(rad)) +
                                        std::abs(gh * std::cos(rad));
                        float dx = sa * lineLen / 2.0f;
                        float dy = ca * lineLen / 2.0f;
                        renderer_->fillLinearGradient(gx, gy, gw, gh,
                            cx2 - dx, cy2 - dy, cx2 + dx, cy2 + dy, stops);
                    } else if (isRadial) {
                        float rcx = radCxFrac * gw;
                        float rcy = radCyFrac * gh;
                        // Distances to each side from center.
                        float dL = rcx, dR = gw - rcx;
                        float dT = rcy, dB = gh - rcy;
                        float closestSideX = std::min(dL, dR);
                        float closestSideY = std::min(dT, dB);
                        float farthestSideX = std::max(dL, dR);
                        float farthestSideY = std::max(dT, dB);
                        float rx = 0, ry = 0;
                        if (radialIsCircle) {
                            // Circle: pick a single radius based on distances.
                            switch (radExtent) {
                                case RAD_CLOSEST_SIDE:
                                    rx = ry = std::min(closestSideX, closestSideY); break;
                                case RAD_CLOSEST_CORNER:
                                    rx = ry = std::sqrt(closestSideX*closestSideX +
                                                        closestSideY*closestSideY); break;
                                case RAD_FARTHEST_SIDE:
                                    rx = ry = std::max(farthestSideX, farthestSideY); break;
                                case RAD_FARTHEST_CORNER:
                                default:
                                    rx = ry = std::sqrt(farthestSideX*farthestSideX +
                                                        farthestSideY*farthestSideY); break;
                            }
                        } else {
                            // Ellipse: rx, ry computed independently per CSS spec.
                            switch (radExtent) {
                                case RAD_CLOSEST_SIDE:
                                    rx = closestSideX; ry = closestSideY; break;
                                case RAD_FARTHEST_SIDE:
                                    rx = farthestSideX; ry = farthestSideY; break;
                                case RAD_CLOSEST_CORNER: {
                                    // Ellipse with same aspect as closest-side, passing
                                    // through closest corner.
                                    float k = std::sqrt(2.0f);
                                    rx = closestSideX * k; ry = closestSideY * k; break;
                                }
                                case RAD_FARTHEST_CORNER:
                                default: {
                                    float k = std::sqrt(2.0f);
                                    rx = farthestSideX * k; ry = farthestSideY * k; break;
                                }
                            }
                        }
                        if (rx < 0.001f) rx = 0.001f;
                        if (ry < 0.001f) ry = 0.001f;
                        renderer_->fillRadialGradient(gx, gy, gw, gh,
                            gx + rcx, gy + rcy, rx, ry, stops);
                    } else if (val.find("conic-gradient") != std::string::npos) {
                        renderer_->fillConicGradient(gx, gy, gw, gh,
                            gx + gw/2, gy + gh/2, conicFromDeg, stops);
                    }
                    } // per-cell loop
                    if (needClip) renderer_->restore();
                }
            }
        }
        if (layerBlended) renderer_->restore();
        } // for layer
    }

    if (clipped) renderer_->restore();
}

} // namespace bro::layout
