#include "engine/hit_region.h"

#include <algorithm>
#include <cmath>

namespace bro::engine {

namespace {

// Sorted, de-duplicated edges inside [0, limit], with both ends.
void normalizeEdges(std::vector<float>& edges, float limit) {
    edges.push_back(0.0f);
    edges.push_back(limit);
    for (float& e : edges) e = std::clamp(e, 0.0f, limit);
    std::sort(edges.begin(), edges.end());
    std::vector<float> out;
    out.reserve(edges.size());
    for (float e : edges)
        if (out.empty() || e - out.back() > 1e-3f) out.push_back(e);
    edges = std::move(out);
}

struct Span {
    float x0, x1;
};

}  // namespace

bool computeHitRegion(int width, int height, std::vector<float> xEdges, std::vector<float> yEdges,
                      const std::function<bool(float x, float y)>& takesInput, std::vector<platform::WindowRect>& out,
                      size_t maxCells, const std::vector<platform::WindowRect>* candidates) {
    out.clear();
    if (width <= 0 || height <= 0) return true;
    normalizeEdges(xEdges, static_cast<float>(width));
    normalizeEdges(yEdges, static_cast<float>(height));
    const size_t nx = xEdges.size() - 1, ny = yEdges.size() - 1;
    if (nx * ny > maxCells) return false;

    // A rect still growing downward: its span, and where it began.
    struct Open {
        Span span;
        float top;
    };
    std::vector<Open> open, next;
    std::vector<Span> spans;
    auto close = [&](const Open& o, float bottom) {
        platform::WindowRect r;
        r.x = static_cast<int>(std::floor(o.span.x0));
        r.y = static_cast<int>(std::floor(o.top));
        r.w = static_cast<int>(std::ceil(o.span.x1)) - r.x;
        r.h = static_cast<int>(std::ceil(bottom)) - r.y;
        if (r.w > 0 && r.h > 0) out.push_back(r);
    };

    for (size_t j = 0; j < ny; ++j) {
        const float y0 = yEdges[j], y1 = yEdges[j + 1];
        const float cy = (y0 + y1) * 0.5f;
        // This band's runs of cells that take input.
        spans.clear();
        for (size_t i = 0; i < nx; ++i) {
            const float x0 = xEdges[i], x1 = xEdges[i + 1];
            const float cx = (x0 + x1) * 0.5f;
            if (candidates && !regionContains(*candidates, cx, cy)) continue;
            if (!takesInput(cx, cy)) continue;
            if (!spans.empty() && spans.back().x1 == x0) spans.back().x1 = x1;
            else spans.push_back({x0, x1});
        }
        // A span the band above had too carries its rect down; the rest close.
        next.clear();
        for (const Span& s : spans) {
            auto it = std::find_if(open.begin(), open.end(),
                                   [&](const Open& o) { return o.span.x0 == s.x0 && o.span.x1 == s.x1; });
            if (it != open.end()) {
                next.push_back(*it);
                it->span.x1 = it->span.x0;  // taken
            } else {
                next.push_back({s, y0});
            }
        }
        for (const Open& o : open)
            if (o.span.x1 != o.span.x0) close(o, y0);
        open.swap(next);
    }
    for (const Open& o : open) close(o, static_cast<float>(height));
    return true;
}

bool regionContains(const std::vector<platform::WindowRect>& rects, float x, float y) {
    for (const auto& r : rects)
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return true;
    return false;
}

}  // namespace bro::engine
