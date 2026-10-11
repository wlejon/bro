// The damage between two recordings (render/damage.h).

#include "render/damage.h"
#include "render/draw_command.h"

#include <include/core/SkM44.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkRect.h>
#include <include/core/SkTextBlob.h>
#include <include/utils/SkParsePath.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace bro::render {

namespace {

// Antialiasing fringe around every bound.
constexpr float kPad = 2.0f;

bool eqMem(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) == 0; }

bool eqColor(const bromath::Color& a, const bromath::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// `count` elements of `elemSize` bytes at each buffer's offset.
bool eqArena(const CommandBuffer& A, uint32_t ao, uint32_t an, const CommandBuffer& B, uint32_t bo, uint32_t bn,
             size_t elemSize) {
    if (an != bn) return false;
    if (an == 0) return true;
    return eqMem(A.bytesAt(ao), B.bytesAt(bo), an * elemSize);
}

bool eqGradient(const GradientPaint& a, const GradientPaint& b) {
    return a.kind == b.kind && a.spread == b.spread && eqColor(a.color, b.color) && eqMem(a.p0, b.p0, sizeof a.p0) &&
           eqMem(a.p1, b.p1, sizeof a.p1) && eqMem(a.center, b.center, sizeof a.center) && a.radius == b.radius &&
           eqMem(a.focal, b.focal, sizeof a.focal) && a.hasFocal == b.hasFocal &&
           eqMem(a.transform, b.transform, sizeof a.transform);
}

bool eqLayerSource(const LayerSource& a, const LayerSource& b) {
    if (a.index() != b.index()) return false;
    return std::visit(
        [&](const auto& x) -> bool {
            using T = std::decay_t<decltype(x)>;
            const T& y = std::get<T>(b);
            if constexpr (std::is_same_v<T, CanvasLayerSource>) return x.sceneId == y.sceneId;
            else if constexpr (std::is_same_v<T, WebGLLayerSource>) return x.elementId == y.elementId;
            else if constexpr (std::is_same_v<T, SceneLayerSource>) return x.elementId == y.elementId;
            else if constexpr (std::is_same_v<T, IframeLayerSource>) return x.docId == y.docId;
            else if constexpr (std::is_same_v<T, TerminalLayerSource>) return x.layerId == y.layerId;
            else if constexpr (std::is_same_v<T, RemoteViewLayerSource>) return x.viewId == y.viewId;
            else if constexpr (std::is_same_v<T, ScrollLayerSource>)
                return x.elementId == y.elementId && x.pass == y.pass && x.part == y.part;
            else if constexpr (std::is_same_v<T, ClientWindowsLayerSource>)
                return x.list == y.list && x.first == y.first && x.count == y.count && x.parts == y.parts;
            else return false;  // a dmabuf: a new buffer every frame
        },
        a);
}

// Whether two commands, each read against its own buffer, draw the same.
bool sameCommand(const CommandBuffer& A, const DrawCommand& a, const CommandBuffer& B, const DrawCommand& b) {
    if (a.index() != b.index()) return false;
    return std::visit(
        [&](const auto& x) -> bool {
            using T = std::decay_t<decltype(x)>;
            const T& y = std::get<T>(b);
            if constexpr (std::is_same_v<T, Cmd_DrawBoxShadow> || std::is_same_v<T, Cmd_DrawBoxShadowRadii>) {
                return eqMem(&x, &y, offsetof(T, inset)) && x.inset == y.inset;
            } else if constexpr (std::is_same_v<T, Cmd_DrawText>) {
                return x.x == y.x && x.y == y.y && x.fontSize == y.fontSize && x.fontWeight == y.fontWeight &&
                       x.fontItalic == y.fontItalic && x.fontLigatures == y.fontLigatures &&
                       x.letterSpacing == y.letterSpacing && x.wordSpacing == y.wordSpacing && x.blur == y.blur &&
                       eqColor(x.color, y.color) &&
                       (x.blobIndex == CommandBuffer::kNoTextBlob) == (y.blobIndex == CommandBuffer::kNoTextBlob) &&
                       eqArena(A, x.textOffset, x.textLen, B, y.textOffset, y.textLen, 1) &&
                       eqArena(A, x.familyOffset, x.familyLen, B, y.familyOffset, y.familyLen, 1) &&
                       eqArena(A, x.featuresOffset, x.featuresLen, B, y.featuresOffset, y.featuresLen, 1);
            } else if constexpr (std::is_same_v<T, Cmd_DrawImage>) {
                return x.x == y.x && x.y == y.y && x.w == y.w && x.h == y.h && x.imageId == y.imageId &&
                       (x.imageId != 0 || eqArena(A, x.dataOffset, x.dataLen, B, y.dataOffset, y.dataLen, 1));
            } else if constexpr (std::is_same_v<T, Cmd_DrawPixelsRGBA>) {
                return x.srcW == y.srcW && x.srcH == y.srcH && x.stride == y.stride && x.x == y.x && x.y == y.y &&
                       x.w == y.w && x.h == y.h &&
                       eqMem(A.bytesAt(x.pixelsOffset), B.bytesAt(y.pixelsOffset),
                             static_cast<size_t>(x.srcH) * static_cast<size_t>(x.stride));
            } else if constexpr (std::is_same_v<T, Cmd_DrawSvgMarkup>) {
                return x.x == y.x && x.y == y.y && x.w == y.w && x.h == y.h &&
                       eqArena(A, x.dataOffset, x.dataLen, B, y.dataOffset, y.dataLen, 1);
            } else if constexpr (std::is_same_v<T, Cmd_DrawSharedPixels>) {
                const SharedPixels* pa = A.sharedPixelsAt(x.index);
                const SharedPixels* pb = B.sharedPixelsAt(y.index);
                return pa && pb && pa->id == pb->id && pa->rgba == pb->rgba && x.sx == y.sx && x.sy == y.sy &&
                       x.sw == y.sw && x.sh == y.sh && x.x == y.x && x.y == y.y && x.w == y.w && x.h == y.h &&
                       x.sampling == y.sampling;
            } else if constexpr (std::is_same_v<T, Cmd_DrawPath>) {
                return eqColor(x.fill, y.fill) && eqColor(x.stroke, y.stroke) && x.strokeWidth == y.strokeWidth &&
                       eqArena(A, x.pathOffset, x.pathLen, B, y.pathOffset, y.pathLen, 1);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPolygon>) {
                return eqColor(x.fill, y.fill) && eqColor(x.stroke, y.stroke) && x.strokeWidth == y.strokeWidth &&
                       eqArena(A, x.pointsOffset, x.pointsLen, B, y.pointsOffset, y.pointsLen, sizeof(PointF));
            } else if constexpr (std::is_same_v<T, Cmd_DrawPolyline>) {
                return eqColor(x.stroke, y.stroke) && x.strokeWidth == y.strokeWidth &&
                       eqArena(A, x.pointsOffset, x.pointsLen, B, y.pointsOffset, y.pointsLen, sizeof(PointF));
            } else if constexpr (std::is_same_v<T, Cmd_DrawSvgPath>) {
                return x.rule == y.rule && eqGradient(x.fill, y.fill) && eqGradient(x.stroke, y.stroke) &&
                       x.strokeStyle.width == y.strokeStyle.width &&
                       x.strokeStyle.miterLimit == y.strokeStyle.miterLimit &&
                       x.strokeStyle.dashOffset == y.strokeStyle.dashOffset &&
                       x.strokeStyle.cap == y.strokeStyle.cap && x.strokeStyle.join == y.strokeStyle.join &&
                       eqArena(A, x.pathOffset, x.pathLen, B, y.pathOffset, y.pathLen, 1) &&
                       eqArena(A, x.fillStopsOffset, x.fillStopsLen, B, y.fillStopsOffset, y.fillStopsLen,
                               sizeof(ColorStop)) &&
                       eqArena(A, x.strokeStopsOffset, x.strokeStopsLen, B, y.strokeStopsOffset, y.strokeStopsLen,
                               sizeof(ColorStop)) &&
                       eqArena(A, x.dashArrOffset, x.dashArrLen, B, y.dashArrOffset, y.dashArrLen, sizeof(float));
            } else if constexpr (std::is_same_v<T, Cmd_ClipSvgPath>) {
                return x.rule == y.rule && eqArena(A, x.pathOffset, x.pathLen, B, y.pathOffset, y.pathLen, 1);
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerAlpha>) {
                return x.alpha == y.alpha;
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerWithBlend>) {
                return x.mode == y.mode;
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerWithFilter>) {
                return x.x == y.x && x.y == y.y && x.w == y.w && x.h == y.h &&
                       eqArena(A, x.filtersOffset, x.filtersLen, B, y.filtersOffset, y.filtersLen,
                               sizeof(CssFilterParams));
            } else if constexpr (std::is_same_v<T, Cmd_SetClipPolygon>) {
                return eqArena(A, x.pointsOffset, x.pointsLen, B, y.pointsOffset, y.pointsLen, sizeof(PointF));
            } else if constexpr (std::is_same_v<T, Cmd_FillLinearGradient> ||
                                 std::is_same_v<T, Cmd_FillRadialGradient> ||
                                 std::is_same_v<T, Cmd_FillConicGradient>) {
                return eqMem(&x, &y, offsetof(T, stopsOffset)) &&
                       eqArena(A, x.stopsOffset, x.stopsLen, B, y.stopsOffset, y.stopsLen, sizeof(ColorStop));
            } else if constexpr (std::is_same_v<T, Cmd_LayerBreak>) {
                return eqLayerSource(x.source, y.source) && eqMem(&x.quad, &y.quad, sizeof x.quad);
            } else if constexpr (std::is_same_v<T, Cmd_ReplaySpan>) {
                return A.spanAt(x.index) == B.spanAt(y.index);
            } else if constexpr (std::is_same_v<T, Cmd_DrawBackdropFilter> ||
                                 std::is_same_v<T, Cmd_BlitCanvasInline>) {
                return false;  // what they show is not in the recording
            } else if constexpr (std::is_empty_v<T>) {
                return true;  // a save, a restore, a marker: its one byte is not a value
            } else {
                // The rest are floats, ints and colours with no padding
                // (rects, radii, transforms, clips).
                return eqMem(&x, &y, sizeof(T));
            }
        },
        a);
}

// The transform a walk is at, and the filter layer it is inside (whose whole
// bounds a change inside it damages: blur and drop shadow reach out of it).
struct State {
    SkMatrix m;               // identity
    bool perspective = false;
    bool inFilter = false;
    SkRect filterBounds = SkRect::MakeEmpty();
};

struct Walker {
    State cur;
    std::vector<State> stack;

    void save() { stack.push_back(cur); }
    void restore() {
        if (stack.empty()) return;
        cur = stack.back();
        stack.pop_back();
    }
    // Apply a state command (saves, restores, transforms; clips are not
    // tracked: a bound that ignores them is only larger).
    void apply(const CommandBuffer& buf, const DrawCommand& cmd) {
        std::visit(
            [&](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, Cmd_Save> || std::is_same_v<T, Cmd_SaveLayerAlpha> ||
                              std::is_same_v<T, Cmd_SaveLayerWithBlend>) {
                    save();
                } else if constexpr (std::is_same_v<T, Cmd_SaveLayerWithFilter>) {
                    save();
                    if (!cur.inFilter) {
                        cur.inFilter = true;
                        // A blur or a drop shadow's offset can reach past the
                        // bounds the traversal gives the layer.
                        float reach = 0.0f;
                        for (const auto& f : buf.spanAt<CssFilterParams>(c.filtersOffset, c.filtersLen)) {
                            if (f.kind == CssFilterParams::Blur) reach += 3.0f * std::fabs(f.a);
                            if (f.kind == CssFilterParams::DropShadow)
                                reach += std::max(std::fabs(f.dx), std::fabs(f.dy)) + 3.0f * std::fabs(f.blur);
                        }
                        SkRect local = SkRect::MakeXYWH(c.x, c.y, c.w, c.h).makeSorted();
                        local.outset(reach, reach);
                        cur.filterBounds = cur.m.mapRect(local);
                    }
                } else if constexpr (std::is_same_v<T, Cmd_Restore>) {
                    restore();
                } else if constexpr (std::is_same_v<T, Cmd_Translate>) {
                    cur.m.preTranslate(c.dx, c.dy);
                } else if constexpr (std::is_same_v<T, Cmd_Scale>) {
                    cur.m.preScale(c.sx, c.sy);
                } else if constexpr (std::is_same_v<T, Cmd_Rotate>) {
                    cur.m.preRotate(c.degrees);
                } else if constexpr (std::is_same_v<T, Cmd_Concat>) {
                    SkMatrix t = SkMatrix::MakeAll(c.a, c.c, c.e, c.b, c.d, c.f, 0, 0, 1);
                    cur.m.preConcat(t);
                } else if constexpr (std::is_same_v<T, Cmd_Concat4x4>) {
                    const SkM44 m44 = SkM44::ColMajor(c.m);
                    const SkMatrix t = m44.asM33();
                    if (t.hasPerspective()) cur.perspective = true;
                    cur.m.preConcat(t);
                }
            },
            cmd);
    }
};

bool isStateCommand(const DrawCommand& c) {
    return std::holds_alternative<Cmd_Save>(c) || std::holds_alternative<Cmd_Restore>(c) ||
           std::holds_alternative<Cmd_SaveLayerAlpha>(c) || std::holds_alternative<Cmd_SaveLayerWithFilter>(c) ||
           std::holds_alternative<Cmd_SaveLayerWithBlend>(c) || std::holds_alternative<Cmd_Translate>(c) ||
           std::holds_alternative<Cmd_Scale>(c) || std::holds_alternative<Cmd_Rotate>(c) ||
           std::holds_alternative<Cmd_Concat>(c) || std::holds_alternative<Cmd_Concat4x4>(c) ||
           std::holds_alternative<Cmd_SetClip>(c) || std::holds_alternative<Cmd_SetClipRRect>(c) ||
           std::holds_alternative<Cmd_ResetClip>(c) || std::holds_alternative<Cmd_SetClipPolygon>(c) ||
           std::holds_alternative<Cmd_ClipSvgPath>(c) || std::holds_alternative<Cmd_BeginFrame>(c) ||
           std::holds_alternative<Cmd_EndFrame>(c);
}

bool isBreak(const DrawCommand& c) {
    return std::holds_alternative<Cmd_LayerBreak>(c) || std::holds_alternative<Cmd_SurfaceBreak>(c);
}

SkRect pointsBounds(std::span<const PointF> pts) {
    if (pts.empty()) return SkRect::MakeEmpty();
    SkRect r = SkRect::MakeLTRB(pts[0].x, pts[0].y, pts[0].x, pts[0].y);
    for (const auto& p : pts) {
        r.fLeft = std::min(r.fLeft, p.x);
        r.fTop = std::min(r.fTop, p.y);
        r.fRight = std::max(r.fRight, p.x);
        r.fBottom = std::max(r.fBottom, p.y);
    }
    return r;
}

SkRect pathBounds(std::string_view d, bool& ok) {
    SkPath path;
    std::string s(d);
    if (!SkParsePath::FromSVGString(s.c_str(), &path)) { ok = false; return SkRect::MakeEmpty(); }
    return path.getBounds();
}

bool spanBounds(const CommandBuffer& buf, SkRect& out);

// A draw command's bounds in the space it is recorded in. False: unbounded.
bool commandBounds(const CommandBuffer& buf, const DrawCommand& cmd, SkRect& r) {
    bool ok = true;
    std::visit(
        [&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            auto rect = [&](float x, float y, float w, float h, float outset) {
                r = SkRect::MakeXYWH(x, y, w, h).makeSorted();
                r.outset(outset, outset);
            };
            if constexpr (std::is_same_v<T, Cmd_FillRect> || std::is_same_v<T, Cmd_FillRoundRect> ||
                          std::is_same_v<T, Cmd_FillRoundRectRadii>) {
                rect(c.x, c.y, c.w, c.h, 0);
            } else if constexpr (std::is_same_v<T, Cmd_DrawRect> || std::is_same_v<T, Cmd_DrawRoundRect>) {
                rect(c.x, c.y, c.w, c.h, 1);
            } else if constexpr (std::is_same_v<T, Cmd_DrawRoundRectRadii>) {
                rect(c.x, c.y, c.w, c.h, c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawBoxShadow> || std::is_same_v<T, Cmd_DrawBoxShadowRadii>) {
                if (c.inset) {
                    rect(c.x, c.y, c.w, c.h, 0);
                } else {
                    rect(c.x + c.offsetX, c.y + c.offsetY, c.w, c.h, std::fabs(c.spread) + 2.0f * c.blur);
                    r.join(SkRect::MakeXYWH(c.x, c.y, c.w, c.h).makeSorted());
                }
            } else if constexpr (std::is_same_v<T, Cmd_DrawText>) {
                const SkTextBlob* blob =
                    c.blobIndex != CommandBuffer::kNoTextBlob ? buf.textBlobAt(c.blobIndex) : nullptr;
                if (!blob) { ok = false; return; }
                r = blob->bounds().makeOffset(c.x, c.y);
                r.outset(2.0f * c.blur, 2.0f * c.blur);
            } else if constexpr (std::is_same_v<T, Cmd_DrawLine>) {
                r = SkRect::MakeLTRB(c.x1, c.y1, c.x2, c.y2).makeSorted();
                r.outset(c.thickness, c.thickness);
            } else if constexpr (std::is_same_v<T, Cmd_DrawImage> || std::is_same_v<T, Cmd_DrawPixelsRGBA> ||
                                 std::is_same_v<T, Cmd_DrawSvgMarkup> || std::is_same_v<T, Cmd_DrawSharedPixels>) {
                rect(c.x, c.y, c.w, c.h, 0);
            } else if constexpr (std::is_same_v<T, Cmd_DrawCircle>) {
                rect(c.cx - c.r, c.cy - c.r, 2 * c.r, 2 * c.r, c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawEllipse>) {
                rect(c.cx - c.rx, c.cy - c.ry, 2 * c.rx, 2 * c.ry, c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPath>) {
                r = pathBounds(buf.stringAt(c.pathOffset, c.pathLen), ok);
                r.outset(2.0f * c.strokeWidth, 2.0f * c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPolygon>) {
                r = pointsBounds(buf.spanAt<PointF>(c.pointsOffset, c.pointsLen));
                r.outset(2.0f * c.strokeWidth, 2.0f * c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPolyline>) {
                r = pointsBounds(buf.spanAt<PointF>(c.pointsOffset, c.pointsLen));
                r.outset(2.0f * c.strokeWidth, 2.0f * c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawSvgPath>) {
                r = pathBounds(buf.stringAt(c.pathOffset, c.pathLen), ok);
                if (!ok) return;
                // A gradientTransform cannot move the paint out of the path;
                // the stroke's miter can reach out by half its width times
                // the limit.
                const float reach = c.strokeStyle.width * std::max(1.0f, c.strokeStyle.miterLimit);
                r.outset(reach, reach);
            } else if constexpr (std::is_same_v<T, Cmd_FillLinearGradient> ||
                                 std::is_same_v<T, Cmd_FillRadialGradient> ||
                                 std::is_same_v<T, Cmd_FillConicGradient>) {
                rect(c.x, c.y, c.w, c.h, 0);
            } else if constexpr (std::is_same_v<T, Cmd_ReplaySpan>) {
                const CommandBuffer* span = buf.spanAt(c.index);
                if (!span) { r = SkRect::MakeEmpty(); return; }
                ok = spanBounds(*span, r);
            } else {
                // A clear, a backdrop filter, an inline canvas: everywhere.
                ok = false;
            }
        },
        cmd);
    return ok;
}

// Map `local` (drawn under `w`) into the space the walk started in.
bool mapped(const Walker& w, const SkRect& local, SkRect& out) {
    if (w.cur.perspective) return false;
    if (w.cur.inFilter) {
        out = w.cur.filterBounds;
        return true;
    }
    out = w.cur.m.mapRect(local);
    return true;
}

// A buffer's bounds in its own space (memoized on the buffer).
bool spanBounds(const CommandBuffer& buf, SkRect& out) {
    if (buf.boundsState_ == 2) return false;
    if (buf.boundsState_ == 1) {
        out = SkRect::MakeLTRB(buf.boundsLTRB_[0], buf.boundsLTRB_[1], buf.boundsLTRB_[2], buf.boundsLTRB_[3]);
        return true;
    }
    Walker w;
    SkRect acc = SkRect::MakeEmpty();
    bool bounded = true;
    for (const auto& cmd : buf.commands()) {
        if (isStateCommand(cmd)) { w.apply(buf, cmd); continue; }
        if (isBreak(cmd)) continue;
        SkRect local, dev;
        if (!commandBounds(buf, cmd, local) || !mapped(w, local, dev)) { bounded = false; break; }
        acc.join(dev);
    }
    if (!bounded) {
        buf.boundsState_ = 2;
        return false;
    }
    buf.boundsState_ = 1;
    buf.boundsLTRB_[0] = acc.fLeft;
    buf.boundsLTRB_[1] = acc.fTop;
    buf.boundsLTRB_[2] = acc.fRight;
    buf.boundsLTRB_[3] = acc.fBottom;
    out = acc;
    return true;
}

void damageCommand(const CommandBuffer& buf, const DrawCommand& cmd, const Walker& w, Damage& d) {
    if (d.full) return;
    if (isBreak(cmd)) { d.setFull(); return; }
    SkRect local, dev;
    if (!commandBounds(buf, cmd, local) || !mapped(w, local, dev)) { d.setFull(); return; }
    if (dev.isEmpty()) return;
    d.addRect(dev.fLeft - kPad, dev.fTop - kPad, dev.fRight + kPad, dev.fBottom + kPad);
}

bool sameState(const Walker& a, const Walker& b) {
    return a.cur.m == b.cur.m && a.cur.perspective == b.cur.perspective && a.cur.inFilter == b.cur.inFilter &&
           (!a.cur.inFilter || a.cur.filterBounds == b.cur.filterBounds);
}

// Everything left of `buf` from `i` on, walked from `w`, is damage.
void drain(const CommandBuffer& buf, size_t i, Walker w, Damage& d) {
    const auto& cmds = buf.commands();
    for (; i < cmds.size() && !d.full; ++i) {
        if (isStateCommand(cmds[i])) w.apply(buf, cmds[i]);
        else damageCommand(buf, cmds[i], w, d);
    }
}

void diffLevel(const CommandBuffer& ob, Walker ow, const CommandBuffer& nb, Walker nw, Damage& d) {
    const auto& oc = ob.commands();
    const auto& nc = nb.commands();
    size_t i = 0, j = 0;
    if (sameState(ow, nw)) {
        while (i < oc.size() && j < nc.size() && !d.full) {
            const DrawCommand& a = oc[i];
            const DrawCommand& b = nc[j];
            if (sameCommand(ob, a, nb, b)) {
                if (isStateCommand(a)) {
                    ow.apply(ob, a);
                    nw.apply(nb, b);
                }
                ++i, ++j;
                continue;
            }
            // Two recordings of one element: what differs is inside.
            if (std::holds_alternative<Cmd_ReplaySpan>(a) && std::holds_alternative<Cmd_ReplaySpan>(b)) {
                const CommandBuffer* sa = ob.spanAt(std::get<Cmd_ReplaySpan>(a).index);
                const CommandBuffer* sb = nb.spanAt(std::get<Cmd_ReplaySpan>(b).index);
                if (sa && sb && sa->ownerId() != 0 && sa->ownerId() == sb->ownerId()) {
                    diffLevel(*sa, ow, *sb, nw, d);
                    ++i, ++j;
                    continue;
                }
            }
            if (isBreak(a) || isBreak(b)) {
                d.setFull();
                return;
            }
            // Two draws apart under the same state: both places.
            if (!isStateCommand(a) && !isStateCommand(b) && !std::holds_alternative<Cmd_ReplaySpan>(a) &&
                !std::holds_alternative<Cmd_ReplaySpan>(b)) {
                damageCommand(ob, a, ow, d);
                damageCommand(nb, b, nw, d);
                ++i, ++j;
                continue;
            }
            break;  // the structure differs from here on
        }
    }
    drain(ob, i, ow, d);
    drain(nb, j, nw, d);
}

}  // namespace

Damage diffDamage(const CommandBuffer& older, const CommandBuffer& newer) {
    Damage d;
    if (older.hasBackdrop() || newer.hasBackdrop()) {
        d.setFull();
        return d;
    }
    diffLevel(older, Walker{}, newer, Walker{}, d);
    return d;
}

}  // namespace bro::render
