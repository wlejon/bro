// Mouse routing from the Engine into the GizmoManager. Lives in its own
// TU to keep engine.cpp / input_handling.cpp focussed on DOM / overlay
// plumbing. The gizmo sits between the overlay/system layers (which are
// modal UI) and the DOM — it consumes mouse events only when actively
// dragging or when a handle is hit.

#include "engine/engine.h"
#include "engine/gizmo.h"
#include "scene/scene_graph.h"
#include "dom/element.h"

namespace bro::engine {

using scene::SceneGraph;
using scene::Vec3;

// Accumulate absolute top-left of `el` by walking layoutParent chain and
// subtracting scrollTop at each step. Mirrors the logic used elsewhere to
// position canvas scenes (engine.cpp:485-503).
static bool elementAbsoluteBox(dom::Element* el,
                               float& outX, float& outY,
                               float& outW, float& outH) {
    if (!el) return false;
    auto& box = el->layoutBox();
    float x = box.contentRect.x;
    float y = box.contentRect.y;
    for (auto* lp = el->layoutParent(); lp; lp = lp->layoutParent()) {
        auto& pb = lp->layoutBox();
        x += pb.contentRect.x;
        y += pb.contentRect.y;
        y -= lp->scrollTopValue();
    }
    outX = x;
    outY = y;
    outW = box.contentRect.width;
    outH = box.contentRect.height;
    return outW > 0 && outH > 0;
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
                }
                break;
            }
        }
        if (!g) return true;
    }

    Vec3 rayO, rayD;
    if (!g->unprojectLocal(lx, ly, rayO, rayD)) return true;

    Vec3 dT; scene::Quat dR; Vec3 dS;
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
