#include "dom/element_geometry.h"
#include "dom/element.h"
#include <algorithm>
#include <vector>

namespace bro::dom {

using htmlayout::css::Matrix3D;

namespace {

struct Raw {
    const Element* el;
    float cx, cy;         // contentRect origin in parent's content-area coords
    float padL, padT, borL, borT;
    float fullW, fullH;
    float scrollY;         // scrollTop this element applies to its children
};

struct Frame {
    const Element* el;
    float bx, by, bw, bh; // absolute border-box top-left + size (pre-transform)
};

} // namespace

AbsoluteFrame computeAbsoluteFrame(const Element* el) {
    AbsoluteFrame result;
    if (!el) return result;

    // Walk up collecting raw layout-box info (composed/layoutParent() chain,
    // so shadow-DOM slot wrappers are handled correctly), then convert to
    // absolute coords root-down.
    std::vector<Raw> raws;
    for (const Element* lp = el; lp; lp = lp->layoutParent()) {
        auto& lb = lp->layoutBox();
        // Clamp the scroll offset to the current layout's scrollable extent, the
        // same clamp the draw traversal applies when painting HTML. scrollTop_
        // can outrun the max after content shrinks (e.g. a fold collapses) with
        // no scroll interaction to re-clamp it; using the raw value here would
        // shift separately-composited canvas/WebGL/iframe quads by more scroll
        // than the HTML flow moved, leaving them stuck out of place until the
        // next scroll. Clamping keeps quad geometry consistent with the paint.
        float maxST = std::max(0.0f, lb.naturalHeight - lb.contentRect.height);
        float st = std::clamp(lp->scrollTopValue(), 0.0f, maxST);
        raws.push_back({lp, lb.contentRect.x, lb.contentRect.y,
                        lb.padding.left, lb.padding.top,
                        lb.border.left, lb.border.top,
                        lb.fullWidth(), lb.fullHeight(),
                        st});
    }

    // Accumulate root-down: parent's content-area origin in absolute coords is
    // (accX, accY); element border-box = (accX + cx - padL - borL, ...).
    std::vector<Frame> chain(raws.size());
    {
        float accX = 0.0f, accY = 0.0f;
        for (int i = static_cast<int>(raws.size()) - 1; i >= 0; --i) {
            const Raw& r = raws[i];
            float bx = accX + r.cx - r.padL - r.borL;
            float by = accY + r.cy - r.padT - r.borT;
            chain[i] = {r.el, bx, by, r.fullW, r.fullH};
            accX += r.cx;
            accY += r.cy - r.scrollY;
        }
    }

    // el's own untransformed content-box origin = its border-box origin
    // (chain[0]) plus its own border + padding.
    const Raw& self = raws[0];
    result.ox = chain[0].bx + self.borL + self.padL;
    result.oy = chain[0].by + self.borT + self.padT;

    // Apply transforms inside-out: combined = T_root * T_parent * ... * T_self.
    // For each element with a transform, the matrix acts about its own border
    // box in absolute coords: full = T(bx+ox, by+oy) * M * T(-(bx+ox), -(by+oy)).
    // Compose by multiplying each ancestor's full transform on the LEFT (since
    // ancestor transforms apply to the already-transformed descendant point).
    // Compose ancestor transforms as a 4x4 (covers 3D + perspective). For the
    // common all-2D case the result reduces to a 2D affine and we still take
    // a fast path below.
    for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
        const Frame& f = chain[i];
        if (!f.el) continue;
        auto& cs = f.el->computedStyle();

        // Ancestor perspective on this element's parent.
        float persp = 0.0f;
        const htmlayout::css::ComputedStyle* perspStyle = nullptr;
        float pbx = 0, pby = 0, pbw = 0, pbh = 0;
        if (auto* parent = f.el->layoutParent()) {
            auto& pcs = parent->computedStyle();
            auto pit = pcs.find("perspective");
            if (pit != pcs.end()) persp = htmlayout::css::parsePerspective(pit->second);
            if (persp > 0) {
                perspStyle = &pcs;
                // Find parent's frame in the chain to get its bx/by/bw/bh.
                for (int j = i + 1; j < static_cast<int>(chain.size()); ++j) {
                    if (chain[j].el == parent) {
                        pbx = chain[j].bx; pby = chain[j].by;
                        pbw = chain[j].bw; pbh = chain[j].bh;
                        break;
                    }
                }
            }
        }

        auto trIt = cs.find("transform");
        bool hasT = (trIt != cs.end() && !trIt->second.empty()
                     && trIt->second != "none");
        if (!hasT && persp <= 0) continue;

        Matrix3D mat;
        if (hasT) mat = htmlayout::css::parseTransform3D(trIt->second, f.bw, f.bh);
        if (mat.isIdentity() && persp <= 0) continue;

        float ox = f.bw * 0.5f, oy = f.bh * 0.5f, oz = 0.0f;
        auto toIt = cs.find("transform-origin");
        std::string_view originVal = (toIt != cs.end())
            ? std::string_view(toIt->second) : std::string_view();
        htmlayout::css::parseTransformOrigin3D(originVal, f.bw, f.bh, ox, oy, oz);

        Matrix3D toOrigin;
        toOrigin.m[12] = f.bx + ox; toOrigin.m[13] = f.by + oy; toOrigin.m[14] = oz;
        Matrix3D fromOrigin;
        fromOrigin.m[12] = -(f.bx + ox); fromOrigin.m[13] = -(f.by + oy); fromOrigin.m[14] = -oz;
        Matrix3D full = toOrigin * mat * fromOrigin;

        if (persp > 0 && perspStyle) {
            float pox = pbw * 0.5f, poy = pbh * 0.5f;
            auto poIt = perspStyle->find("perspective-origin");
            std::string_view poVal = (poIt != perspStyle->end())
                ? std::string_view(poIt->second) : std::string_view();
            htmlayout::css::parseTransformOrigin(poVal, pbw, pbh, pox, poy);
            float ax = pbx + pox, ay = pby + poy;
            Matrix3D persp_m = htmlayout::css::makePerspectiveMatrix(persp);
            Matrix3D toPO;     toPO.m[12] = ax; toPO.m[13] = ay;
            Matrix3D fromPO;   fromPO.m[12] = -ax; fromPO.m[13] = -ay;
            Matrix3D P = toPO * persp_m * fromPO;
            full = P * full;
        }

        result.transform = result.transform * full;
        result.hasTransform = true;
    }

    return result;
}

AbsolutePoint absoluteContentOrigin(const Element* el) {
    AbsoluteFrame frame = computeAbsoluteFrame(el);
    if (!frame.hasTransform) return {frame.ox, frame.oy};
    AbsolutePoint out;
    frame.transform.project2D(frame.ox, frame.oy, 0.0f, out.x, out.y);
    return out;
}

namespace {

// Project the four corners of a pre-transform rect through `frame` and
// return their axis-aligned bounding box (identity fast path when the
// ancestor chain has no transform).
AbsoluteRect projectAABB(const AbsoluteFrame& frame, float x, float y, float w, float h) {
    if (!frame.hasTransform) return {x, y, w, h};
    float cx[4], cy[4];
    frame.transform.project2D(x,     y,     0.0f, cx[0], cy[0]);
    frame.transform.project2D(x + w, y,     0.0f, cx[1], cy[1]);
    frame.transform.project2D(x + w, y + h, 0.0f, cx[2], cy[2]);
    frame.transform.project2D(x,     y + h, 0.0f, cx[3], cy[3]);
    float minX = cx[0], maxX = cx[0], minY = cy[0], maxY = cy[0];
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, cx[i]); maxX = std::max(maxX, cx[i]);
        minY = std::min(minY, cy[i]); maxY = std::max(maxY, cy[i]);
    }
    return {minX, minY, maxX - minX, maxY - minY};
}

} // namespace

AbsoluteRect absoluteBorderBox(const Element* el) {
    if (!el) return {};
    auto& cs = el->computedStyle();
    auto dIt = cs.find("display");
    if (dIt != cs.end() && dIt->second == "none") return {};

    AbsoluteFrame frame = computeAbsoluteFrame(el);
    auto& box = el->layoutBox();
    float bx = frame.ox - box.padding.left - box.border.left;
    float by = frame.oy - box.padding.top - box.border.top;
    return projectAABB(frame, bx, by, box.fullWidth(), box.fullHeight());
}

AbsoluteRect absoluteContentBox(const Element* el) {
    if (!el) return {};
    AbsoluteFrame frame = computeAbsoluteFrame(el);
    auto& box = el->layoutBox();
    return projectAABB(frame, frame.ox, frame.oy, box.contentRect.width, box.contentRect.height);
}

AbsoluteRect projectRectThroughAncestors(const Element* el, float x, float y, float w, float h) {
    if (!el) return {x, y, w, h};
    return projectAABB(computeAbsoluteFrame(el), x, y, w, h);
}

} // namespace bro::dom
