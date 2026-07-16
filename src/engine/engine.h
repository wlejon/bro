#pragma once

#include "engine/app_loader.h"
#include "util/asset_mounts.h"
#include "engine/css_transitions.h"
#if BRO_WITH_3D
#include "engine/gizmo.h"  // GizmoManager (3D-only; pulls scene::MeshNode)
#endif
#include "engine/gamepad.h"
#include "engine/inspector_state.h"
#include "engine/menu_bar.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "dom/node_handle.h"
#include "engine/scrollbar.h"
#include "engine/settings.h"
#include "engine/ui_layer.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "render/skia_backend.h"
#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <glad/gl.h>
#include <include/core/SkSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <quickjs.h>

typedef struct SDL_GLContextState* SDL_GLContext;

namespace bro::layout { struct KeyHandleResult; }
namespace bro::render {
    class GLContext;
    class RasterRenderer;
    class RecordingRenderer;
    class CommandReplayer;
    class CommandBuffer;
}
namespace bro::webgl { class WebGL2RenderingContext; }
namespace broaudio { class Engine; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::net { class NetService; }
namespace bro::steam { class SteamService; }
namespace bro::scene { class SceneGraph; class HtmlNode; struct CullStats; }
namespace bro::canvas { class CanvasScene; class CanvasRasterThread; }

namespace bro::platform {
    class Window;
    class EventLoop;
}
namespace bro::render { class Renderer; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; class TextNode; }
namespace bro::layout { class DrawTraversal; class SkiaTextMetrics; }

namespace bro::engine {

class FramePresenter;
class LayoutPipeline;
class AudioInference;

enum class DisplayMode { Windowed, Headless, Server };

/// Graphics/display settings configurable per app.
struct GraphicsConfig {
    int width = 1920;
    int height = 1080;
    bool useGPU = true;       // headless uses GPU by default; --no-gpu disables
    bool resizable = true;    // whether the window can be resized
    bool vsync = true;        // true = adaptive or standard vsync; false = uncapped
    double maxFrameIntervalMs = 8.0;  // layout/raster throttle (0 = uncapped)
    double maxFps = 0.0;      // present-rate cap independent of vsync (0 = uncapped)
};

/// Input behavior settings configurable per app.
struct InputConfig {
    float scrollSpeed = 48.0f;             // pixels per mouse wheel tick
    double doubleClickThresholdMs = 500.0; // max time between clicks for dblclick
    float doubleClickDistancePx = 5.0f;    // max movement between clicks for dblclick
    uint32_t overlayToggleKey = 0x40000041u; // SDLK_F8; 0 = disabled
};

struct EngineConfig {
    std::string appDir;
    std::string title;   // window title override (empty = use <title> from HTML)
    std::string settingsPath; // path to .bro_settings.json (empty = auto-detect)

    /// Project root for engine-supplied asset mounts (`/lib`, `/system`, ...).
    /// Empty when launched with just an app dir; populated from a project
    /// bro.json or the BRO_PROJECT_ROOT env var (set by parent bro processes
    /// when spawning child apps).
    std::string projectRoot;

    /// Names of the engine-supplied mount directories under projectRoot.
    /// Defaults are "lib" and "system"; override via project bro.json keys.
    /// The mounts are exposed as `/lib` and `/system` (or whatever the names
    /// are) regardless of disk directory name.
    std::string libDirName    = "lib";
    std::string systemDirName = "system";

    DisplayMode displayMode = DisplayMode::Windowed;

    /// Open the real SDL audio device even in headless mode. Default false:
    /// headless uses broaudio::Engine::initHeadless() (no playback device, no
    /// mic stream — scripts drive the mic via broaudio::Engine::injectMicSamples,
    /// exposed as bro.mic.feed). Set true to open the real device and the
    /// default recording stream — needed for reproducing live-mic behaviour
    /// from a headless script.
    bool realAudio = false;
    /// Show the startup splash screen (system/splash.html). Defaults to true
    /// for windowed, false for headless (splash is visual-only and its matrix
    /// animation leaks into early-frame screenshots if not given enough time
    /// to dismiss). Can be overridden per-app via bro.json `"splash": false`
    /// or the `--no-splash` / `--splash` CLI flags.
    bool showSplash = true;
    GraphicsConfig graphics;
    InputConfig input;
    Scrollbar::Style viewportScrollbar;    // default Scrollbar::Style
    Scrollbar::Style elementScrollbar{5.0f, 1.0f, 16.0f,
        {255,255,255,20}, {255,255,255,100}, {255,255,255,150}, {255,255,255,180}};
};

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

    /// Joins and destroys the net/Steam background service threads.
    ///
    /// NOT safe to call standalone before teardown: the JS bindings hold raw
    /// service pointers (NetBindings' per-context state keeps a NetService*
    /// and calls destroySubscriber() on it from NetBindings::cleanup), so the
    /// services must OUTLIVE the binding cleanup. Destroying them first is a
    /// use-after-free that faults or hangs on a freed condvar depending on
    /// timing. ~Engine() calls this at the one correct point — immediately
    /// after the binding cleanup block, while the JS runtime is still alive.
    /// Idempotent; a no-op after the first call.
    void stopBackgroundServices();

    /// Quiesce every worker thread and GPU context the engine owns, without
    /// destroying anything the JS runtime or DOM still points at. Idempotent.
    ///
    /// This exists because the shutdown sequence used to live at the bottom of
    /// run() — which early-returns for Headless and Server, so two of the three
    /// display modes never ran it. Notably js::shutdownAsyncJobs(), whose
    /// absence let ~AsyncJob join a model thread that was never cancelled.
    /// run() calls this on the way out; ~Engine() calls it first thing, so the
    /// sequence is identical no matter how the engine is torn down.
    void shutdown();

private:
    /// Drops the modal-move/resize event watch. Lives in engine_frame.cpp with
    /// the watcher itself (SDL_Event is a union — it can't be forward-declared
    /// here, and this header deliberately stays free of the SDL headers).
    void removeModalEventWatch();

public:

    /// Run the main event / render loop. Returns when the window is closed.
    /// In headless mode, performs initial layout and returns immediately.
    void run();

    /// Handle a window resize.
    void handleResize(int w, int h);

    /// Input events forwarded from the event loop.
    void handleMouseDown(float x, float y, int button);
    void handleMouseUp(float x, float y, int button);
    void handleMouseMove(float x, float y, float xrel, float yrel);
    void handleKeyDown(int keycode, int scancode, int mod, bool repeat);
    void handleKeyUp(int keycode, int scancode, int mod, bool repeat);
    void handleTextInput(const std::string& text);
    void handleWheel(float x, float y, float dx, float dy);

    /// Eases accumulated wheel deltas (see wheelResidualY_) into scrollY_
    /// over time. Called once per frame before layout/render.
    void drainWheelSmoothing(float frameDtSec);
    void handleDropFile(const std::string& path, float x = -1, float y = -1);
    void handleDropText(const std::string& text, float x = -1, float y = -1);

    // --- Gamepads (SDL event path; implementations in gamepad.cpp) ---
    void handleGamepadAdded(uint32_t instanceId);
    void handleGamepadRemoved(uint32_t instanceId);
    void handleGamepadButton(uint32_t instanceId, int sdlButton, bool down);
    /// `value` is normalized: sticks -1..1, triggers 0..1.
    void handleGamepadAxis(uint32_t instanceId, int sdlAxis, float value);

    // Gamepad simulation seam (headless testing — injects below the JS API and
    // above SDL, so getGamepads(), connection events, and action dispatch all
    // run the real path). Also callable in windowed mode.
    int  gamepadConnectVirtual(const std::string& id);       // returns slot index, -1 on failure
    bool gamepadDisconnectVirtual(int index);
    bool gamepadSetVirtualButton(int index, int w3cButton, bool pressed, float value);
    bool gamepadSetVirtualAxis(int index, int w3cAxis, float value);

    /// Dual-rumble request from JS (Gamepad.vibrationActuator). Magnitudes are
    /// 0..1. Real pads forward to SDL_RumbleGamepad; virtual pads just record.
    bool gamepadRumble(int index, float strongMagnitude, float weakMagnitude,
                       int durationMs);

    /// All gamepad slots (connected and not) — read by the JS bindings to
    /// build navigator.getGamepads() snapshots.
    const std::vector<GamepadState>& gamepads() const { return gamepads_; }

    // Clipboard simulation (for headless testing — bypasses system clipboard)
    void simulatePaste(const std::string& text);
    std::string simulateCopy();
    std::string simulateCut();

    float getLastMouseX() const { return lastMouseX_; }
    float getLastMouseY() const { return lastMouseY_; }

    // --- Pointer lock ---
    // requestPointerLock: freeze the reported cursor position, enable SDL relative
    // mouse mode, and route all subsequent mousemove events to `target` until
    // exitPointerLock() is called. Fires "pointerlockchange" on documentElement.
    bool requestPointerLock(dom::Element* target);
    void exitPointerLock();
    dom::Element* pointerLockElement() const { return lockedElement_.get(); }

    // --- Pointer capture (Element.setPointerCapture) ---
    // While captured, pointermove/pointerup/pointercancel dispatch to the
    // captured element regardless of the cursor's hit target (offsetX/Y
    // recomputed against it), and the capture auto-releases after pointerup —
    // the web's drag idiom, so a drag whose release lands off-element still
    // reaches the element that started it. Mouse events keep normal hit-test
    // targeting. bro synthesizes one mouse pointer (pointerId 1), so there is
    // no per-pointer table. Fires gotpointercapture / lostpointercapture.
    bool setPointerCapture(dom::Element* target);
    void releasePointerCapture(dom::Element* target);
    bool hasPointerCapture(const dom::Element* target) const {
        return target != nullptr && pointerCaptureElement_.get() == target;
    }

    // --- Document lifecycle ---
    // Tracks the HTML document.readyState. Progresses "loading" (during script
    // execution) -> "interactive" (just before DOMContentLoaded) -> "complete"
    // (just before load). Apps gate DOM measurement on this, so it must not
    // report "complete" while scripts are still running and no layout exists.
    const std::string& documentReadyState() const { return documentReadyState_; }

    // --- Page visibility / fullscreen notifications ---
    // Invoke the JS bridge to flip document.visibilityState / dispatch
    // visibilitychange / fullscreenchange. Safe to call before the JS runtime
    // is ready (no-op).
    void setPageVisibility(bool visible);
    void setFullscreenState(bool fullscreen);

    // --- Headless API (also usable in windowed mode) ---

    /// Access the document.
    dom::Document* document() const { return document_.get(); }

    /// Access the renderer.
    render::Renderer* renderer() const { return renderer_.get(); }

    /// Request an <iframe> element's sub-document be reloaded from its current
    /// src attribute (tear down + rebuild). Public: driven from JS by
    /// iframe.reload() and by assigning iframe.src. The actual teardown/rebuild
    /// is DEFERRED to the raster-idle point in the frame loop (see
    /// processPendingIframeReloads) — doing it synchronously here would free a
    /// sub-doc the raster thread may be mid-replay on. Fires "load" on rebuild.
    void reloadIframe(dom::Element* el);

    /// Read back the pixels an <iframe> sub-document last rendered into its GPU
    /// surface. Returns tightly-packed top-down RGBA8 (row 0 = top), sized to the
    /// sub-doc's content box (outW×outH); empty if `el` isn't an iframe, has no
    /// sub-document, or hasn't been rendered yet (fboTexture == 0). Runs on the
    /// main thread against the shared GL context group — the same texture the
    /// compositor samples each frame — so no cross-context readback is needed.
    /// This is the host's "look": point an iframe at a generated app, let a frame
    /// render, and read what the user sees. Callable in windowed and headless.
    std::vector<uint8_t> captureIframe(dom::Element* el, int& outW, int& outH);

    /// Access the JS runtime.
    js::Runtime* jsRuntime() const { return jsRuntime_.get(); }

    /// Access the timers.
    js::Timers* timers() const { return timers_.get(); }

    /// True if any system panel content is visible.
    bool isSystemVisible() const;

    /// Access the settings manager.
    Settings* settings() const { return settings_.get(); }

    /// Access the overlay manager (hosts dropdowns, color picker, etc.).
    OverlayManager& overlays() { return overlayMgr_; }

    /// Run pending JS jobs and re-layout if dirty.
    void flush();

    /// Advance virtual time by the given milliseconds (headless mode).
    /// In windowed mode this is a no-op.
    void advanceTime(double ms);

    /// Evaluate JS code and return the string result.
    std::string eval(const std::string& code);

    /// Render the current page to a PNG file.
    bool screenshot(const std::string& path);

    /// Render the current page and crop to the given rect before saving.
    bool screenshot(const std::string& path, int x, int y, int w, int h);

    /// Capture the current page as an RGBA pixel buffer (width x height x 4).
    /// Returns empty vector on failure.
    std::vector<uint8_t> capturePixels();

private:
    /// Unified GPU readback used by screenshot() and capturePixels(). Mirrors
    /// the windowed pipeline: rAF → render scenes → buildAppLayers →
    /// buildSystemPanelLayers → compositeLayers (into a one-shot FBO) → readback.
    /// Returns RGBA8 top-down pixels; empty vector on failure or non-GPU mode.
    std::vector<uint8_t> renderUnifiedToPixels();
public:

    /// Find an element by selector (#id shorthand or CSS selector).
    dom::Element* querySelector(const std::string& selector) const;

    /// Find an element in an overlay panel's DOM.
    dom::Element* overlayQuerySelector(const std::string& panelName,
                                       const std::string& selector) const;

    /// Get overlay panel names.
    std::vector<std::string> overlayPanelNames() const;

    /// Simulate a click on the given element.
    void dispatchClickOn(dom::Element* target);

    /// Engine-level 3D gizmo (translate / rotate / scale handles). Driven
    /// from JS via bro.gizmo.*. 3D-only.
#if BRO_WITH_3D
    GizmoManager& gizmo() { return *gizmo_; }
    const GizmoManager& gizmo() const { return *gizmo_; }

    /// Sum of frustum-culling counters across every scene graph's most
    /// recent render. Consumed by headless perf.stats().
    scene::CullStats sceneCullStats() const;
#endif

    /// Standard app menu bar (rendered by system/menu.html, driven via bro.menu.*).
    MenuBar& menuBar() { return menuBar_; }
    const MenuBar& menuBar() const { return menuBar_; }
    /// Dispatch a menu action: engine-handled IDs (__system.*) first, else app JS.
    void triggerMenuAction(const std::string& id);
    /// Invoked after bro.menu mutations — forces re-render and calls
    /// window.__onMenuChanged() in the menu panel's JS context.
    void onMenuChanged();

    /// Get display mode.
    DisplayMode displayMode() const { return displayMode_; }

    /// Get viewport dimensions.
    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

    /// Insets reserved by engine UI around the app document. Top is the menu
    /// bar; right/bottom are the inspector when docked. The app document lays
    /// out into (contentLeft, contentTop, contentWidth, contentHeight) and is
    /// drawn translated by (contentLeft, contentTop). System panels keep using
    /// the full viewport — only the app document is inset.
    struct ContentInsets { int top = 0, right = 0, bottom = 0, left = 0; };
    ContentInsets contentInsets() const;
    int contentTop() const { return contentInsets().top; }
    int contentLeft() const { return contentInsets().left; }
    int contentRight() const { return contentInsets().right; }
    int contentBottom() const { return contentInsets().bottom; }
    int contentWidth() const {
        auto i = contentInsets(); return viewportWidth_ - i.left - i.right;
    }
    int contentHeight() const {
        auto i = contentInsets(); return viewportHeight_ - i.top - i.bottom;
    }

    /// Inspector overlay (View → Inspector). Read-only access for bindings/
    /// callers; mutation happens through the inspector* methods below.
    InspectorState& inspector() { return inspector_; }
    const InspectorState& inspector() const { return inspector_; }
    void toggleInspector();
    void inspectorSetDock(InspectorDock dock);
    void inspectorSetSize(int sizePx);
    void inspectorSetPickerMode(bool on);
    void inspectorPickElement(dom::Element* el);
    /// Resolve `id` against the most recent tree/children fetch, then update
    /// `inspector_.selected`. Invalid ids are silently ignored.
    void inspectorSelectById(int id);
    /// Build a JS tree representation of the app document, rebuilding the
    /// per-fetch nodeId map. `maxDepth` < 0 means unlimited. Returns the
    /// root-element node object (with nested `children` arrays).
    JSValue inspectorBuildTreeJS(JSContext* ctx, int maxDepth);
    /// One level of children for a previously assigned nodeId. Each child
    /// gets a fresh id minted into the existing map.
    JSValue inspectorChildrenJS(JSContext* ctx, int parentId);
    /// `{ id, tag, idAttr, classes }` for the currently selected element, or
    /// JS null if there is no live selection.
    JSValue inspectorSelectedJS(JSContext* ctx);

    /// Get virtual time (headless mode).
    double virtualTime() const { return virtualTime_; }

    /// Server mode: request graceful shutdown.
    void requestServerStop() { serverStopRequested_ = true; }

    /// Server mode: get/set tick rate (ticks per second).
    double serverTickRate() const { return serverTickRate_; }
    void setServerTickRate(double hz) { serverTickRate_ = hz; }

    /// Server mode: uptime in seconds since run() started.
    double serverUptime() const;

    /// Lightweight tick: advance JS timers + pending jobs only.
    /// Used during modal blocking (window move/resize, file dialogs)
    /// to keep audio sequencer and other JS timers alive.
    void tickTimersOnly();

    /// Shared text metrics, used by layout and by JS bindings that need
    /// geometry against the live font stack (Range.getBoundingClientRect).
    layout::SkiaTextMetrics* textMetrics() const { return textMetrics_.get(); }
    /// Vertical offset the main draw pass applies to the app document,
    /// (contentTop - scrollY). Bindings that return absolute viewport
    /// coordinates add this to layout-space rects.
    float docContentOffsetY() const;

private:
    // Per-app configuration (stored for use throughout engine lifetime)
    GraphicsConfig graphicsConfig_;
    InputConfig inputConfig_;

    dom::Element* hitTest(float x, float y);

    // Find the scene graph whose canvas element is under (x, y) in screen
    // coords. Returns nullptr if none. Writes canvas-local coords (top-left
    // origin) into outLocalX/Y when it returns a graph.
    scene::SceneGraph* findSceneGraphAt(float x, float y,
                                        float& outLocalX, float& outLocalY) const;

    bool gizmoHandleMouseDown(float x, float y, int button);
    bool gizmoHandleMouseMove(float x, float y);
    bool gizmoHandleMouseUp(float x, float y, int button);

    void dispatchEvent(dom::Element* target, dom::Event& event);
    // Synthesize a DOM pointer event (pointerdown / pointerup / pointermove)
    // from the mouse event `src`, dispatched to `target`. bro has no native
    // pointer device; per the web platform a mouse drives a primary pointer,
    // and the pointer event fires just before its mouse analog. Call this
    // immediately before the corresponding mouse dispatch.
    void dispatchPointerAlias(const char* type, dom::Element* target,
                              const dom::MouseEvent& src);
    void pumpVideoEvents();
    /// Translate a window-space mouse y into the active overlay's coordinate
    /// space. App-context overlays (dropdown, color picker) anchor in app
    /// content space — window minus the engine-reserved top inset; System
    /// overlays keep raw window coords. This is the single input-side
    /// boundary where the inset is folded for overlays.
    float overlayMouseY(float y) const;
    void applyKeyResult(dom::Element* el, const layout::KeyHandleResult& r);
    void dispatchInputEvent(dom::Element* el, const std::string& data = "",
                            const std::string& inputType = "");
    void dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget);
    void dispatchScrollEvent(dom::Element* el);

    // World-space HtmlNode mouse routing. Returns the SceneGraph attached
    // to `el` if any, or null. Scene-anchored hit testing only kicks in
    // for the canvas element that owns a graph; everything else passes
    // through to the standard DOM dispatch path.
    scene::SceneGraph* sceneGraphForElement(const dom::Element* el) const;
    bool elementAbsoluteOrigin(dom::Element* el, float& outX, float& outY) const;
    // Pick the HtmlNode under (docX, docY) when the DOM hit test landed on
    // `canvasEl` and that element owns a SceneGraph. Returns true on hit
    // and writes the picked node + element + local CSS pixel coords.
    bool pickHtmlNodeUnderMouse(dom::Element* canvasEl, float docX, float docY,
                                scene::HtmlNode*& outNode, dom::Element*& outEl,
                                float& outLocalPxX, float& outLocalPxY);
    // Dispatch one synthesized mouse event into a HtmlNode's detached
    // document. Caller has already resolved the inner element and local
    // CSS pixel coords. `relatedTarget` is for over/out semantics.
    void dispatchHtmlNodeMouseEvent(const std::string& type,
                                    dom::Element* target,
                                    float localPxX, float localPxY,
                                    int button, int pressedButtons, int mods,
                                    float movX, float movY, bool bubbles,
                                    dom::Element* relatedTarget = nullptr);
    void advanceFocus(bool reverse);
    void addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene);
    void drawTexturedQuad(GLuint tex, float x, float y, float w, float h);
    /// Composite a layer set into `targetFBO`. `offsetY`/`layerW`/`layerH`
    /// place the set: app layers are content-sized and recorded in content
    /// space, so they composite at (0, insetTop) with content dimensions —
    /// the single boundary where the engine-reserved inset enters the frame.
    /// System-panel layer sets pass the defaults (full viewport at (0, 0)).
    void compositeLayers(const std::vector<UILayer>& layers, GLuint targetFBO = 0,
                         int offsetY = 0, int layerW = -1, int layerH = -1);

    /// Walk the app document and emit draw commands into `outBuffer`.
    /// Run on the main thread after layout. The buffer is then handed to the
    /// raster thread to replay against its Skia renderer. Layer-break and
    /// inline-canvas commands sit inline in the buffer; the replayer's
    /// handlers manage GPU surface pools at replay time.
    // promotedSet != nullptr enables compositor-layer splitting: with
    // promotedOnly=false the base is recorded skipping promoted subtrees
    // (holes); with promotedOnly=true ONLY the promoted subtrees are recorded
    // (for the separate on-top layer). promotedSet==nullptr is the default
    // full single-pass record (headless + any non-promoted path), unchanged.
    void recordAppLayers(render::CommandBuffer& outBuffer,
                         int vpW, int vpH,
                         int insetTop, int insetRight, int insetBottom,
                         float scrollY,
                         const std::unordered_set<dom::Element*>* promotedSet = nullptr,
                         bool promotedOnly = false);

    /// Replay the previously-recorded app command buffer against `renderer`,
    /// producing UILayers as a side-effect of layer-break commands. Run on
    /// the raster thread (windowed) or main thread (headless). surfW/surfH
    /// are the app *content* dimensions (viewport minus engine insets) —
    /// app layer surfaces are content-sized; the compositor places them.
    // promotedBuffer != nullptr (and non-empty) is replayed after the base into
    // one extra pool surface and appended as the topmost HTML UILayer — the
    // compositor-promoted layer that sits above the cached base.
    void replayAppLayers(render::SkiaRenderer* renderer,
                         const render::CommandBuffer& buffer,
                         std::vector<render::SkiaRenderer::GPUSurface>& pool,
                         int& poolW, int& poolH,
                         int surfW, int surfH,
                         std::vector<UILayer>& outLayers,
                         const render::CommandBuffer* promotedBuffer = nullptr);

    /// Walk the visible system-panel documents and emit draw commands.
    /// One bundle of commands per visible panel, separated by Cmd_LayerBreak
    /// (kind=HTMLPanel) so the replayer can split them onto separate GPU
    /// surfaces. Inline canvas blits use Cmd_BlitCanvasInline.
    void recordSystemPanelLayers(render::CommandBuffer& outBuffer,
                                 int vpW, int vpH);

    /// Replay system-panel commands against `renderer`, producing one HTML
    /// UILayer per panel.
    void replaySystemPanelLayers(render::SkiaRenderer* renderer,
                                 const render::CommandBuffer& buffer,
                                 std::vector<render::SkiaRenderer::GPUSurface>& pool,
                                 int& poolW, int& poolH,
                                 int vpW, int vpH,
                                 std::vector<UILayer>& outLayers);
    void ensureReplacedElements(dom::Element* elem);

    // --- System panel management (implementation in system_panels.cpp) ---
    // System panels are ordinary HTML documents rendered through the same
    // layout/raster pipeline as the app document. They share the engine's
    // textMetrics_ for layout, and in windowed mode the raster thread draws
    // each visible panel into its own GPU surface.
    struct SystemDocument {
        std::string name;
        std::string tabLabel;
        std::string group;
        bool active = true;
        JSContext* jsCtx = nullptr;
        std::unique_ptr<js::Timers> timers;
        // 2D canvas contexts owned by this panel (inline-blit at layer break).
        // Declared before `document` so it is destroyed *after* it: the panel's
        // Elements (owned by `document`) fire CanvasScene::onElementFinalized via
        // their on-destroy hook, which must run while the scenes are still alive.
        std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
        std::unique_ptr<dom::Document> document;
        JSValue broPerfObj = JS_UNDEFINED;
        MouseDispatchState mouseState;  // per-doc click/dblclick tracking
    };

    // An isolated sub-document hosted by an <iframe> element in the app document.
    // Owns its own JS realm, DOM tree, timers, canvas scenes, and input state —
    // structurally identical to a SystemDocument, but bound to a DOM element and
    // laid out at that element's content box instead of an engine overlay slot.
    struct IframeDoc {
        dom::Element* element = nullptr;  // the <iframe> in the app document (non-owning)
        uint64_t id = 0;                  // registry id for compositor texture resolve
        std::string src;                  // resolved src currently loaded (change detection)
        JSContext* jsCtx = nullptr;
        std::unique_ptr<js::Timers> timers;
        // canvasScenes MUST precede `document`: Elements (owned by document) fire
        // CanvasScene::onElementFinalized on destroy, which needs the scenes alive.
        // Member destruction is reverse-declaration order, so this destructs last.
        std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
        std::unique_ptr<dom::Document> document;
        MouseDispatchState mouseState;    // per-doc click/dblclick tracking
        dom::Element* hoveredElement = nullptr; // sub-doc :hover target (non-owning)
        int boxW = 0, boxH = 0;           // last content-box size laid out
        // Render target: the sub-document paints into cmdBuffer (main thread),
        // which is replayed into `surface` (raster thread) → fboTexture, which
        // the app compositor draws at the <iframe> element's box.
        render::CommandBuffer cmdBuffer;
        render::SkiaRenderer::GPUSurface surface;
        int surfW = 0, surfH = 0;
        unsigned int fboTexture = 0;
    };

    void initSystemPanels();
    void loadCustomFonts();
    void destroySystemPanels();
    void loadSystemPanels(const std::string& systemDir);
    void scanSystemPanelDir(const std::string& baseDir, const std::string& relPath);
    void installBroObject(SystemDocument& doc);
    bool isSystemDocVisible(const SystemDocument& doc) const;
    void toggleSystemPerf();
    void toggleSystemSettings();
    void showSystemPanel(const std::string& name);
    void tickSystemPanels(double nowMs);
    void updateSystemPerf(double fps, double frameTime, double js, double layout,
                          double raster, double gpu, double draw, int vpW, int vpH);
    void layoutSystemPanels(layout::SkiaTextMetrics& metrics);
    void drawSystemPanels(render::Renderer* renderer,
                          layout::DrawTraversal& traversal);
    /// Render one system panel's document tree into the current surface of
    /// `renderer`. Handles basePath setup, the inline canvas-blit callback,
    /// the main draw traversal, and overflow scrollbars — all the per-doc
    /// work that's common between windowed mode (which per-panel switches
    /// GPU surfaces beforehand) and headless mode (which draws straight onto
    /// the main target). Keeping both paths funneled through one call site
    /// means decorations added here (scrollbars, badges, outlines) show up
    /// everywhere without duplication drift.
    void drawSystemPanelDoc(render::Renderer* renderer,
                            layout::DrawTraversal& traversal,
                            SystemDocument& doc,
                            int vpW, int vpH);
    /// Main thread: swap each visible system-panel CanvasScene's recorded
    /// commands into its staged buffer, so the raster thread can replay them
    /// without racing with JS that keeps pushing new commands. Must be called
    /// only when the raster thread is idle.
    void stageSystemPanelCanvases();
    void resizeSystemPanels(int w, int h);
    dom::Element* systemHitTest(SystemDocument& doc, float x, float y);
    bool systemHandleMouseDown(float x, float y, int button);
    bool systemHandleMouseUp(float x, float y, int button);

    // ── Iframe sub-documents (src/engine/iframe.cpp) ────────────────────────
    // Reconcile iframeDocs_ with the <iframe> elements in the app document:
    // create sub-docs for new src'd iframes, tear down removed ones, reload on
    // src change. Called after app load and after DOM mutations.
    void syncIframes();
    void createIframeDoc(dom::Element* el, const std::string& srcAttr);
    void teardownIframeDoc(IframeDoc* doc);
    void destroyAllIframes();
    // Tick each iframe sub-document's timers + rAF. Returns true if any sub-doc
    // needs (re)recording this frame — freshly reloaded, DOM-mutated, or
    // animating — so the caller can fold that into uiDirty_ (iframe activity has
    // no other route to the raster thread).
    bool tickIframes(double nowMs);
    IframeDoc* iframeDocById(uint64_t id);
    IframeDoc* iframeDocForElement(const dom::Element* el);
    // Record each iframe sub-document's paint into its own command buffer (main
    // thread), then replay each into a box-sized GPU surface → fboTexture
    // (raster thread). The app compositor draws those textures via UILayer::
    // Iframe breaks emitted while recording the app document.
    // Process queued iframe.reload() requests: tear each sub-doc down and
    // rebuild it from src. MUST be called only at the raster-idle point in the
    // frame loop (alongside recordIframeLayers), never from JS — it mutates
    // iframeDocs_ and frees sub-doc state the raster thread replays.
    void processPendingIframeReloads();
    void recordIframeLayers();
    void replayIframeLayers(render::SkiaRenderer* renderer);
    /// Hand an orphaned iframe GPU surface to whoever owns its GL context, to be
    /// destroyed there.
    ///
    /// An IframeDoc's surface is created by whichever renderer replays the
    /// sub-doc: the RASTER thread's windowed, the MAIN one headless (there is no
    /// raster thread there, so screenshot() replays inline). It can only be
    /// destroyed on that same context — the FBO is a GL container object, which
    /// unlike the texture is NOT shared across the context share group, and the
    /// sk_sp<SkSurface> holds a ref to that context. Deleting the FBO from the
    /// wrong thread is not an error, just a silent no-op that leaks it.
    ///
    /// So main-thread code that drops a surface (a reload whose rebuild failed)
    /// must route it through here rather than letting it destruct. Push only at
    /// the raster-idle point in the frame loop — the same invariant
    /// processPendingIframeReloads() already runs under, and what makes the
    /// unlocked handoff safe (the FramePresenter request/publish handshake
    /// orders these writes against the drain).
    void queueIframeSurfaceFree(render::SkiaRenderer::GPUSurface&& surf);
    /// Destroy every surface queued above, using the renderer that owns them.
    /// Called from replayIframeLayers() — whichever renderer replays the
    /// sub-docs is by construction the one that created their surfaces — and
    /// again from the raster thread's exit cleanup and ~Engine(), which are the
    /// last points the windowed and headless contexts respectively still exist.
    void drainIframeSurfaceFrees(render::SkiaRenderer* renderer);
    // Route a host mouse event that landed on an <iframe> element into its
    // sub-document: translate document-space coords to the sub-doc's own content
    // space, hit-test the sub-doc, and dispatch through the shared per-doc mouse
    // helpers against the iframe's isolated document/JS/mouseState. Return true
    // when the point lies inside the iframe box (event consumed).
    dom::Element* iframeHitTest(IframeDoc* dp, float localX, float localY);
    bool iframeHandleMouseDown(dom::Element* frameEl, float docX, float docY,
                               int button, float movementX, float movementY, int mod);
    bool iframeHandleMouseUp(dom::Element* frameEl, float docX, float docY,
                             int button, float movementX, float movementY, int mod);
    bool iframeHandleMouseMove(dom::Element* frameEl, float docX, float docY,
                               float movementX, float movementY, int mod);
    bool systemHandleMouseMove(float x, float y);
    /// Wheel scrolling on a system panel's overflow element. Returns true if
    /// some element was scrolled; caller should skip app-level wheel handling.
    bool systemHandleWheel(float x, float y, float dx, float dy);
    /// Recurse the given subtree drawing a scrollbar thumb for every element
    /// with overflow-y: auto|scroll that actually has clipped content. Shared
    /// by the app-doc draw pass and per-system-panel drawing so modals and
    /// overlays get scrollbars with no code duplication.
    void drawElementScrollbars(render::Renderer* renderer,
                               dom::Element* root,
                               float offsetX, float offsetY);
    /// Snapshot the Selection's geometry (highlight rects + optional caret)
    /// into selectionSnapshot_. Must run on the main thread because it reads
    /// live Range/Node pointers that JS can mutate. Call before signaling the
    /// raster thread — drawSelectionHighlight consumes the snapshot without
    /// touching the DOM.
    void updateSelectionSnapshot();
    /// Draw the document's Selection highlight (semi-transparent rectangles
    /// behind the selected text runs). `docOffsetY` is the pass's
    /// document→surface translation — (−scrollY) for the app document, which
    /// draws in content space. Reads selectionSnapshot_, so safe on the
    /// raster thread. No-op when the selection is empty.
    void drawSelectionHighlight(render::Renderer* renderer, float docOffsetY);
    /// Route a keydown/keyup to visible system panels (settings modal, etc.)
    /// so its JS can capture keys. Returns true if the modal is active, in
    /// which case the app does NOT see the key — modal panels are meant to
    /// fully capture input while open.
    bool systemHandleKeyDown(int keycode, int scancode, int mod, bool repeat);
    bool systemHandleKeyUp(int keycode, int scancode, int mod, bool repeat);

    /// Raster thread entry point (windowed mode only).
    void rasterThreadFunc();

    /// Layout thread entry point (windowed mode only).
    void layoutThreadFunc();

    DisplayMode displayMode_;

    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<render::GLContext> gl_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<js::Runtime> jsRuntime_;
    std::unique_ptr<js::Timers> timers_;
    std::unique_ptr<dom::Document> document_;
    TransitionManager transitionManager_;
    AnimationManager animationManager_;

    // Elements the layout-thread tick promoted to compositor layers this frame
    // (active transform/opacity-only animation/transition). Written by
    // layoutThreadFunc, read by the main thread's record pass after the layout
    // barrier (waitClaimDone) establishes happens-before. Drives which subtrees
    // the base record skips and re-records separately as promoted layers.
    std::unordered_set<dom::Element*> promotedElements_;

    // Cached base app command buffer — the static UI minus promoted subtrees
    // (DrawTraversal PaintMode::BaseSkipPromoted). Rebuilt only on a real base
    // change (DOM/style/hover/scroll/resize, or the promoted set changing);
    // reused verbatim across promoted-only animation frames so those frames pay
    // no main-thread record cost. Written by the main thread inside the
    // isRasterIdle-gated record block, read by the raster thread after
    // signalRender — same happens-before the old per-slot appCommands had.
    render::CommandBuffer baseCommands_;
    // The promoted set baseCommands_ was recorded against; when the live
    // promotedElements_ differs, the base's skip-holes are stale and it must be
    // rebuilt. baseValid_ is false until the first base record.
    std::unordered_set<dom::Element*> basePromotedSet_;
    bool baseValid_ = false;
    // Narrow "app base content changed" signal — set only on a genuine app
    // change (document dirty, promoted-set change), NOT on the broad uiDirty_
    // (which also fires for splash/menu/overlay animation and stats refresh).
    // Gating the base re-record on this is what keeps a lone spinner from
    // re-recording the whole 4k-element DOM every frame.
    bool appBaseDirty_ = false;
    // Force the cached HTML base to be re-recorded next frame *without* a full
    // relayout. For interaction state that feeds the base record but changes
    // neither computed styles nor box geometry: text selection / caret,
    // element scroll offset (applied at draw time, not layout time), and the
    // base-only chrome recorded in engine_compositor (dropdown overlays,
    // inspector highlight, scrollbars). Before the base was cached these
    // repainted for free — every uiDirty_ frame did a full re-record — so a
    // handler only had to set uiDirty_. Now the record reuses the cache unless
    // appBaseDirty_ says otherwise, so those handlers must call this. Focus
    // changes go through Document::markDirty() instead: they can alter :focus
    // styling, so they want the restyle + relayout, not just a re-record.
    void markAppBaseDirty() { appBaseDirty_ = true; uiDirty_ = true; }
    // Scroll + insets the cached base was recorded under; a change invalidates
    // the cache (both are baked into the recorded commands).
    float baseScrollY_ = 0.0f;
    int baseInsetTop_ = -1, baseInsetRight_ = -1, baseInsetBottom_ = -1;

    // Content dimensions the layout tree was last laid out at (layout-thread
    // owned). A promoted-only frame skips the full layoutTree() pass since
    // transform/opacity are paint-only — but a viewport/inset resize still must
    // re-lay-out even when the DOM is otherwise clean, so we compare against
    // these. -1 forces the first pass.
    int lastLayoutContentW_ = -1, lastLayoutContentH_ = -1;

    // Loaded custom font data for registering on layout thread's renderer
    struct LoadedFont {
        std::string family;
        std::vector<char> data;
        int weight;
        bool italic;
    };
    std::vector<LoadedFont> loadedFonts_;
    // Recorder for the main-thread paint walk. Wraps renderer_ for synchronous
    // queries (measureText, createFont) but appends mutating calls to a
    // CommandBuffer instead of issuing Skia work. drawTraversal_ paints
    // through this so the raster thread can replay the buffer without
    // touching the DOM.
    std::unique_ptr<render::RecordingRenderer> recordingRenderer_;
    std::unique_ptr<layout::DrawTraversal> drawTraversal_;
    std::unique_ptr<layout::SkiaTextMetrics> textMetrics_;
    std::unique_ptr<platform::EventLoop> eventLoop_;

    // Selection geometry snapshot — computed on the main thread from live
    // Range/Node pointers, then consumed by the raster thread without
    // touching the DOM. See updateSelectionSnapshot / drawSelectionHighlight.
    struct SelectionSnapshot {
        struct Rect { float x, y, w, h; };
        std::vector<Rect> rects;
        bool hasCaret = false;
        float caretX = 0, caretY = 0, caretHeight = 0;
    };
    SelectionSnapshot selectionSnapshot_;

    bool running_ = false;
    bool shutdownDone_ = false;  // shutdown() guard — run() and ~Engine() both call it
    int viewportWidth_;
    int viewportHeight_;

    // Pre-compiled observer check function (avoids JS_Eval parse per frame)
    JSValue observerCheckFn_ = JS_UNDEFINED;
    AppManifest manifest_;
    util::AssetMounts assetMounts_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes_;
    // Detached scenes awaiting destruction. A scene whose element was removed
    // is moved here (out of canvasScenes_) and unregistered from
    // canvasSceneRegistry_ — from that moment every UILayer or command that
    // still names it by sceneId resolves to null. Actual destruction is still
    // deferred to frame top with the raster worker idle, because inline-blit
    // commands replayed on the raster thread hold the raw pointer. See the
    // drain in Engine::run().
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenesDetached_;
    // Main-thread-only resolver for UILayer::canvasSceneId /
    // Cmd_LayerBreak::canvasSceneId. Insert on scene adoption, erase on
    // detach. The raster thread never touches this map — it only copies ids.
    std::unordered_map<uint64_t, canvas::CanvasScene*> canvasSceneRegistry_;
    canvas::CanvasScene* canvasSceneById(uint64_t id) const {
        if (!id) return nullptr;
        auto it = canvasSceneRegistry_.find(id);
        return it == canvasSceneRegistry_.end() ? nullptr : it->second;
    }
    // Shared canvas-raster worker (windowed): one persistent GL context + thread
    // that rasterizes every threaded CanvasScene. Created once at run() start so
    // canvas churn never creates/destroys GL contexts on the hot path.
    std::unique_ptr<canvas::CanvasRasterThread> canvasRasterThread_;

    // WebGL contexts (owned by engine, associated with canvas elements)
    struct WebGLEntry {
        std::unique_ptr<webgl::WebGL2RenderingContext> context;
        dom::Element* element = nullptr;  // non-owning
    };
    std::vector<WebGLEntry> webglEntries_;

    // --- Threaded rasterization / layout ---
    // FramePresenter owns the raster thread's synchronization (state machine,
    // fence handshake, double-buffered layer lists) and the snapshot atomics
    // the main thread hands the worker each frame. LayoutPipeline does the
    // same for the layout worker. Both forward-declared above so engine.h
    // doesn't pull in the implementations — see frame_presenter.h /
    // layout_pipeline.h.
    std::unique_ptr<FramePresenter> framePresenter_;
    std::unique_ptr<LayoutPipeline>  layoutPipeline_;

    std::atomic<bool> rasterReady_{false};
    std::thread       rasterThread_;
    std::thread       layoutThread_;
    SDL_GLContext     rasterGLContext_ = nullptr;

    // Double-buffered pools of reusable GPU-backed Skia surfaces for HTML
    // layers — one pool per frame-buffer index (matching FramePresenter's
    // front_/back). The raster thread draws into the *back* pool while the
    // main-thread compositor samples the *front* pool. A single shared pool
    // aliased the in-flight raster frame's textures with the frame being
    // composited: the compositor sampled surfaces the raster thread was
    // concurrently clearing+redrawing (switchSurface clears to transparent),
    // so every dirty frame flashed the whole HTML layer → heavy flicker, and
    // animations looked frozen because no clean frame was ever sampled. The
    // consumeIfReady() fence only becomes correct once the pools don't alias.
    // Owned by the raster thread — FBOs are per-context but textures are
    // shared across GL contexts for compositing on the main thread.
    std::vector<render::SkiaRenderer::GPUSurface> htmlSurfacePool_[2];
    int htmlSurfacePoolW_[2] = {0, 0}, htmlSurfacePoolH_[2] = {0, 0};
    // Parallel pools, one entry per visible system panel per frame. Separate
    // from htmlSurfacePool_ so app layer-break sizing can't invalidate panel
    // surfaces mid-frame; double-buffered for the same reason as above.
    std::vector<render::SkiaRenderer::GPUSurface> systemSurfacePool_[2];
    int systemSurfacePoolW_[2] = {0, 0}, systemSurfacePoolH_[2] = {0, 0};

    // Headless screenshot path uses the main thread's renderer + GL context,
    // but needs its own surface pool so it doesn't fight the raster thread's
    // pool on size invalidation. Lives only in headless mode.
    std::vector<render::SkiaRenderer::GPUSurface> screenshotHtmlPool_;
    int screenshotHtmlPoolW_ = 0, screenshotHtmlPoolH_ = 0;
    std::vector<render::SkiaRenderer::GPUSurface> screenshotSystemPool_;
    int screenshotSystemPoolW_ = 0, screenshotSystemPoolH_ = 0;

    MenuBar menuBar_;
    InspectorState inspector_;
    // Per-tree-fetch element ↔ id map. Rebuilt every getAppDOMTree() so ids
    // never outlive a fetch. Selection survives by re-resolving the element
    // pointer to a fresh id when the panel re-fetches.
    std::unordered_map<int, dom::Element*> inspectorNodeMap_;
    int inspectorNextId_ = 0;
#if BRO_WITH_3D
    std::unique_ptr<GizmoManager> gizmo_;
#endif
    OverlayManager overlayMgr_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<broaudio::Engine> audioEngine_;
    std::unique_ptr<AudioInference> audioInference_;

    // Per-frame main-thread service hooks. An optional subsystem registers its
    // result-delivery pump here at install time (inside its BRO_WITH_* guard);
    // the frame loop iterates them once per tick. A subsystem compiled out never
    // registers — so there is no dead call and no stub in the hot path. Written
    // once during init and only iterated on the main thread thereafter, so it
    // needs no synchronisation.
    std::vector<std::function<void()>> framePumps_;
#if BRO_WITH_PHYSICS
    std::unique_ptr<physics::PhysicsWorld> physicsWorld_;
#endif
#if BRO_WITH_NET
    std::unique_ptr<net::NetService> netService_;
#endif
    std::unique_ptr<steam::SteamService> steamService_;
#if BRO_WITH_3D
    struct SceneGraphEntry {
        std::unique_ptr<scene::SceneGraph> graph;
        dom::Element* element = nullptr;  // non-owning
    };
    std::vector<SceneGraphEntry> sceneGraphs_;
#endif
    double physicsAccumMs_ = 0.0;
    double lastPhysicsTimeMs_ = 0.0;
    double lastFrameTimeMs_ = 0.0; // wall-clock time of previous frame's start (for syncAgents dt)
    // System panels (settings, perf, nav)
    std::vector<SystemDocument> systemDocs_;
    // Iframe sub-documents, keyed by their <iframe> element. unique_ptr so the
    // IframeDoc address is stable (captured by canvas getContext factories and
    // referenced from dom::Element::iframeDoc()).
    std::vector<std::unique_ptr<IframeDoc>> iframeDocs_;
    uint64_t nextIframeId_ = 1;
    // <iframe> elements whose sub-document JS requested a reload this frame.
    // reloadIframe() only queues here; the actual teardown+rebuild (which frees
    // a sub-doc's JS/DOM/canvas scenes the raster thread may still be replaying)
    // runs in processPendingIframeReloads() at the raster-idle point in the
    // frame loop — never on the JS thread mid-render. Non-owning element ptrs.
    std::vector<dom::Element*> pendingIframeReloads_;
    // Iframe GPU surfaces orphaned on the main thread (a reload whose rebuild
    // failed), awaiting destruction on the raster thread that created them —
    // see queueIframeSurfaceFree(). Unlocked: pushes happen only at the
    // raster-idle point, drains only on the raster thread.
    std::vector<render::SkiaRenderer::GPUSurface> iframeSurfaceFrees_;
    // An <iframe> may have been added to or removed from the app document since
    // the last syncIframes(). Set where the main thread observes structureDirty_
    // (before the layout pass clears it) and consumed at the raster-idle point,
    // which is the only place sub-docs may be created or destroyed. Without it
    // syncIframes() would have to re-walk the whole DOM every frame.
    bool iframeSyncNeeded_ = false;
    // <iframe> elements whose src failed to load, and the src that failed.
    // syncIframes() runs on every DOM structure change and builds a sub-doc for
    // any src'd iframe that hasn't got one — so without this, one bad src re-hits
    // the filesystem and re-logs its error on every mutation. An explicit
    // reload()/src= clears the entry (that's a request to retry); so does the
    // element leaving the tree, which also keeps reused Element* addresses from
    // colliding with a stale record.
    std::unordered_map<dom::Element*, std::string> iframeLoadFailed_;
    bool systemPerfVisible_ = false;
    bool systemSettingsVisible_ = false;
    bool splashVisible_ = false;
    bool splashEnabled_ = true;   // from EngineConfig::showSplash
    bool splashDismissTriggered_ = false;
    double splashStartMs_ = 0.0;
    double lastSystemRafMs_ = 0.0;
    bool systemDirty_ = true;
    bool systemMouseConsumed_ = false;
    std::string systemActivePanel_;
    dom::ElementHandle systemHoverTarget_;
    SystemDocument* systemHoverDoc_ = nullptr;

    // Headless-specific
    double virtualTime_ = 0.0;

    // Server-specific
    double serverTickRate_ = 60.0;    // ticks per second
    double serverStartTime_ = 0.0;   // wall-clock start time (ms)
    bool serverStopRequested_ = false;

    // Stats tracking
    double statsAccumMs_ = 0.0;
    int statsFrameCount_ = 0;
    double statsFps_ = 0.0;
    double statsFrameTimeMs_ = 0.0;
    double statsMinFrameMs_ = 999.0;
    double statsMaxFrameMs_ = 0.0;
    double totalFrameMs_ = 0.0;
    bool uiDirty_ = true;
    bool hasRenderedOnce_ = false;
    bool mediaEventsArmed_ = false;

    // Hover tracking for mouseenter/mouseleave/mouseover/mouseout
    dom::ElementHandle hoveredElement_;

    // World-space HtmlNode hover + press tracking. When a click lands on a
    // canvas with a SceneGraph, the engine ray-casts into the scene's
    // HtmlNode billboards and routes mouse events into the detached
    // document. These mirror hoveredElement_ but for the inner doc.
    scene::HtmlNode*   hoveredHtmlNode_ = nullptr;
    dom::ElementHandle hoveredHtmlElement_;
    scene::HtmlNode*   htmlNodeMouseDownNode_ = nullptr;
    dom::ElementHandle htmlNodeMouseDownElement_;

    // Mouse tracking for mousemove movement deltas
    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;

    // Pointer lock: while set, hit-testing/hover is frozen and mousemove events
    // are routed to lockedElement_ with clientX/Y pinned to lockedMouse{X,Y}_.
    // Only movementX/Y (SDL xrel/yrel) reflect the actual motion.
    dom::ElementHandle lockedElement_;
    float lockedMouseX_ = 0.0f;
    float lockedMouseY_ = 0.0f;

    // Pointer capture: while held, pointer events (not mouse events) route to
    // this element. Cleared by pointerup/pointercancel, an explicit
    // releasePointerCapture(), or a buttons-free pointermove (self-heal when
    // the release was never delivered).
    dom::ElementHandle pointerCaptureElement_;

    // HTML document.readyState. Starts "loading" while user scripts execute,
    // advances to "interactive"/"complete" as DOMContentLoaded/load dispatch.
    std::string documentReadyState_ = "loading";

    // Per-document mouse dispatch state for the app doc (mousedown target +
    // rolling click/dblclick tracking). System docs carry their own instance.
    MouseDispatchState appMouseState_;

    // Mouse button state for buttons bitmask
    int pressedButtons_ = 0;

    // Gamepad slots (see engine/gamepad.h). Helpers live in gamepad.cpp.
    std::vector<GamepadState> gamepads_;
    GamepadState* gamepadByInstance(uint32_t instanceId);
    GamepadState* connectedGamepadAt(int index);
    GamepadState& allocateGamepadSlot();
    void gamepadButtonChanged(GamepadState& gp, int w3cIndex, float value);
    void dispatchGamepadConnectionEvent(const GamepadState& gp, bool connected);
    void closeAllGamepads();

    // Modifier keys currently held via simulated handleKeyDown()/handleKeyUp()
    // calls (SDL_KMOD_* bits). safeGetModState() only sees the OS's real
    // physical keyboard state, which headless input simulation never touches —
    // this mask lets simulated keyDown(shift)+click() combinations (e.g.
    // shift-click) produce a MouseEvent with the correct shiftKey/ctrlKey/etc.
    // See safeGetModState() in input_handling.cpp.
    int heldModifierMask_ = 0;

    /// Current modifier bits: the OS keyboard, plus any modifier held down via
    /// simulated key events (headless). See safeGetModState in input_handling.
    int currentModState() const;

    // Mouse-driven text selection. `selectionAnchor*` is pinned on mousedown
    // (the static endpoint of a drag); selectionDragging_ means subsequent
    // mousemove events should extend the focus to follow the cursor.
    bool selectionDragging_ = false;
    dom::TextNodeHandle selectionAnchorNode_;
    int selectionAnchorOffset_ = 0;
    // Press position in document space, used to gate selection extension until
    // the pointer has moved far enough that the user intends a drag (rather
    // than a click with incidental sub-pixel motion).
    float selectionPressX_ = 0.0f;
    float selectionPressY_ = 0.0f;
    bool  selectionPastThreshold_ = false;

    // Drag-selection inside a text control (<input> / <textarea>). Those manage
    // their own selection rather than the document's, so the engine only tracks
    // which control the press landed in — the control itself holds the anchor.
    // A panel's controls draw in window space and the app document's in content
    // space, so the space the drag point must be given in depends on which
    // document the control belongs to.
    dom::ElementHandle controlDragElement_;
    bool controlDragIsPanel_ = false;

    // Viewport scrolling
    float scrollY_ = 0.0f;
    float documentHeight_ = 0.0f;
    // Pending wheel-scroll deltas, drained with exponential easing each
    // frame (see Engine::drainWheelSmoothing). macOS trackpad momentum
    // phases emit events at irregular intervals with decaying magnitudes
    // — applying them directly produces visible jitter; smoothing over a
    // handful of frames yields steady deceleration.
    float wheelResidualY_ = 0.0f;

    // Scrollbar components (styles from config)
    Scrollbar viewportScrollbar_;
    Scrollbar elementScrollbar_;
    bool draggingViewportScrollbar_ = false;
    dom::ElementHandle scrollbarDragTarget_;
    dom::ElementHandle scrollbarHoveredElement_;
    /// When non-null, scrollbarDragTarget_ belongs to this system panel
    /// document rather than the app document, and the drag-update code
    /// dispatches scroll events through the panel's JS context. Cleared
    /// when the drag ends.
    SystemDocument* scrollbarDragSystemDoc_ = nullptr;

    // UI render throttle — layout+rasterize at most every N ms (from config)
    double uiFrameIntervalMs_ = 8.0;
    double lastUIRenderMs_ = 0.0;

    // Present-rate cap (graphics.maxFps) — the main loop sleeps so a frame never
    // completes faster than this, independent of vsync. 0 = uncapped (vsync
    // governs). This is the app-settable "cap other than vsync".
    double frameCapIntervalMs_ = 0.0;
    // Whether the window currently has input focus. Windows stops pacing
    // wglSwapBuffers for unfocused windows, so the loop would free-run and
    // judder; while unfocused we clamp the present rate to kUnfocusedFps.
    bool windowFocused_ = true;
    static constexpr double kUnfocusedFps = 30.0;

    // QuickJS cycle-collector GC — run periodically to free cyclic garbage
    static constexpr double kGCIntervalMs = 1000.0;
    double lastGCMs_ = 0.0;

    // Per-phase timing (smoothed over stats window)
    double phaseJsMs_ = 0.0;       // JS execution (rAF + pending jobs)
    double phaseLayoutMs_ = 0.0;   // layout
    double phaseRasterMs_ = 0.0;   // Skia rasterization + upload
    double phaseGpuMs_ = 0.0;      // GL composite + swap
    double phaseGlStateMs_ = 0.0;  // GL state save/restore
    // Raster sub-phase timing
    double phaseDrawMs_ = 0.0;     // draw (Skia commands)
    double phaseUploadMs_ = 0.0;   // texture upload to GPU
    // Accumulators for averaging
    double accumJsMs_ = 0.0;
    double accumLayoutMs_ = 0.0;
    double accumRasterMs_ = 0.0;
    double accumGpuMs_ = 0.0;
    double accumGlStateMs_ = 0.0;
    double accumDrawMs_ = 0.0;
    double accumUploadMs_ = 0.0;

    // UI overlay quad (OpenGL) — unsigned int to avoid including glad/gl.h
    unsigned int uiQuadVAO_ = 0;
    unsigned int uiQuadVBO_ = 0;
};

} // namespace bro::engine
