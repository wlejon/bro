#pragma once

#include "engine/app_loader.h"
#include "engine/css_transitions.h"
#include "engine/gizmo.h"
#include "engine/menu_bar.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "engine/scrollbar.h"
#include "engine/settings.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "layout/font_manager.h"
#include "render/skia_backend.h"
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <glad/gl.h>
#include <include/core/SkSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <quickjs.h>

typedef struct SDL_GLContextState* SDL_GLContext;

namespace bro::layout { struct KeyHandleResult; }
namespace bro::render { class GLContext; class RasterRenderer; }
namespace bro::webgl { class WebGL2RenderingContext; }
namespace broaudio { class Engine; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::net { class NetService; }
namespace bro::scene { class SceneGraph; }
namespace bro::canvas { class CanvasScene; }

namespace bro::platform {
    class Window;
    class EventLoop;
}
namespace bro::render { class Renderer; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; class TextNode; }
namespace bro::layout { class DrawTraversal; class SkiaTextMetrics; }

namespace bro::engine {

enum class DisplayMode { Windowed, Headless, Server };

/// Raster thread state machine (atomics only, no mutexes).
enum RasterState : uint32_t {
    kRasterIdle          = 0,  // Raster thread waiting for work
    kRasterDomStable     = 1,  // Main thread: DOM is stable, go rasterize
    kRasterBusy          = 2,  // Raster thread is drawing
    kRasterTexturesReady = 3,  // Raster thread: new textures + GL fence ready
    kRasterShutdown      = 4,  // Main thread: terminate raster thread
};

/// Layout thread state machine (atomics only, no mutexes).
enum LayoutState : uint32_t {
    kLayoutIdle      = 0,  // Layout thread waiting for work
    kLayoutDomStable = 1,  // Main thread: DOM is stable, go layout
    kLayoutBusy      = 2,  // Layout thread computing styles + layout
    kLayoutDone      = 3,  // Layout thread: results ready
    kLayoutShutdown  = 4,  // Main thread: terminate layout thread
};

/// Graphics/display settings configurable per app.
struct GraphicsConfig {
    int width = 1920;
    int height = 1080;
    bool useGPU = true;       // headless uses GPU by default; --no-gpu disables
    bool resizable = true;    // whether the window can be resized
    bool vsync = true;        // true = adaptive or standard vsync; false = uncapped
    double maxFrameIntervalMs = 8.0;  // layout/raster throttle (0 = uncapped)
};

/// Input behavior settings configurable per app.
struct InputConfig {
    float scrollSpeed = 48.0f;             // pixels per mouse wheel tick
    double doubleClickThresholdMs = 500.0; // max time between clicks for dblclick
    float doubleClickDistancePx = 5.0f;    // max movement between clicks for dblclick
    uint32_t overlayToggleKey = 0x40000041u; // SDLK_F8; 0 = disabled
};

/// Crosshair configuration — rendered directly in the GL/Skia pipeline.
struct CrosshairConfig {
    // --- Visual ---
    bool visible = false;
    enum Style : uint8_t { Cross, Dot, Circle, CrossDot } style = Cross;
    float size = 20.0f;           // arm length from center
    float thickness = 2.0f;       // line width in pixels
    float dotSize = 2.0f;         // center dot radius
    float outlineThickness = 1.0f;
    bool outline = true;          // dark outline for visibility on any background
    uint8_t r = 0, g = 255, b = 0, a = 204;   // color
    uint8_t outR = 0, outG = 0, outB = 0, outA = 180; // outline color

    // --- Spread system ---
    float spread = 4.0f;         // base spread (idle gap in pixels)
    float moveSpread = 0.0f;     // added to spread when moving
    float fireBloom = 0.0f;      // spread kick per shot
    float adsSpread = -1.0f;     // spread when ADS (-1 = no ADS override)
    float bloomDecay = 40.0f;    // bloom recovery (pixels/sec)
    float lerpSpeed = 10.0f;     // spread interpolation speed (higher = faster)

    // --- State (engine-managed, readable by app) ---
    bool moving = false;
    bool aiming = false;
    float currentBloom = 0.0f;   // current fire bloom (decays over time)
    float currentSpread = 4.0f;  // interpolated spread (used as gap when drawing)
    float manualSpread = -1.0f;  // if >= 0, overrides spread system

    /// Advance spread interpolation and bloom decay.
    void tick(float dtSec);
};

struct EngineConfig {
    std::string appDir;
    std::string title;   // window title override (empty = use <title> from HTML)
    std::string settingsPath; // path to .bro_settings.json (empty = auto-detect)
    DisplayMode displayMode = DisplayMode::Windowed;
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
    // Compositing layer entry — built by the raster thread, consumed by the main thread.
    struct UILayer {
        enum Type { HTML, Canvas };
        Type type;
        GLuint texture = 0;
        canvas::CanvasScene* canvasScene = nullptr;
        float cx = 0, cy = 0, cw = 0, ch = 0;
    };

    explicit Engine(const EngineConfig& config);
    ~Engine();

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
    dom::Element* pointerLockElement() const { return lockedElement_; }

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

    /// Find an element by selector (#id shorthand or CSS selector).
    dom::Element* querySelector(const std::string& selector) const;

    /// Find an element in an overlay panel's DOM.
    dom::Element* overlayQuerySelector(const std::string& panelName,
                                       const std::string& selector) const;

    /// Get overlay panel names.
    std::vector<std::string> overlayPanelNames() const;

    /// Simulate a click on the given element.
    void dispatchClickOn(dom::Element* target);

    /// Crosshair configuration (read/write from JS bindings).
    CrosshairConfig& crosshair() { return crosshair_; }
    const CrosshairConfig& crosshair() const { return crosshair_; }

    /// Engine-level 3D gizmo (translate arrows in phase 1; rotate + scale
    /// handles arrive in later phases). Driven from JS via bro.gizmo.*.
    GizmoManager& gizmo() { return *gizmo_; }
    const GizmoManager& gizmo() const { return *gizmo_; }

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

    /// Top inset reserved for the standard menu bar (0 when hidden).
    /// The app document lays out into (viewportWidth, viewportHeight - contentTop())
    /// and is drawn translated down by this amount; system panels outside the
    /// menu (e.g. nav/settings) read it to position themselves below the bar.
    int contentTop() const { return menuBar_.visible ? menuBar_.height : 0; }
    int contentHeight() const { return viewportHeight_ - contentTop(); }

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
    void applyKeyResult(dom::Element* el, const layout::KeyHandleResult& r);
    void dispatchInputEvent(dom::Element* el, const std::string& data = "",
                            const std::string& inputType = "");
    void dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget);
    void dispatchScrollEvent(dom::Element* el);
    void advanceFocus(bool reverse);
    void addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene);
    void compositeCanvasScenes(int w, int h);
    void compositeCanvasScenes(render::GLContext* gl, int w, int h, GLuint targetFBO);
    void drawTexturedQuad(GLuint tex, float x, float y, float w, float h);
    void compositeLayers(const std::vector<UILayer>& layers);
    void drawCrosshairGL();                  // windowed/headless GPU path
    void drawCrosshairSkia(SkCanvas* canvas); // headless CPU path
    void ensureReplacedElements(dom::Element* elem);

    // --- System panel management (implementation in system_panels.cpp) ---
    // System panels are ordinary HTML documents rendered through the same
    // layout/raster pipeline as the app document. They share the engine's
    // fontManager_ + textMetrics_ for layout, and in windowed mode the raster
    // thread draws each visible panel into its own GPU surface.
    struct SystemDocument {
        std::string name;
        std::string tabLabel;
        std::string group;
        bool active = true;
        JSContext* jsCtx = nullptr;
        std::unique_ptr<js::Timers> timers;
        std::unique_ptr<dom::Document> document;
        JSValue broPerfObj = JS_UNDEFINED;
        MouseDispatchState mouseState;  // per-doc click/dblclick tracking
        // 2D canvas contexts owned by this panel (inline-blit at layer break).
        std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
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
    /// Draw the document's Selection highlight (semi-transparent rectangles
    /// behind the selected text runs). `docOffsetY` is the vertical offset
    /// applied to the app content by the main draw pass — typically
    /// (insetTop - scrollY). No-op when the selection is empty.
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
    std::unique_ptr<layout::DrawTraversal> drawTraversal_;
    std::unique_ptr<layout::SkiaTextMetrics> textMetrics_;
    layout::FontManager fontManager_;
    std::unique_ptr<platform::EventLoop> eventLoop_;

    bool running_ = false;
    int viewportWidth_;
    int viewportHeight_;

    // Pre-compiled observer check function (avoids JS_Eval parse per frame)
    JSValue observerCheckFn_ = JS_UNDEFINED;
    AppManifest manifest_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes_;

    // WebGL contexts (owned by engine, associated with canvas elements)
    struct WebGLEntry {
        std::unique_ptr<webgl::WebGL2RenderingContext> context;
        dom::Element* element = nullptr;  // non-owning
    };
    std::vector<WebGLEntry> webglEntries_;

    // --- Raster thread state ---
    // Shared atomic communication between main and raster threads.
    struct RasterShared {
        std::atomic<uint32_t> state{kRasterIdle};
        std::atomic<uintptr_t> fenceSync{0};      // GLsync handle
        std::atomic<int> vpWidth{0};
        std::atomic<int> vpHeight{0};
        std::atomic<int> insetTop{0};               // top inset reserved for menu bar
        std::atomic<uint32_t> scrollYBits{0};      // float via bit_cast
        std::atomic<int> frontBuffer{0};            // 0 or 1
    };
    RasterShared rasterShared_;
    std::atomic<bool> rasterReady_{false};
    std::thread rasterThread_;
    SDL_GLContext rasterGLContext_ = nullptr;

    // --- Layout thread state ---
    struct LayoutShared {
        std::atomic<uint32_t> state{kLayoutIdle};
        std::atomic<int> vpWidth{0};
        std::atomic<int> vpHeight{0};
        std::atomic<int> insetTop{0};
        std::atomic<bool> animationsActive{false};
        std::atomic<dom::Element*> hoveredElement{nullptr};
    };
    LayoutShared layoutShared_;
    std::thread layoutThread_;

    // Double-buffered layer lists for lock-free handoff.
    // Raster thread writes to back buffer, main reads front buffer.
    // `appLayers` are composited first (with crosshair drawn between app and
    // system), then `systemLayers` on top so menu bar / preferences / splash
    // sit above app content + crosshair.
    struct LayerBuffer {
        std::vector<UILayer> appLayers;
        std::vector<UILayer> systemLayers;
    };
    LayerBuffer layerBuffers_[2];

    // Pool of reusable GPU-backed Skia surfaces for HTML layers.
    // Owned by the raster thread — FBOs are per-context but textures
    // are shared across GL contexts for compositing on the main thread.
    std::vector<render::SkiaRenderer::GPUSurface> htmlSurfacePool_;
    int htmlSurfacePoolW_ = 0, htmlSurfacePoolH_ = 0;
    // Parallel pool, one entry per visible system panel per frame. Separate
    // from htmlSurfacePool_ so app layer-break sizing can't invalidate panel
    // surfaces mid-frame.
    std::vector<render::SkiaRenderer::GPUSurface> systemSurfacePool_;
    int systemSurfacePoolW_ = 0, systemSurfacePoolH_ = 0;

    CrosshairConfig crosshair_;
    MenuBar menuBar_;
    std::unique_ptr<GizmoManager> gizmo_;
    OverlayManager overlayMgr_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<broaudio::Engine> audioEngine_;
    std::unique_ptr<physics::PhysicsWorld> physicsWorld_;
    std::unique_ptr<net::NetService> netService_;
    struct SceneGraphEntry {
        std::unique_ptr<scene::SceneGraph> graph;
        dom::Element* element = nullptr;  // non-owning
    };
    std::vector<SceneGraphEntry> sceneGraphs_;
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
    dom::Element* systemHoverTarget_ = nullptr;
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
    dom::Element* hoveredElement_ = nullptr;

    // Mouse tracking for mousemove movement deltas
    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;

    // Pointer lock: while set, hit-testing/hover is frozen and mousemove events
    // are routed to lockedElement_ with clientX/Y pinned to lockedMouse{X,Y}_.
    // Only movementX/Y (SDL xrel/yrel) reflect the actual motion.
    dom::Element* lockedElement_ = nullptr;
    float lockedMouseX_ = 0.0f;
    float lockedMouseY_ = 0.0f;

    // Per-document mouse dispatch state for the app doc (mousedown target +
    // rolling click/dblclick tracking). System docs carry their own instance.
    MouseDispatchState appMouseState_;

    // Mouse button state for buttons bitmask
    int pressedButtons_ = 0;

    // Mouse-driven text selection. `selectionAnchor*` is pinned on mousedown
    // (the static endpoint of a drag); selectionDragging_ means subsequent
    // mousemove events should extend the focus to follow the cursor.
    bool selectionDragging_ = false;
    dom::TextNode* selectionAnchorNode_ = nullptr;
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
    dom::Element* scrollbarDragTarget_ = nullptr;
    dom::Element* scrollbarHoveredElement_ = nullptr;
    /// When non-null, scrollbarDragTarget_ belongs to this system panel
    /// document rather than the app document, and the drag-update code
    /// dispatches scroll events through the panel's JS context. Cleared
    /// when the drag ends.
    SystemDocument* scrollbarDragSystemDoc_ = nullptr;

    // UI render throttle — layout+rasterize at most every N ms (from config)
    double uiFrameIntervalMs_ = 8.0;
    double lastUIRenderMs_ = 0.0;

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
