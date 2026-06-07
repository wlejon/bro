#include "engine/engine.h"
#include "engine/inspector_highlight.h"
#include "engine/key_mapping.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"

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
#include "js/physics_bindings.h"
#include "js/scene_bindings.h"
#include "js/crosshair_bindings.h"
#include "js/menu_bindings.h"
#include "js/gizmo_bindings.h"
#include "js/mesh_bindings.h"
#include "js/rigging_bindings.h"
#include "js/ai_bindings.h"
#include "js/terrain_bindings.h"
#include "js/net_bindings.h"
#include "js/server_bindings.h"

#include "physics/physics_world.h"
#include "net/net_service.h"
#include "scene/scene_graph.h"
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
#include <bit>
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
    double now = util::currentTimeMs();
    timers_->tick(now);
    jsRuntime_->executePendingJobs();
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

void Engine::reapDeadInputPointers() {
    if (!document_) return;
    // isNodeLive is a pointer-value lookup — safe to call on an already-freed
    // pointer (it never dereferences). A node is reported dead only once
    // drainPendingFrees() has actually destroyed it, so this must run after the
    // drain. Each cached pointer below targets the app document; the scene
    // HtmlNode mirrors and systemHoverTarget_ live in other documents and are
    // left alone (isNodeLive against this document would wrongly null them).
    auto dead = [&](const dom::Node* n) { return n && !document_->isNodeLive(n); };
    if (dead(hoveredElement_)) hoveredElement_ = nullptr;
    if (dead(scrollbarHoveredElement_)) scrollbarHoveredElement_ = nullptr;
    // scrollbarDragTarget_ may belong to a system panel (scrollbarDragSystemDoc_
    // non-null); only scrub it when it's an app-document element.
    if (!scrollbarDragSystemDoc_ && dead(scrollbarDragTarget_)) scrollbarDragTarget_ = nullptr;
    if (dead(lockedElement_)) lockedElement_ = nullptr;
    if (dead(selectionAnchorNode_)) { selectionAnchorNode_ = nullptr; selectionDragging_ = false; }
    if (dead(appMouseState_.mouseDownTarget)) appMouseState_.mouseDownTarget = nullptr;
    if (dead(appMouseState_.lastClickTarget)) appMouseState_.lastClickTarget = nullptr;
}

// ---------------------------------------------------------------------------
// Event dispatch to JS (delegates to shared implementation)
// ---------------------------------------------------------------------------

void Engine::dispatchEvent(dom::Element* target, dom::Event& event) {
    if (!target || !jsRuntime_) return;
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
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
        document_->markDirty();
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
    const bool menuShown = menuBar_.visible
                        && displayMode_ != DisplayMode::Headless;
    c.top = menuShown ? menuBar_.height : 0;
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
    if (!document_ || !textMetrics_) return;
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
        selectionSnapshot_.rects.reserve(rects.size());
        for (const auto& r : rects) {
            selectionSnapshot_.rects.push_back({r.x, r.y, r.width, r.height});
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
    selectionSnapshot_.hasCaret = true;
    selectionSnapshot_.caretX = cx;
    selectionSnapshot_.caretY = cy;
    selectionSnapshot_.caretHeight = ch;
}

void Engine::drawSelectionHighlight(render::Renderer* renderer, float docOffsetY) {
    if (!renderer) return;
    // Accent with transparency — keeps underlying glyphs legible.
    bromath::Color hl{0x33, 0x77, 0xff, 0x55};
    for (const auto& r : selectionSnapshot_.rects) {
        renderer->fillRect(r.x, r.y + docOffsetY, r.w, r.h, hl);
    }
    if (selectionSnapshot_.hasCaret) {
        bromath::Color caretColor{0xff, 0xff, 0xff, 0xff};
        renderer->fillRect(selectionSnapshot_.caretX,
                           selectionSnapshot_.caretY + docOffsetY,
                           1.5f, selectionSnapshot_.caretHeight, caretColor);
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