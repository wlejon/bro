#include "engine/engine.h"
#include "engine/inspector_highlight.h"
#include "engine/key_mapping.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "layout/pseudo_style.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"
#include "dom/element_geometry.h"

#include <filesystem>
#include <fstream>

#include "observer_check.js.h"

#include "platform/sdl_window.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/event_dispatch.h"
#include "js/audio_bindings.h"
#include "js/storage_bindings.h"
#include "js/settings_bindings.h"
#include "js/dialog_bindings.h"
#include "js/window_bindings.h"
#include "js/custom_elements.h"
#include "js/webgl2_bindings.h"
#include "js/image_bindings.h"
#include "js/video_bindings.h"
#include "js/headless_bindings.h"
#include "js/worker.h"
#if BRO_WITH_PHYSICS
#include "js/physics_bindings.h"
#endif
#include "js/scene_bindings.h"
#include "js/menu_bindings.h"
#include "js/gizmo_bindings.h"
#include "js/mesh_bindings.h"
#include "js/rigging_bindings.h"
#include "js/ai_bindings.h"
#include "js/terrain_bindings.h"
#include "js/net_bindings.h"
#include "js/server_bindings.h"

#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "api/api.h"
#include "runtime/runtime.h"
#include <broaudio/engine.h>
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include "dom/shadow_root.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "layout/selection_geometry.h"

#include <cstring>
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "layout/element_ref_adapter.h"
#include "layout/layout_node_adapter.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "engine/default_styles.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <glad/gl.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>

namespace bro::engine {

using bromath::cfromColor8;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------





// ---------------------------------------------------------------------------
// Lightweight tick for use during modal blocking (move/resize, dialogs)
// ---------------------------------------------------------------------------

void Engine::tickTimersOnly()
{
    // Advance the bro.time scaled clock exactly like a frame top would, so
    // timers keep obeying pause/timescale during modal blocking, and the
    // frame loop resumes with a fresh wall reference (no post-modal jump).
    double wallNow = util::currentTimeMs();
    if (lastWallTickMs_ > 0.0 && wallNow > lastWallTickMs_)
        engineNowMs_ += (wallNow - lastWallTickMs_) * effectiveTimeScale();
    lastWallTickMs_ = wallNow;
    timers_->tick(engineNowMs_);
    jsRuntime_->executePendingJobs();
}

// ---------------------------------------------------------------------------
// bro.time — global pause + timescale
// ---------------------------------------------------------------------------

void Engine::setTimeScale(double scale)
{
    if (!std::isfinite(scale)) return;
    timeScale_ = std::clamp(scale, 0.0, 100.0);
}

void Engine::setTimePaused(bool paused)
{
    if (timePaused_ == paused) return;
    timePaused_ = paused;
    // Suspend/resume audio output with the clock. broaudio's master pause is
    // a transport freeze (silence out, engine clock stops), not a mute — so
    // scheduled notes/clips resume exactly in place. Timescale is deliberately
    // NOT forwarded: audio always renders at real rate (no pitch shift).
    if (audioEngine_) audioEngine_->setMasterPaused(paused);
}


// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------


// Input handling methods (handleMouse*, handleKey*, handleTextInput,
// handleWheel, advanceFocus, dispatchInputEvent) are in input_handling.cpp.


dom::Element* Engine::hitTest(float x, float y) {
    // x, y are already in document space (scroll-adjusted by callers)
    if (!document_ || !document_->documentElement())
        return document_ ? document_->body() : nullptr;

    auto* root = document_->layoutRoot();
    if (!root) return document_->body();
    auto* node = htmlayout::layout::hitTest(root, x, y);
    auto* hit = layout::LayoutNodeAdapter::elementFor(node);
    // The <html> element fills the viewport — stray clicks outside any
    // laid-out content should still resolve to the document element.
    if (!hit) return document_->documentElement();
    return hit;
}

// ---------------------------------------------------------------------------
// Event dispatch to JS (delegates to shared implementation)
// ---------------------------------------------------------------------------

void Engine::dispatchEvent(dom::Element* target, dom::Event& event) {
    if (!target || !jsRuntime_) return;
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}

dom::Element* Engine::pointerCaptureFor(int pointerId) const {
    auto it = pointerCaptures_.find(pointerId);
    return it == pointerCaptures_.end() ? nullptr : it->second.get();
}

void Engine::dispatchPointerAlias(const char* type, dom::Element* target,
                                  const dom::MouseEvent& src) {
    if (!jsRuntime_) return;
    // Pointer capture: pointermove/pointerup/pointercancel retarget to the
    // captured element — the web's drag idiom, so the element that captured
    // on pointerdown keeps seeing the gesture wherever the cursor goes.
    // pointerdown never retargets (capture is taken during it, not before),
    // and cross-document targets (3D HtmlNode inner docs) are left alone —
    // their events carry panel-local coordinates, not app-document ones.
    // This is the MOUSE pointer's alias path (pointerId 1); touch contacts
    // route through handleTouch* in touch_input.cpp with their own ids.
    const bool isDown = std::strcmp(type, "pointerdown") == 0;
    dom::Element* captured = isDown ? nullptr : pointerCaptureFor(kMousePointerId);
    if (captured && target && captured->document() != target->document()) {
        captured = nullptr;
    }
    // Self-heal: a buttons-free pointermove while captured means the release
    // never reached us (e.g. swallowed by a native dialog) — end the capture
    // rather than dragging forever.
    if (captured && !isDown && src.buttons() == 0 &&
        std::strcmp(type, "pointermove") == 0) {
        releasePointerCapture(captured, kMousePointerId);
        captured = nullptr;
    }
    if (captured) target = captured;
    if (!target) return;
    // Clone the already-populated mouse event under the pointer type. Handlers
    // read the shared MouseEvent fields; populateJsEvent() adds pointerId etc.
    // when the type begins with "pointer". Pointer events bubble and cancel
    // like their mouse analogs.
    dom::MouseEvent pe(type, /*bubbles=*/true, /*cancelable=*/true);
    pe.setIsTrusted(true);
    pe.setClientX(src.clientX());     pe.setClientY(src.clientY());
    pe.setScreenX(src.screenX());     pe.setScreenY(src.screenY());
    pe.setPageX(src.pageX());         pe.setPageY(src.pageY());
    pe.setOffsetX(src.offsetX());     pe.setOffsetY(src.offsetY());
    pe.setMovementX(src.movementX()); pe.setMovementY(src.movementY());
    pe.setButton(src.button());       pe.setButtons(src.buttons());
    pe.setCtrlKey(src.ctrlKey());     pe.setShiftKey(src.shiftKey());
    pe.setAltKey(src.altKey());       pe.setMetaKey(src.metaKey());
    // pointerId/pointerType/isPrimary keep their MouseEvent defaults (1,
    // "mouse", primary) — exactly the mouse pointer this path synthesizes.
    // offsetX/Y in `src` are relative to the hit target — recompute against
    // the element actually receiving the event.
    if (captured) applyMouseOffset(pe, captured);
    js::dispatchDomEvent(jsRuntime_->getContext(), target, pe);
    // Implicit release (spec): the pointerup/pointercancel that ends the
    // gesture also ends the capture.
    auto capIt = pointerCaptures_.find(kMousePointerId);
    if (!isDown && capIt != pointerCaptures_.end() && capIt->second.held() &&
        (std::strcmp(type, "pointerup") == 0 ||
         std::strcmp(type, "pointercancel") == 0)) {
        releasePointerCapture(capIt->second.get(), kMousePointerId);
    }
}

bool Engine::setPointerCapture(dom::Element* target, int pointerId) {
    // Validate the JS-supplied element against the app document (same
    // soundness policy as requestPointerLock).
    if (!target || !document_ || !document_->isNodeLive(target)) return false;
    // A touch pointerId must name a live contact; the mouse pointer (id 1)
    // is always live. Unknown ids are a silent no-op (the web throws
    // NotFoundError; bro's binding layer stays exception-free here).
    if (pointerId != kMousePointerId) {
        bool live = false;
        for (const auto& c : touchContacts_) {
            if (c.pointerId == pointerId) { live = true; break; }
        }
        if (!live) return false;
    }
    auto& handle = pointerCaptures_[pointerId];
    if (handle.get() == target) return true;
    handle.assign(document_.get(), target);
    dom::Event evt("gotpointercapture", /*bubbles=*/true, /*cancelable=*/false);
    evt.setIsTrusted(true);
    dispatchEvent(target, evt);
    return true;
}

void Engine::releasePointerCapture(dom::Element* target, int pointerId) {
    auto it = pointerCaptures_.find(pointerId);
    if (it == pointerCaptures_.end() || !it->second.held()) return;
    dom::Element* cur = it->second.get();
    // Only the holder may release; a stale (freed) holder always releases.
    if (cur && target != cur) return;
    pointerCaptures_.erase(it);
    if (cur) {
        dom::Event evt("lostpointercapture", /*bubbles=*/true,
                       /*cancelable=*/false);
        evt.setIsTrusted(true);
        dispatchEvent(cur, evt);
    }
}

bool Engine::hasPointerCapture(const dom::Element* target, int pointerId) const {
    if (!target) return false;
    return pointerCaptureFor(pointerId) == target;
}

// Walk the document + shadow trees and pump any pending HTMLMediaElement
// events (loadedmetadata, timeupdate, ended) on each ElVideo. Called from
// the main thread because QuickJS is not thread-safe; ElVideo::draw() runs
// on the raster thread and deliberately does not touch JS.
static void pumpVideoEventsWalk(dom::Element* el, bool& anyPlaying) {
    if (!el) return;
    if (auto* v = el->videoControl()) {
        v->pumpEvents();
        if (v->isPlaying()) anyPlaying = true;
    }
    el->forEachComposedChild([&](dom::Element* c) {
        pumpVideoEventsWalk(c, anyPlaying);
    });
}
void Engine::pumpVideoEvents() {
    // The engine's initial layout flush runs BEFORE user script, so
    // dispatching loadedmetadata / timeupdate there would drop events on
    // the floor (no listeners registered yet). Wait until the user code
    // has had a chance to run — the windowed main loop and any JS-driven
    // flush() set this flag before pumping.
    if (!mediaEventsArmed_) return;
    if (!document_) return;
    bool anyPlaying = false;
    pumpVideoEventsWalk(document_->documentElement(), anyPlaying);
    // Playing <video> elements don't mutate the DOM, so nothing else would
    // mark the document dirty. Force a re-raster each frame while any video
    // is advancing so ElVideo::draw() keeps calling pipeline_->advance() and
    // presenting new frames.
    if (anyPlaying) {
        // Paint-dirty: a playing video changes pixels, never geometry, so it
        // must not drag the document through a layout pass on every frame.
        document_->markPaintDirty();
        // markDirty alone isn't enough in the windowed main loop: if the
        // layout thread is already idle, nothing sets uiDirty_ so the
        // raster signal path is skipped. Set uiDirty_ directly so the
        // "no layout this frame" branch at engine.cpp still signals raster.
        uiDirty_ = true;
    }
}

// Is this node inside a subtree marked contenteditable? Walks up the DOM
// to find the nearest ancestor element with a contenteditable attribute.
// Treats contenteditable="false" as explicitly *not* editable (matches spec).
// The nearest Element ancestor (or the node itself if already an Element) —
// used to look up the CSS transform chain that applies to a Range endpoint,
// since htmlayout's selection/caret geometry (below) is transform-unaware.
static bro::dom::Element* nearestElementAncestor(bro::dom::Node* node) {
    for (bro::dom::Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() == bro::dom::NodeType::Element)
            return static_cast<bro::dom::Element*>(n);
    }
    return nullptr;
}

static bool inContenteditableHost(bro::dom::Node* node) {
    for (bro::dom::Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() != bro::dom::NodeType::Element) continue;
        auto* el = static_cast<bro::dom::Element*>(n);
        if (!el->hasAttribute("contenteditable")) continue;
        const std::string& v = el->getAttribute("contenteditable");
        if (v == "false") return false;
        return true;
    }
    return false;
}

float Engine::docContentOffsetY() const {
    return static_cast<float>(contentTop()) - scrollY_;
}

Engine::ContentInsets Engine::contentInsets() const {
    ContentInsets c;
    // Headless is deliberately NOT special-cased: the menu bar is opt-in
    // (bro.menu.show()), so an app that shows one gets the same reserved
    // inset in every display mode. Suppressing it headless made headless
    // rendering diverge from windowed and hid inset-dependent compositing
    // bugs from the entire headless test surface.
    c.top = menuBar_.visible ? menuBar_.height : 0;
    if (inspector_.visible) {
        if (inspector_.dock == InspectorDock::Right) {
            c.right = inspector_.width;
        } else {
            c.bottom = inspector_.height;
        }
    }
    return c;
}

void Engine::updateSelectionSnapshot() {
    selectionSnapshot_.rects.clear();
    selectionSnapshot_.hasCaret = false;
    selectionSnapshot_.compUnderlines.clear();
    // Default: translucent accent blue (#3377ff at 1/3 alpha), legible because
    // the highlight paints OVER the glyphs.
    selectionSnapshot_.highlight = bromath::cfromColor8({0x33, 0x77, 0xff, 0x55});
    if (!document_ || !textMetrics_) return;

    // Contenteditable IME preedit: a thin underline segment per line the
    // composition range covers — the same visual cue the controls draw under
    // their preedit, in the composition host's text color. Computed before
    // the Selection logic below (it does not depend on the live range: the
    // preedit's provisional splice IS the range's neighborhood, and the
    // collapsed-caret path underneath still paints the composition cursor).
    if (editComp_.active) {
        auto* tn = editableCompositionTarget();   // drops a stale composition
        if (tn && editComp_.length > 0) {
            auto rects = layout::getSelectionRects(
                document_.get(), tn, editComp_.start,
                tn, editComp_.start + editComp_.length, *textMetrics_);
            auto* ctxEl = nearestElementAncestor(tn);
            selectionSnapshot_.compColor = bromath::Color{0.0f, 0.0f, 0.0f, 1.0f};
            if (auto* host = editComp_.host.get()) {
                auto& style = host->computedStyle();
                auto cIt = style.find("color");
                bromath::Color parsed;
                if (cIt != style.end() && !cIt->second.empty() &&
                    layout::DrawTraversal::tryParseColor(cIt->second, parsed)) {
                    selectionSnapshot_.compColor = parsed;
                }
            }
            for (const auto& r : rects) {
                // Same transform-unaware-geometry projection as the
                // selection rects below.
                auto pr = ctxEl ? dom::projectRectThroughAncestors(
                                      ctxEl, r.x, r.y, r.width, r.height)
                                : dom::AbsoluteRect{r.x, r.y, r.width, r.height};
                // 1px line hugging the bottom of the line box (glyphs sit on
                // the baseline above it — reads as the IME underline).
                selectionSnapshot_.compUnderlines.push_back(
                    {pr.x, pr.y + pr.height - 2.0f, pr.width, 1.0f});
            }
        }
    }
    auto* sel = document_->selection();
    if (!sel || sel->rangeCount() == 0) return;
    const auto* range = sel->getRangeAt(0);
    if (!range) return;
    // Validate the Range endpoints are still live. A hit-test textnode can
    // be orphaned or freed via a path that bypasses freeNode (or slips past
    // the onNodeDestroyed safety net), leaving the Range with a dangling
    // pointer. Dereferencing it in getSelectionRects / inContenteditableHost
    // crashes. If either endpoint is stale, clear the selection and bail.
    if (!document_->ownsNode(range->startContainer()) ||
        !document_->ownsNode(range->endContainer())) {
        sel->removeAllRanges();
        return;
    }

    if (!sel->isCollapsed()) {
        auto rects = layout::getSelectionRects(document_.get(),
                                               range->startContainer(),
                                               range->startOffset(),
                                               range->endContainer(),
                                               range->endOffset(),
                                               *textMetrics_);
        // getSelectionRects returns transform-unaware document-space rects
        // (htmlayout has no notion of CSS transform). Project through the
        // ancestor chain of the selection's start — the common-case approx.
        // (a selection normally stays within one transformed subtree, e.g.
        // one panned/zoomed node-forge card) — or the highlight renders at
        // its raw pre-transform position while the selected text itself
        // paints correctly transformed, same symptom as the dropdown/
        // slider/canvas-layer bugs already fixed this session.
        auto* ctxEl = nearestElementAncestor(range->startContainer());
        // ::selection background-color, scoped to the element at the selection
        // start (the common case is a selection within one styled subtree).
        // The overlay paints over the glyphs, so a fully opaque author color
        // would hide the text it highlights — cap the alpha at 0.5.
        if (ctxEl) {
            auto selStyle = layout::resolveStyledPseudo(ctxEl, "selection");
            bromath::Color bg;
            if (!selStyle.empty() &&
                layout::pseudoColor(selStyle, "background-color", bg) &&
                bg.a > 0.0f) {
                if (bg.a > 0.5f) bg.a = 0.5f;
                selectionSnapshot_.highlight = bg;
            }
        }
        selectionSnapshot_.rects.reserve(rects.size());
        for (const auto& r : rects) {
            auto pr = ctxEl ? dom::projectRectThroughAncestors(ctxEl, r.x, r.y, r.width, r.height)
                             : dom::AbsoluteRect{r.x, r.y, r.width, r.height};
            selectionSnapshot_.rects.push_back({pr.x, pr.y, pr.width, pr.height});
        }
        return;
    }

    // Collapsed selection inside a contenteditable host: compute caret rect.
    // Bro's inputs/textareas manage their own carets; the DOM Selection caret
    // only shows outside form fields.
    if (!inContenteditableHost(range->startContainer())) return;
    auto* startNode = range->startContainer();
    if (!startNode || startNode->nodeType() != bro::dom::NodeType::Text) return;
    auto* tn = static_cast<bro::dom::TextNode*>(startNode);
    float cx = 0, cy = 0, ch = 0;
    if (!layout::getCaretRect(document_.get(), tn, range->startOffset(),
                              *textMetrics_, cx, cy, ch)) return;
    // Same transform-unaware-geometry caveat as the selection rects above.
    auto* ctxEl = nearestElementAncestor(tn);
    auto pr = ctxEl ? dom::projectRectThroughAncestors(ctxEl, cx, cy, 0.0f, ch)
                     : dom::AbsoluteRect{cx, cy, 0.0f, ch};
    selectionSnapshot_.hasCaret = true;
    selectionSnapshot_.caretX = pr.x;
    selectionSnapshot_.caretY = pr.y;
    selectionSnapshot_.caretHeight = pr.height;
}

void Engine::drawSelectionHighlight(render::Renderer* renderer, float docOffsetY) {
    if (!renderer) return;
    // Fill resolved in updateSelectionSnapshot (::selection background-color,
    // or the translucent accent default). NOTE: bromath::Color is float 0-1 —
    // the old hardcoded {0x33, 0x77, 0xff, 0x55} int literals saturated every
    // channel to 1.0 and painted the highlight as OPAQUE WHITE over the text.
    for (const auto& r : selectionSnapshot_.rects) {
        renderer->fillRect(r.x, r.y + docOffsetY, r.w, r.h,
                           selectionSnapshot_.highlight);
    }
    if (selectionSnapshot_.hasCaret) {
        bromath::Color caretColor{1.0f, 1.0f, 1.0f, 1.0f};
        renderer->fillRect(selectionSnapshot_.caretX,
                           selectionSnapshot_.caretY + docOffsetY,
                           1.5f, selectionSnapshot_.caretHeight, caretColor);
    }
    // Contenteditable IME preedit underline (see updateSelectionSnapshot).
    for (const auto& r : selectionSnapshot_.compUnderlines) {
        renderer->fillRect(r.x, r.y + docOffsetY, r.w, r.h,
                           selectionSnapshot_.compColor);
    }
}

void Engine::drawElementScrollbars(render::Renderer* renderer,
                                   dom::Element* root,
                                   float offsetX, float offsetY) {
    if (!renderer || !root) return;
    std::function<void(dom::Element*, float, float)> walk;
    walk = [&](dom::Element* elem, float ox, float oy) {
        if (!elem) return;
        auto& style = elem->computedStyle();
        auto dispIt = style.find("display");
        if (dispIt != style.end() && dispIt->second == "none") return;

        auto& lbox = elem->layoutBox();
        float absX = lbox.contentRect.x + ox;
        float absY = lbox.contentRect.y + oy;

        std::string ov = getOverflowY(style);
        if (overflowScrollable(ov)) {
            float maxST = maxScrollTop(elem);
            if (maxST > 0) {
                float viewH = lbox.contentRect.height;
                float contentH = viewH + maxST;
                float bx = absX - lbox.padding.left - lbox.border.left;
                float by = absY - lbox.padding.top - lbox.border.top;
                float bw = lbox.fullWidth();
                float bh = lbox.fullHeight();

                auto& es = elementScrollbar_.style();
                auto m = elementScrollbar_.layout(
                    bx + bw - es.width - es.margin,
                    by, bh, contentH, viewH,
                    elem->scrollTopValue());
                elementScrollbar_.draw(renderer, m);
            }
        }

        float childOx = absX;
        float childOy = absY - elem->scrollTopValue();
        elem->forEachComposedChild([&](dom::Element* child) {
            walk(child, childOx, childOy);
        });
    };
    walk(root, offsetX, offsetY);
}


// ---------------------------------------------------------------------------
// Replaced element control initialization
// ---------------------------------------------------------------------------

void Engine::ensureReplacedElements(dom::Element* elem) {
    JSContext* jsCtx = jsRuntime_ ? jsRuntime_->getContext() : nullptr;
    bro::engine::ensureReplacedElements(elem, renderer_.get(), jsCtx,
                                         audioEngine_.get());
}

// Headless/capture API (flush, advanceTime, eval, screenshot, capturePixels,
// querySelector, dispatchClickOn) is in headless_api.cpp.
} // namespace bro::engine