// Mouse routing from the Engine into the GizmoManager. Lives in its own
// TU to keep engine.cpp / input_handling.cpp focussed on DOM / overlay
// plumbing. The gizmo sits between the overlay/system layers (which are
// modal UI) and the DOM — it consumes mouse events only when actively
// dragging or when a handle is hit.

#include "engine/engine.h"
#include "engine/gizmo.h"
#include "scene/scene_graph.h"
#include "dom/element.h"
#include "dom/element_geometry.h"

namespace bro::engine {

using scene::SceneGraph;
using bromath::Vec3;
using bromath::Quat;

// Absolute (transform-correct) content-box rect of `el`. Shares the same
// ancestor-transform-aware math as getBoundingClientRect() — see
// dom::absoluteContentBox().
static bool elementAbsoluteBox(dom::Element* el,
                               float& outX, float& outY,
                               float& outW, float& outH) {
    if (!el) return false;
    dom::AbsoluteRect r = dom::absoluteContentBox(el);
    outX = r.x;
    outY = r.y;
    outW = r.width;
    outH = r.height;
    return outW > 0 && outH > 0;
}

// Map an absolute (post-transform) screen offset into the scene's own canvas
// coordinate space.
//
// These are two different spaces whenever a CSS transform scales the canvas or
// an ancestor. absoluteContentBox() projects through ancestor transforms, so
// its width is the on-screen size, while SceneGraph::canvasWidth() tracks the
// element's *layout* content rect — unscaled. unprojectLocal() divides by the
// latter, so feeding it the former made every ray wrong by the scale factor:
// under a scale(2) the normalized device x ran -1..3 instead of -1..1, and the
// gizmo's handles could not even be picked, let alone dragged predictably.
static void absoluteToCanvas(const SceneGraph* g,
                             float absW, float absH,
                             float& localX, float& localY) {
    if (!g) return;
    const float cw = static_cast<float>(g->canvasWidth());
    const float ch = static_cast<float>(g->canvasHeight());
    // Before the first layout-driven setCanvasSize the graph has no size yet;
    // 1:1 is the only sane assumption and matches the untransformed case.
    if (cw > 0 && absW > 0) localX *= cw / absW;
    if (ch > 0 && absH > 0) localY *= ch / absH;
}

SceneGraph* Engine::findSceneGraphAt(float x, float y,
                                     float& outLocalX, float& outLocalY) const {
    for (auto& entry : sceneGraphs_) {
        if (!entry.graph || !entry.element) continue;
        float ex, ey, ew, eh;
        if (!elementAbsoluteBox(entry.element, ex, ey, ew, eh)) continue;
        if (x < ex || x >= ex + ew) continue;
        if (y < ey || y >= ey + eh) continue;
        outLocalX = x - ex;
        outLocalY = y - ey;
        absoluteToCanvas(entry.graph.get(), ew, eh, outLocalX, outLocalY);
        return entry.graph.get();
    }
    return nullptr;
}

bool Engine::gizmoHandleMouseDown(float x, float y, int /*button*/) {
    if (!gizmo_) return false;

    // If the gizmo is already dragging (edge case: missed mouseUp), flush.
    if (gizmo_->isDragging()) gizmo_->endDrag();

    if (!gizmo_->visible()) return false;

    float lx, ly;
    SceneGraph* g = findSceneGraphAt(x, y, lx, ly);
    if (!g) return false;

    Vec3 rayO, rayD;
    if (!g->unprojectLocal(lx, ly, rayO, rayD)) return false;

    auto hit = gizmo_->pick(rayO, rayD);
    if (hit.axis == engine::GizmoAxis::None) return false;

    gizmo_->beginDrag(hit, rayO, rayD);
    uiDirty_ = true;
    return true;
}

bool Engine::gizmoHandleMouseMove(float x, float y) {
    if (!gizmo_) return false;

    float lx, ly;
    SceneGraph* g = findSceneGraphAt(x, y, lx, ly);

    // Hover tracking (no-drag case). Only re-pick if visible and not dragging.
    if (!gizmo_->isDragging()) {
        if (!gizmo_->visible() || !g) {
            if (gizmo_->hovered() != engine::GizmoAxis::None) {
                gizmo_->setHovered(engine::GizmoAxis::None);
                uiDirty_ = true;
            }
            return false;
        }
        Vec3 rayO, rayD;
        if (!g->unprojectLocal(lx, ly, rayO, rayD)) return false;
        auto hit = gizmo_->pick(rayO, rayD);
        if (gizmo_->hovered() != hit.axis) {
            gizmo_->setHovered(hit.axis);
            uiDirty_ = true;
        }
        return false;  // hover never consumes
    }

    // Active drag — consume. If the mouse leaves the canvas we still want
    // drag to follow (classic DCC behaviour); use the last scene graph
    // found at drag-start by falling back to the first available graph
    // with a valid camera.
    if (!g) {
        for (auto& entry : sceneGraphs_) {
            if (entry.graph && entry.graph->canvasWidth() > 0) {
                g = entry.graph.get();
                float ex, ey, ew, eh;
                if (elementAbsoluteBox(entry.element, ex, ey, ew, eh)) {
                    lx = x - ex; ly = y - ey;
                    // Same space conversion as findSceneGraphAt — without it a
                    // drag that wandered off the canvas would jump the moment
                    // it left, under any scaled view.
                    absoluteToCanvas(g, ew, eh, lx, ly);
                }
                break;
            }
        }
        if (!g) return true;
    }

    Vec3 rayO, rayD;
    if (!g->unprojectLocal(lx, ly, rayO, rayD)) return true;

    Vec3 dT; Quat dR; Vec3 dS;
    gizmo_->updateDrag(rayO, rayD, dT, dR, dS);
    uiDirty_ = true;
    return true;
}

bool Engine::gizmoHandleMouseUp(float /*x*/, float /*y*/, int /*button*/) {
    if (!gizmo_ || !gizmo_->isDragging()) return false;
    gizmo_->endDrag();
    uiDirty_ = true;
    return true;
}

} // namespace bro::engine
