#pragma once

#include "engine/app_loader.h"
#include "util/asset_mounts.h"
#include "engine/css_transitions.h"
#if BRO_WITH_3D
#include "engine/gizmo.h"  // GizmoManager (3D-only; pulls scene::MeshNode)
#endif
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
namespace bro::scene { class SceneGraph; class HtmlNode; }
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

    /// Joins and destroys the net/Steam background service threads without
    /// touching the JS runtime, document, or GPU state — the rest of
    /// ~Engine() is intentionally skipped in headless mode (see
    /// src/headless/main.cpp) because tearing down QuickJS there has hit a
    /// GC-leak assertion, and abandoning worker threads outright via _exit()
    /// has been implicated in OS-level thread-rundown bugchecks (unrelated
    /// to that QuickJS issue). netService_/steamService_ have no ordering
    /// dependency on jsRuntime_/audioEngine_ teardown (their destructors are
    /// a plain thread join), so this is safe to call standalone right before
    /// an abrupt process exit to shrink the number of live threads at that
    /// moment. Safe to call multiple times or skip; a no-op after the first.
    void stopBackgroundServices();

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
    void recordAppLayers(render::CommandBuffer& outBuffer,
                         int vpW, int vpH,
                         int insetTop, int insetRight, int insetBottom,
                         float scrollY);

    /// Replay the previously-recorded app command buffer against `renderer`,
    /// producing UILayers as a side-effect of layer-break commands. Run on
    /// the raster thread (windowed) or main thread (headless). surfW/surfH
    /// are the app *content* dimensions (viewport minus engine insets) —
    /// app layer surfaces are content-sized; the compositor places them.
    void replayAppLayers(render::SkiaRenderer* renderer,
                         const render::CommandBuffer& buffer,
                         std::vector<render::SkiaRenderer::GPUSurface>& pool,
                         int& poolW, int& poolH,
                         int surfW, int surfH,
                         std::vector<UILayer>& outLayers);

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

    // HTML document.readyState. Starts "loading" while user scripts execute,
    // advances to "interactive"/"complete" as DOMContentLoaded/load dispatch.
    std::string documentReadyState_ = "loading";

    // Per-document mouse dispatch state for the app doc (mousedown target +
    // rolling click/dblclick tracking). System docs carry their own instance.
    MouseDispatchState appMouseState_;

    // Mouse button state for buttons bitmask
    int pressedButtons_ = 0;

    // Modifier keys currently held via simulated handleKeyDown()/handleKeyUp()
    // calls (SDL_KMOD_* bits). safeGetModState() only sees the OS's real
    // physical keyboard state, which headless input simulation never touches —
    // this mask lets simulated keyDown(shift)+click() combinations (e.g.
    // shift-click) produce a MouseEvent with the correct shiftKey/ctrlKey/etc.
    // See safeGetModState() in input_handling.cpp.
    int heldModifierMask_ = 0;

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
