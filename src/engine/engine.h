#pragma once

#include "engine/app_loader.h"
#include "engine/css_transitions.h"
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
namespace bro::render { class GLContext; class RasterRenderer; class CPURasterRenderer; }
namespace bro::webgl { class WebGL2RenderingContext; }
namespace broaudio { class Engine; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::net { class NetworkManager; }
namespace bro::scene { class SceneGraph; }
namespace bro::canvas { class CanvasScene; }

namespace bro::platform {
    class Window;
    class EventLoop;
}
namespace bro::render { class Renderer; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; }
namespace bro::layout { class DrawTraversal; }

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
    void handleDropFile(const std::string& path, float x = -1, float y = -1);
    void handleDropText(const std::string& text, float x = -1, float y = -1);

    // Clipboard simulation (for headless testing — bypasses system clipboard)
    void simulatePaste(const std::string& text);
    std::string simulateCopy();
    std::string simulateCut();

    float getLastMouseX() const { return lastMouseX_; }
    float getLastMouseY() const { return lastMouseY_; }

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

    /// Get display mode.
    DisplayMode displayMode() const { return displayMode_; }

    /// Get viewport dimensions.
    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

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

private:
    // Per-app configuration (stored for use throughout engine lifetime)
    GraphicsConfig graphicsConfig_;
    InputConfig inputConfig_;

    dom::Element* hitTest(float x, float y);
    void dispatchEvent(dom::Element* target, dom::Event& event);
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
    struct SystemDocument {
        std::string name;
        std::string tabLabel;
        std::string group;
        bool active = true;
        JSContext* jsCtx = nullptr;
        std::unique_ptr<js::Timers> timers;
        std::unique_ptr<layout::DrawTraversal> drawTraversal;
        std::unique_ptr<layout::FontManager> fontManager;
        std::unique_ptr<dom::Document> document;
        JSValue broPerfObj = JS_UNDEFINED;
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
    void renderSystemPanels();
    void resizeSystemPanels(int w, int h);
    dom::Element* systemHitTest(SystemDocument& doc, float x, float y);
    bool systemHandleMouseDown(float x, float y, int button);
    bool systemHandleMouseUp(float x, float y, int button);
    bool systemHandleMouseMove(float x, float y);

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
        std::atomic<uint32_t> scrollYBits{0};      // float via bit_cast
        std::atomic<int> frontBuffer{0};            // 0 or 1
    };
    RasterShared rasterShared_;
    std::thread rasterThread_;
    SDL_GLContext rasterGLContext_ = nullptr;

    // --- Layout thread state ---
    struct LayoutShared {
        std::atomic<uint32_t> state{kLayoutIdle};
        std::atomic<int> vpWidth{0};
        std::atomic<int> vpHeight{0};
        std::atomic<bool> animationsActive{false};
        std::atomic<dom::Element*> hoveredElement{nullptr};
    };
    LayoutShared layoutShared_;
    std::thread layoutThread_;

    // Double-buffered layer lists for lock-free handoff.
    // Raster thread writes to back buffer, main reads front buffer.
    struct LayerBuffer {
        std::vector<UILayer> layers;
    };
    LayerBuffer layerBuffers_[2];

    // Pool of reusable GPU-backed Skia surfaces for HTML layers.
    // Owned by the raster thread — FBOs are per-context but textures
    // are shared across GL contexts for compositing on the main thread.
    std::vector<render::SkiaRenderer::GPUSurface> htmlSurfacePool_;
    int htmlSurfacePoolW_ = 0, htmlSurfacePoolH_ = 0;

    CrosshairConfig crosshair_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<broaudio::Engine> audioEngine_;
    std::unique_ptr<physics::PhysicsWorld> physicsWorld_;
    std::unique_ptr<net::NetworkManager> networkManager_;
    struct SceneGraphEntry {
        std::unique_ptr<scene::SceneGraph> graph;
        dom::Element* element = nullptr;  // non-owning
    };
    std::vector<SceneGraphEntry> sceneGraphs_;
    double physicsAccumMs_ = 0.0;
    double lastPhysicsTimeMs_ = 0.0;
    // System panels (settings, perf, nav)
    std::vector<SystemDocument> systemDocs_;
    std::unique_ptr<render::CPURasterRenderer> systemRenderer_;
    bool systemPerfVisible_ = false;
    bool systemSettingsVisible_ = false;
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

    // Hover tracking for mouseenter/mouseleave/mouseover/mouseout
    dom::Element* hoveredElement_ = nullptr;

    // Mouse tracking for mousemove movement deltas
    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;

    // Double-click detection
    double lastClickTimeMs_ = 0.0;
    float lastClickX_ = 0.0f;
    float lastClickY_ = 0.0f;
    int clickCount_ = 0;
    dom::Element* lastClickTarget_ = nullptr;

    // Mouse button state for buttons bitmask
    int pressedButtons_ = 0;
    dom::Element* mouseDownTarget_ = nullptr;

    // Viewport scrolling
    float scrollY_ = 0.0f;
    float documentHeight_ = 0.0f;

    // Scrollbar components (styles from config)
    Scrollbar viewportScrollbar_;
    Scrollbar elementScrollbar_;
    bool draggingViewportScrollbar_ = false;
    dom::Element* scrollbarDragTarget_ = nullptr;
    dom::Element* scrollbarHoveredElement_ = nullptr;

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
