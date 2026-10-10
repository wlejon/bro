// CSS border-image (Backgrounds-3 §6): the nine-slice of a raster image over
// the border area, with slice / width / outset / repeat.

#include "layout/draw_traversal_internal.h"
#include "render/image_store.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace bro::layout {

bool DrawTraversal::drawBorderImage(const PaintBox& pb, float x, float y, float w, float h) {
    auto& style = pb.style;
    auto srcIt = style.find("border-image-source");
    if (srcIt == style.end() || srcIt->second.empty() || srcIt->second == "none")
        return false;
    const std::string& src = srcIt->second;
    // Only url() sources are supported; gradients fall back to normal borders.
    if (src.compare(0, 4, "url(") != 0) return false;
    size_t uStart = src.find('(') + 1;
    size_t uEnd = src.rfind(')');
    if (uEnd == std::string::npos || uEnd <= uStart) return false;
    std::string url = src.substr(uStart, uEnd - uStart);
    if (!url.empty() && (url.front() == '"' || url.front() == '\''))
        url = url.substr(1, url.size() - 2);
    if (url.empty()) return false;

    // Missing/broken source, one still decoding (it repaints when it lands),
    // or an SVG without an intrinsic raster: normal border painting takes
    // over. An SVG with one slices its raster.
    auto pic = paintImage(url, styleImageOriented(style));
    if (!pic || pic->rgba.empty()) return false;
    const float imgW = static_cast<float>(pic->width);
    const float imgH = static_cast<float>(pic->height);
    if (imgW <= 0 || imgH <= 0) return false;
    const render::SharedPixels pixels = render::sharedPixelsOf(pic);
    const render::ImageSampling sampling = styleImageSampling(style);

    auto& box = pb.box;
    // Computed border widths — the reference for number-valued
    // border-image-width and border-image-outset.
    const float cbw[4] = {box.border.top, box.border.right,
                          box.border.bottom, box.border.left};

    auto tokenize = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    // 1-4 value box expansion into [top, right, bottom, left].
    auto boxOrder = [](const std::vector<std::string>& t) {
        std::array<std::string, 4> o;
        size_t n = t.size();
        if (n == 0) return o;
        o[0] = t[0];
        o[1] = n > 1 ? t[1] : t[0];
        o[2] = n > 2 ? t[2] : t[0];
        o[3] = n > 3 ? t[3] : o[1];
        return o;
    };

    // ---- border-image-slice: number = px in the SOURCE image, % of the
    // image dimension; optional `fill`. Clamped to the image bounds. --------
    bool fill = false;
    float slice[4];  // top, right, bottom, left (image px)
    {
        std::vector<std::string> toks;
        if (auto it = style.find("border-image-slice"); it != style.end())
            toks = tokenize(it->second);
        for (auto tIt = toks.begin(); tIt != toks.end();) {
            if (*tIt == "fill") { fill = true; tIt = toks.erase(tIt); }
            else ++tIt;
        }
        auto o = boxOrder(toks);
        for (int i = 0; i < 4; ++i) {
            float dim = (i == 0 || i == 2) ? imgH : imgW;  // top/bottom vs left/right
            if (o[i].empty()) { slice[i] = dim; continue; }  // initial 100%
            char* end = nullptr;
            float v = std::strtof(o[i].c_str(), &end);
            if (end == o[i].c_str()) { slice[i] = dim; continue; }
            if (end && *end == '%') v = v * dim / 100.0f;
            slice[i] = std::clamp(v, 0.0f, dim);
        }
    }

    // ---- border-image-outset: number = multiples of the computed
    // border-width, length = px. Expands the paint area outside the border
    // box (paint-only: layout and hit-testing are unaffected). -------------
    float outset[4] = {0, 0, 0, 0};
    {
        if (auto it = style.find("border-image-outset"); it != style.end()) {
            auto o = boxOrder(tokenize(it->second));
            for (int i = 0; i < 4; ++i) {
                if (o[i].empty()) continue;
                char* end = nullptr;
                float v = std::strtof(o[i].c_str(), &end);
                if (end == o[i].c_str()) continue;
                bool bare = (end == nullptr || *end == '\0');
                outset[i] = std::max(0.0f, bare ? v * cbw[i] : v);
            }
        }
    }

    // Border image area: the border box expanded by the outsets.
    const float ax = x - outset[3];
    const float ay = y - outset[0];
    const float aw = w + outset[3] + outset[1];
    const float ah = h + outset[0] + outset[2];
    if (aw <= 0 || ah <= 0) return true;  // nothing to paint, but bi is active

    // ---- border-image-width: number = multiples of the computed
    // border-width, length = px, % of the border image area (horizontal
    // sides against its width, vertical against its height), auto = the
    // intrinsic slice size. ------------------------------------------------
    float bw4[4];  // top, right, bottom, left
    {
        std::vector<std::string> toks;
        if (auto it = style.find("border-image-width"); it != style.end())
            toks = tokenize(it->second);
        auto o = boxOrder(toks);
        for (int i = 0; i < 4; ++i) {
            float areaRef = (i == 0 || i == 2) ? ah : aw;
            if (o[i].empty()) { bw4[i] = cbw[i]; continue; }  // initial 1
            if (o[i] == "auto") { bw4[i] = slice[i]; continue; }
            char* end = nullptr;
            float v = std::strtof(o[i].c_str(), &end);
            if (end == o[i].c_str()) { bw4[i] = cbw[i]; continue; }
            if (end && *end == '%') bw4[i] = v * areaRef / 100.0f;
            else if (end == nullptr || *end == '\0') bw4[i] = v * cbw[i];  // number
            else bw4[i] = v;  // length (px)
            bw4[i] = std::max(0.0f, bw4[i]);
        }
        // Proportional reduction when opposite widths together exceed the
        // border image area (CSS Backgrounds-3 §6.3).
        float f = 1.0f;
        if (bw4[3] + bw4[1] > aw && bw4[3] + bw4[1] > 0)
            f = std::min(f, aw / (bw4[3] + bw4[1]));
        if (bw4[0] + bw4[2] > ah && bw4[0] + bw4[2] > 0)
            f = std::min(f, ah / (bw4[0] + bw4[2]));
        for (float& v : bw4) v *= f;
    }
    const float wT = bw4[0], wR = bw4[1], wB = bw4[2], wL = bw4[3];

    // ---- border-image-repeat: 1-2 of stretch | repeat | round | space
    // (first horizontal — top/bottom edges + middle x — second vertical). --
    enum { kStretch = 0, kRepeat, kRound, kSpace };
    int repH = kStretch, repV = kStretch;
    {
        auto parseMode = [](const std::string& s) {
            if (s == "repeat") return (int)kRepeat;
            if (s == "round") return (int)kRound;
            if (s == "space") return (int)kSpace;
            return (int)kStretch;
        };
        if (auto it = style.find("border-image-repeat"); it != style.end()) {
            auto toks = tokenize(it->second);
            if (!toks.empty()) repH = parseMode(toks[0]);
            repV = toks.size() > 1 ? parseMode(toks[1]) : repH;
        }
    }

    // Source slice geometry (image px).
    const float sT = slice[0], sR = slice[1], sB = slice[2], sL = slice[3];
    const float midW = imgW - sL - sR;  // <= 0 → top/bottom edges + middle empty
    const float midH = imgH - sT - sB;  // <= 0 → left/right edges + middle empty

    // Draw the source sub-rect (sx, sy, sw, sh) into the dest rect
    // (dx, dy, dw, dh). A sub-rect is sampled strictly inside itself, so no
    // neighbouring-slice texels bleed in, smoothed or not.
    auto drawRegion = [&](float sx, float sy, float sw, float sh,
                          float dx, float dy, float dw, float dh) {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        renderer_->drawSharedPixels(pixels, sx, sy, sw, sh, dx, dy, dw, dh, sampling);
    };

    // Tile positions along one axis for a repeat mode. `ideal` is the tile
    // length the source region maps to at the edge's cross-axis scale.
    struct Tile { float pos, len; };
    auto axisTiles = [](float start, float len, float ideal, int mode) {
        std::vector<Tile> out;
        if (len <= 0) return out;
        if (mode == kStretch || ideal <= 0.01f) {
            out.push_back({start, len});
            return out;
        }
        if (mode == kRound) {
            // Scale tiles so a whole number fits exactly.
            int n = std::max(1, (int)std::lround(len / ideal));
            n = std::min(n, 4096);
            float t = len / n;
            for (int i = 0; i < n; ++i) out.push_back({start + i * t, t});
            return out;
        }
        if (mode == kSpace) {
            // Whole tiles at their ideal size, leftover distributed as equal
            // gaps around them. Less than one tile fits → nothing painted.
            int n = (int)std::floor(len / ideal + 1e-4f);
            if (n < 1) return out;
            n = std::min(n, 4096);
            float gap = (len - n * ideal) / (n + 1);
            for (int i = 0; i < n; ++i)
                out.push_back({start + gap + i * (ideal + gap), ideal});
            return out;
        }
        // kRepeat: ideal-size tiles with the pattern centered in the area
        // (a tile centered on the midpoint), extended to cover it; the
        // enclosing clip trims the partial tiles at both ends.
        float mid = start + len * 0.5f - ideal * 0.5f;
        float first = mid - std::ceil((mid - start) / ideal) * ideal;
        int n = (int)std::ceil((start + len - first) / ideal);
        n = std::clamp(n, 1, 4096);
        for (int i = 0; i < n; ++i) out.push_back({first + i * ideal, ideal});
        return out;
    };

    auto tileRegion = [&](float sx, float sy, float sw, float sh,
                          float dx, float dy, float dw, float dh,
                          int modeH, int modeV, float idealW, float idealH) {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        auto hs = axisTiles(dx, dw, idealW, modeH);
        auto vs = axisTiles(dy, dh, idealH, modeV);
        if (hs.empty() || vs.empty()) return;
        renderer_->save();
        renderer_->setClip(dx, dy, dw, dh);
        for (auto& tv : vs)
            for (auto& th : hs)
                drawRegion(sx, sy, sw, sh, th.pos, tv.pos, th.len, tv.len);
        renderer_->restore();
    };

    // Corners: drawn 1:1 into their border-image-width boxes (stretched).
    drawRegion(0, 0, sL, sT,                    ax, ay, wL, wT);                    // TL
    drawRegion(imgW - sR, 0, sR, sT,            ax + aw - wR, ay, wR, wT);          // TR
    drawRegion(imgW - sR, imgH - sB, sR, sB,    ax + aw - wR, ay + ah - wB, wR, wB);// BR
    drawRegion(0, imgH - sB, sL, sB,            ax, ay + ah - wB, wL, wB);          // BL

    // Edges: cross axis stretched to the border-image width, main axis tiled
    // per the repeat mode. The ideal tile length preserves the slice's aspect
    // ratio at the cross-axis scale.
    const float edgeW = aw - wL - wR;  // horizontal edges' dest length
    const float edgeH = ah - wT - wB;  // vertical edges' dest length
    if (midW > 0) {
        if (sT > 0 && wT > 0)
            tileRegion(sL, 0, midW, sT, ax + wL, ay, edgeW, wT,
                       repH, kStretch, midW * wT / sT, wT);
        if (sB > 0 && wB > 0)
            tileRegion(sL, imgH - sB, midW, sB, ax + wL, ay + ah - wB, edgeW, wB,
                       repH, kStretch, midW * wB / sB, wB);
    }
    if (midH > 0) {
        if (sL > 0 && wL > 0)
            tileRegion(0, sT, sL, midH, ax, ay + wT, wL, edgeH,
                       kStretch, repV, wL, midH * wL / sL);
        if (sR > 0 && wR > 0)
            tileRegion(imgW - sR, sT, sR, midH, ax + aw - wR, ay + wT, wR, edgeH,
                       kStretch, repV, wR, midH * wR / sR);
    }

    // Middle: only with `fill`. Tile size follows the top edge's horizontal
    // scale and the left edge's vertical scale (falling back to the opposite
    // side, then 1:1) so the middle pattern lines up with the edges.
    if (fill && midW > 0 && midH > 0) {
        float hScale = sT > 0 && wT > 0 ? wT / sT : (sB > 0 && wB > 0 ? wB / sB : 1.0f);
        float vScale = sL > 0 && wL > 0 ? wL / sL : (sR > 0 && wR > 0 ? wR / sR : 1.0f);
        tileRegion(sL, sT, midW, midH, ax + wL, ay + wT, edgeW, edgeH,
                   repH, repV, midW * hScale, midH * vScale);
    }

    return true;
}

} // namespace bro::layout
