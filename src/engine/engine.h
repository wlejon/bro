#pragma once

#include "engine/app_loader.h"
#include "util/asset_mounts.h"
#include "engine/css_transitions.h"
#include "engine/dom_undo.h"
#include "engine/web_animations.h"
#if BRO_WITH_3D
#include "engine/gizmo.h"  // GizmoManager (3D-only; pulls scene::MeshNode)
#endif
#include "engine/gamepad.h"
#include "engine/inspector_state.h"
#include "engine/menu_bar.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "dom/event_target.h"   // dom::EventCallback / ListenerHandle — window listeners
#include "dom/node_handle.h"
#include "engine/scrollbar.h"
#include "engine/settings.h"
#include "engine/ui_layer.h"
#include "js/message_queue.h"  // js::Message — secondary-window postMessage queues
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "render/skia_backend.h"
#include <atomic>
#include <bit>
#include <climits>
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
/// By-reference view of the members shared by every hosted sub-document
/// (engine/sub_document.h). Incomplete here on purpose — engine.h is what
/// sub_document.h includes, not the other way round.
struct SubDocRef;

enum class DisplayMode { Windowed, Headless, Server };

/// Sentinel for "no explicit startup window position requested" (the bro.json
/// windowX/windowY keys). Any real coordinate — including negative ones on a
/// multi-monitor desktop — is representable, so use INT_MIN, not 0/-1.
inline constexpr int kWindowPosUnset = INT_MIN;

/// Graphics/display settings configurable per app.
struct GraphicsConfig {
    int width = 1920;
    int height = 1080;
    bool useGPU = true;       // headless uses GPU by default; --no-gpu disables
    bool resizable = true;    // whether the window can be resized
    bool vsync = true;        // true = adaptive or standard vsync; false = uncapped
    double maxFrameIntervalMs = 8.0;  // layout/raster throttle (0 = uncapped)
    double maxFps = 0.0;      // present-rate cap independent of vsync (0 = uncapped)

    // Startup window management (bro.json; runtime control via bro.window.*).
    // Transparent windows are deliberately NOT offered: the compositor owns
    // the GL swap chain and a per-pixel-alpha window would need a different
    // swap-chain setup on every platform — deferred until something needs it.
    bool borderless = false;   // no title bar / border (SDL_WINDOW_BORDERLESS)
    bool alwaysOnTop = false;  // keep above all normal windows
    int minWidth = 0;          // min/max resize limits; 0 = unconstrained
    int minHeight = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    int windowX = kWindowPosUnset;  // explicit startup position (both must be set)
    int windowY = kWindowPosUnset;
    int display = -1;          // display INDEX to center on at startup; -1 = OS default
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
    /// Hook for a host application that embeds the engine: called during JS
    /// init, after every built-in binding is installed and before any app
    /// script runs. This is how an executable that links bro_engine adds its
    /// own `bro.*` namespace without editing engine_init.cpp — ffmpeg-bro
    /// installs `bro.ffmpeg` through it.
    ///
    /// Runs on the main thread, once per realm the engine creates bindings
    /// for. Keep it to installing bindings; the Engine is still mid-init.
    std::function<void(JSContext*)> installHostBindings;

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

    /// Handle SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: re-read the window's
    /// display scale and, when it actually moved, refresh devicePixelRatio in
    /// every live realm and dispatch a window resize event so apps re-read it.
    /// No-op outside windowed mode — headless keeps a deterministic 1.0.
    void handleDisplayScaleChanged();

    /// OS display scale exposed to JS as window.devicePixelRatio. 1.0 in
    /// headless mode (deterministic tests) and before the window exists.
    float displayScale() const { return displayScale_; }

    /// Effective CSS color scheme ("light" or "dark") for
    /// `@media (prefers-color-scheme)`. Resolves the appearance.colorScheme
    /// setting: "light"/"dark" force a scheme, "system" follows the OS theme
    /// (SDL_GetSystemTheme; unknown → light).
    std::string effectiveColorScheme() const;

    /// Push the effective color scheme into every live document (app,
    /// iframes, system panels). Documents whose scheme actually changed
    /// re-evaluate their @media blocks and mark themselves dirty for restyle.
    /// Called at init, on OS theme change, and on appearance settings change.
    void applyColorScheme();

    /// Re-evaluate window.matchMedia MediaQueryLists in every live realm
    /// (app, iframes, system panels) and fire 'change' where matches flipped.
    /// Cheap no-op per realm unless its document's media context moved; each
    /// realm additionally defers until its media-triggered restyle has landed,
    /// so listeners observe styles consistent with the new context. Called at
    /// the post-restyle drain points (windowed frame, headless flush) and at
    /// the end of handleResize (which restyles synchronously).
    void deliverMediaQueryChangesAllRealms();

    /// Input events forwarded from the event loop.
    void handleMouseDown(float x, float y, int button);
    void handleMouseUp(float x, float y, int button);
    void handleMouseMove(float x, float y, float xrel, float yrel);
    void handleKeyDown(int keycode, int scancode, int mod, bool repeat);
    void handleKeyUp(int keycode, int scancode, int mod, bool repeat);
    void handleTextInput(const std::string& text);
    /// IME composition update (SDL_EVENT_TEXT_EDITING, and the headless
    /// imeCompose/imeCancel seam). `text` is the current preedit ("" cancels
    /// the composition), `start` the composition cursor in UTF-8 characters
    /// within it. The focused input/textarea shows the preedit inline in its
    /// value as provisional text (browser behavior); with the DOM Selection
    /// caret inside a contenteditable host instead, the preedit is spliced
    /// provisionally into the text node at the caret (visible to textContent
    /// reads). A TEXT_INPUT while composing commits it. `length` (SDL's
    /// selected span) is accepted for signature parity but the preedit
    /// renders as one underlined run.
    void handleTextEditing(const std::string& text, int start, int length);
    void handleWheel(float x, float y, float dx, float dy);

    /// Eases accumulated wheel deltas (see wheelResidualY_) into scrollY_
    /// over time. Called once per frame before layout/render.
    void drainWheelSmoothing(float frameDtSec);
    /// One drop gesture. `paths` carries every file the user dropped together;
    /// they land in a single DragEvent as dataTransfer.files.
    void handleDropFile(const std::vector<std::string>& paths, float x = -1, float y = -1);
    /// Single-file convenience for callers that genuinely have one path.
    void handleDropFile(const std::string& path, float x = -1, float y = -1) {
        handleDropFile(std::vector<std::string>{ path }, x, y);
    }
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

    /// Trigger-rumble request (vibrationActuator "trigger-rumble" effect).
    /// Magnitudes 0..1 per trigger. Real pads forward to
    /// SDL_RumbleGamepadTriggers; virtual pads just record.
    bool gamepadRumbleTriggers(int index, float leftMagnitude,
                               float rightMagnitude, int durationMs);

    // --- Touch input (SDL finger events; also the headless touch* seam) ---
    // One call per contact transition/movement. `fingerId` is any stable
    // per-contact id (the SDL finger id on the real path, a caller-chosen id
    // from the headless injector); x/y are window coordinates, pressure 0..1.
    // Each contact drives W3C Pointer Events (pointerdown/move/up/cancel,
    // pointerType "touch", unique pointerId ≥ 2) followed by Touch Events
    // (touchstart/move/end/cancel with touches/targetTouches/changedTouches),
    // and a primary-contact tap synthesizes the compat mouse sequence
    // (mousedown → mouseup → click). Implementations in touch_input.cpp.
    void handleTouchDown(uint64_t fingerId, float x, float y, float pressure = 1.0f);
    void handleTouchMove(uint64_t fingerId, float x, float y, float pressure = 1.0f);
    void handleTouchUp(uint64_t fingerId, float x, float y);
    void handleTouchCancel(uint64_t fingerId, float x, float y);

    // ── Secondary window hosts (src/engine/window_host.cpp) ─────────────────
    // One extra OS window opened via bro.window.open(src, opts). Each host is
    // a real OS window hosting a full, isolated document realm built from
    // opts.src — open/close/'close'/'load' events, per-host
    // focus/minimized/occluded state, per-host record/replay of that document,
    // a per-window composite+swap pass, and its own input routing (mouse,
    // cursor, keyboard, text input, IME, wheel, drop — see the routing entry
    // points below and src/engine/window_host_input.cpp). v1 IN PROGRESS:
    // postMessage between windows lands with the next chunk of the plan.
    //
    // Threading/lifecycle: creation and destruction are QUEUED
    // (openWindowHost/closeWindowHost) and drained at the raster-idle point
    // in the frame loop — processPendingWindowHosts(), beside
    // processPendingIframeReloads() — or in headless flush(). Nothing else
    // may create or destroy a host's SDL window.

    /// Options for bro.window.open (geometry/flags feed the secondary
    /// window; `display` is a display INDEX like bro.json's, -1 = OS default).
    struct WindowHostOptions {
        std::string src;
        std::string title = "bro";
        int width = 800;
        int height = 600;
        int x = kWindowPosUnset;
        int y = kWindowPosUnset;
        int display = -1;
        bool resizable = true;
        bool borderless = false;
        bool alwaysOnTop = false;
        bool hidden = false;   // forced true in headless (deterministic tests)
        int minWidth = 0, minHeight = 0;   // 0 = unconstrained
        int maxWidth = 0, maxHeight = 0;

        /// Which of the above the CALLER passed explicitly. The child app's own
        /// bro.json supplies the rest (see applyChildManifestDefaults): explicit
        /// open() options win over the child manifest, which wins over the
        /// built-in defaults. Only the keys a bro.json can carry need a flag.
        struct Provided {
            bool width = false, height = false, title = false;
            bool resizable = false, borderless = false, alwaysOnTop = false;
            bool minWidth = false, minHeight = false;
            bool maxWidth = false, maxHeight = false;
        } provided;
    };

    /// One secondary window host: a real OS window plus the isolated document
    /// realm rendered into it. `window` is a context-less platform window
    /// (Window::createSecondary); null while pendingCreate or after close.
    ///
    /// The document half is the same shape as IframeDoc and is driven by the
    /// same shared helpers (engine/sub_document.h) through windowHostSubDoc().
    /// It is NOT a common base class — see that header for why.
    struct WindowHost {
        uint64_t id = 0;        // bro-side handle id (stable across lifetime)
        uint32_t sdlId = 0;     // SDL windowID for event routing (0 until created)
        std::unique_ptr<platform::Window> window;
        WindowHostOptions opts; // requested state; geometry queried live once created
        bool pendingCreate = true;
        bool pendingClose = false;
        bool focused = false;
        bool minimized = false;
        bool occluded = false;
        int width = 0, height = 0;  // client size in window coords
        // Composite clear color behind the document (visible only where the
        // document surface is transparent, and before the first frame lands).
        float clearColor[4] = {0.07f, 0.07f, 0.09f, 1.0f};

        // ── The host's document realm ────────────────────────────────────────
        double displayScale = 1.0;   // this window's DPR (headless pins 1.0)
        bool loadFired = false;      // handle 'load' dispatched after first record
        JSContext* jsCtx = nullptr;
        std::unique_ptr<js::Timers> timers;
        // canvasScenes MUST precede `document`: Elements (owned by document)
        // fire CanvasScene::onElementFinalized on destroy, which needs the
        // scenes alive. Member destruction is reverse-declaration order, so
        // this destructs last.
        std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
        std::unique_ptr<dom::Document> document;
        // ── Per-window input state (src/engine/window_host_input.cpp) ───────
        // A host window carries no engine chrome, so window space == content
        // space == document space: no menu-bar inset to fold out and no
        // viewport scroll to add. Every one of these mirrors an app-document
        // member (appMouseState_, hoveredElement_, lastMouseX_, ...) that the
        // main window keeps for itself.
        MouseDispatchState mouseState;          // per-doc click/dblclick tracking
        dom::Element* hoveredElement = nullptr; // per-window :hover target
        dom::Element* activeElement = nullptr;  // per-window keyboard focus
        float lastMouseX = 0.0f, lastMouseY = 0.0f;
        int pressedButtons = 0;                 // DOM MouseEvent.buttons mask
        dom::ElementHandle controlDragElement;  // text drag-select in this window
        std::string resolvedCursor = "default"; // this window's CSS cursor
        int boxW = 0, boxH = 0;                 // last client size laid out
        // Render target: the host document paints into cmdBuffer (main thread),
        // replayed into `surface` (raster thread) → fboTexture, which
        // compositeWindowHosts() draws fullscreen on this window's drawable.
        render::CommandBuffer cmdBuffer;
        render::SkiaRenderer::GPUSurface surface;
        int surfW = 0, surfH = 0;
        unsigned int fboTexture = 0;

        /// Parent → this window: structured-clone payloads waiting to be
        /// deserialized into `jsCtx` and dispatched as 'message' events, in
        /// post order. Drained at the raster-idle point
        /// (drainWindowHostMessages) so the app JS a message triggers never
        /// lands mid-frame. Living IN the host means a window that closes
        /// before delivery simply drops its undelivered mail.
        std::vector<std::unique_ptr<js::Message>> inbox;
    };

    /// Queue a new host. Returns its bro id immediately; the OS window
    /// materializes at the next drain. Caller (the JS binding) has already
    /// validated mode + realm. Headless forces hidden.
    uint64_t openWindowHost(const WindowHostOptions& opts);
    /// Queue destruction of a host (idempotent — double-close is a no-op).
    /// The window is destroyed and the handle's 'close' event fires at the
    /// next drain. Also the OS-close-button path via handleWindowCloseRequested.
    void closeWindowHost(uint64_t id);
    WindowHost* windowHostById(uint64_t id);
    WindowHost* windowHostBySdlId(uint32_t sdlId);
    /// Any host whose SDL window currently exists (pendingClose included —
    /// SDL still counts it). Drives close-requested routing and the
    /// unfocused-present-clamp condition.
    bool anyLiveWindowHosts() const;
    /// Any created, non-minimized host — i.e. the per-window composite pass
    /// has something to present.
    bool anyPresentableWindowHosts() const;
    /// Any created host with input focus. The unfocused present-rate clamp
    /// fires only when NO bro window (main or host) is focused.
    bool anyWindowHostFocused() const;

    /// SDL_EVENT_WINDOW_CLOSE_REQUESTED for any window. Main window: quits
    /// the app — but only when secondary windows exist; with a lone main
    /// window SDL follows the request with SDL_EVENT_QUIT and the event
    /// loop's quit path handles it (acting here too would double
    /// requestInterrupt, whose second call hard-exits). Secondary window:
    /// queues the host close (window destroys + 'close' event at the drain).
    void handleWindowCloseRequested(uint32_t sdlWindowId);
    /// Window-state bookkeeping for secondary hosts (per-host flags; the
    /// main window keeps its existing dedicated paths).
    void handleHostResized(uint32_t sdlWindowId, int w, int h);
    void handleHostFocusChanged(uint32_t sdlWindowId, bool focused);
    void handleHostMinimized(uint32_t sdlWindowId, bool minimized);
    void handleHostOccluded(uint32_t sdlWindowId, bool occluded);

    /// Drain queued host creates/destroys. MUST run only at the raster-idle
    /// point (windowed frame loop) or from headless flush() — destroying a
    /// host mid-frame would later race the raster thread once host documents
    /// render (chunk 2), and firing 'close' runs app JS. Fires handle 'close'
    /// events through the window-host bindings.
    void processPendingWindowHosts();

    // ── Messaging between a window host's realm and the app realm ───────────
    // Both directions are structured clones (js/message_serializer.cpp — the
    // same encoder Worker.postMessage uses, transfers included) handed over as
    // a serialized Message and deserialized into the DESTINATION context.
    // Delivery is asynchronous: queued here, drained at the raster-idle point
    // by drainWindowHostMessages(), never on the caller's stack.

    /// App realm → host `id`. Takes ownership. False when `id` is unknown or
    /// already closing — a message to a dead window is a silent no-op, exactly
    /// like the web's postMessage to a closed window.
    bool postMessageToWindowHost(uint64_t id, std::unique_ptr<js::Message> msg);
    /// Host `hostId` → the app realm's handle for it. Takes ownership. A
    /// message whose window (or handle) is gone by delivery time is dropped.
    void postMessageToParent(uint64_t hostId, std::unique_ptr<js::Message> msg);
    /// Deliver every queued message: children first (so a reply posted from a
    /// child's 'message' handler reaches the parent in this same drain), then
    /// the parent's handles. Runs app JS — raster-idle points only, beside
    /// processPendingWindowHosts().
    void drainWindowHostMessages();
    /// The host whose realm is `ctx`, or 0 when `ctx` is not a host realm
    /// (the app realm, an iframe, a system panel). Lets the child-side
    /// bindings resolve "my window" from the context they run on.
    uint64_t windowHostIdForContext(JSContext* ctx) const;

    // ── Per-window input routing (src/engine/window_host_input.cpp) ─────────
    // Every input event that lands on a secondary window comes through here,
    // keyed by bro host id. Two callers: the event loop (which resolves the
    // SDL windowID through windowHostBySdlId first) and the headless
    // injection seams (whose optional windowId argument names the host
    // directly). Unknown or document-less ids are silently ignored — a window
    // can close between an event being queued and delivered.
    //
    // Coordinates are in the host window's own space, which is also its
    // document space: a secondary window has no menu-bar inset and no engine
    // viewport scroll. Button ids are SDL convention, like the main-window
    // handlers.
    //
    // These NEVER consult main-window chrome: overlays, system panels, the
    // inspector, the gizmo and the viewport scrollbar are all primary-window
    // furniture. v1 also keeps pointer lock, touch, and the gamepad/"action"
    // stream bound to the main window (see docs/window-api.js).
    void hostMouseDown(uint64_t hostId, float x, float y, int sdlButton);
    void hostMouseUp(uint64_t hostId, float x, float y, int sdlButton);
    void hostMouseMove(uint64_t hostId, float x, float y, float xrel, float yrel);
    void hostKeyDown(uint64_t hostId, int keycode, int scancode, int mod, bool repeat);
    void hostKeyUp(uint64_t hostId, int keycode, int scancode, int mod, bool repeat);
    void hostTextInput(uint64_t hostId, const std::string& text);
    void hostTextEditing(uint64_t hostId, const std::string& text, int start, int length);
    void hostWheel(uint64_t hostId, float x, float y, float dx, float dy);
    void hostDropFile(uint64_t hostId, const std::vector<std::string>& paths, float x, float y);
    void hostDropFile(uint64_t hostId, const std::string& path, float x, float y) {
        hostDropFile(hostId, std::vector<std::string>{ path }, x, y);
    }
    void hostDropText(uint64_t hostId, const std::string& text, float x, float y);

    /// bro host id of the secondary window that currently has input focus,
    /// or 0 when focus is on the main window (or nowhere). Keyboard, text
    /// input and IME follow this.
    uint64_t focusedWindowHostId() const { return focusedHostId_; }

    /// Per-window resolved cursor shape — `hostId` 0 means the main window,
    /// so the existing no-arg resolvedCursor() is exactly resolvedCursor(0).
    /// Unknown ids report "default".
    const std::string& resolvedCursor(uint64_t hostId) const;

    /// Synchronous, main-thread capture of a host window's document pixels
    /// (top-down RGBA), for the parent-side handle's capture(). Brings the
    /// host current first — quiesce the raster worker, re-record at the
    /// window's CURRENT size — so the result never lags by a frame. Empty when
    /// the host is unknown or has no document.
    std::vector<uint8_t> captureWindowHost(uint64_t id, int& outW, int& outH);

    /// All gamepad slots (connected and not) — read by the JS bindings to
    /// build navigator.getGamepads() snapshots.
    const std::vector<GamepadState>& gamepads() const { return gamepads_; }

    // --- Polled action state (bro.settings.getActionStrength /
    // isActionPressed; implementations in action_input.cpp) ---
    /// Current analog strength of a bound action, 0..1: max over the action's
    /// bindings. Keyboard keys and mouse buttons contribute 0/1, gamepad
    /// buttons their analog value (0/1 digital, analog for triggers), and
    /// axis-direction bindings their deadzone-rescaled deflection
    /// (m - deadzone) / (1 - deadzone), clamped to 0..1.
    float actionStrength(const std::string& action) const;
    /// Polled pressed state of a bound action. Axis-direction bindings use
    /// the same hysteresis latch that drives their "action" events, so this
    /// always agrees with the down/up stream.
    bool actionPressed(const std::string& action) const;

    /// Programmatic focus transfer from JS .focus()/.blur() on the app
    /// document: commits any in-progress IME composition, mirrors the
    /// control focused flags (so typing works after .focus(), as in a
    /// browser), and starts/stops SDL text input to match the new focus.
    /// No-op for documents other than the app document (iframes/panels).
    void handleProgrammaticFocus(dom::Document* doc, dom::Element* oldEl,
                                 dom::Element* newEl);

    // Clipboard simulation (for headless testing — bypasses system clipboard)
    void simulatePaste(const std::string& text);
    std::string simulateCopy();
    std::string simulateCut();

    /// document.execCommand(): run a named editing command against the current
    /// Selection, as the equivalent key press would. Returns false for an
    /// unsupported command and for a supported one that had nothing to act on
    /// (no editable selection, an empty history) — the same true/false
    /// contract browsers give. `showUI` is accepted and ignored, as it is
    /// everywhere; `value` carries the argument for the commands that take one
    /// ("insertText"). Implemented in input_handling.cpp on the same edit
    /// primitives the keyboard path uses, so a scripted command and a typed
    /// one produce identical DOM, undo entries and beforeinput/input events.
    bool execCommand(const std::string& name, bool showUI,
                     const std::string& value);
    /// Whether `name` is a command this build implements at all — a static
    /// property of the name, independent of the current selection.
    bool queryCommandSupported(const std::string& name) const;
    /// Whether `name` would do something right now: supported, and with a
    /// selection (and, for undo/redo, a history) it can act on.
    bool queryCommandEnabled(const std::string& name);

    float getLastMouseX() const { return lastMouseX_; }
    float getLastMouseY() const { return lastMouseY_; }

    /// Resolved OS cursor shape for the current hover target, as a stable
    /// name ("default", "pointer", "text", "move", "crosshair", "wait",
    /// "progress", "not-allowed", "ew-resize", "ns-resize", "nesw-resize",
    /// "nwse-resize", "none"). Updated on every app-document mouse move from
    /// the hovered element's computed `cursor`; in windowed mode the same
    /// shape is applied to the OS cursor. This is the headless seam for
    /// asserting the CSS cursor → OS cursor mapping (currentCursor() global).
    const std::string& resolvedCursor() const { return resolvedCursor_; }

    // --- Pointer lock ---
    // requestPointerLock: freeze the reported cursor position, enable SDL relative
    // mouse mode, and route all subsequent mousemove events to `target` until
    // exitPointerLock() is called. Fires "pointerlockchange" on documentElement.
    bool requestPointerLock(dom::Element* target);
    void exitPointerLock();
    dom::Element* pointerLockElement() const { return lockedElement_.get(); }

    // --- Pointer capture (Element.setPointerCapture) ---
    // While captured, pointermove/pointerup/pointercancel for that pointerId
    // dispatch to the captured element regardless of the hit target (offsetX/Y
    // recomputed against it), and the capture auto-releases after pointerup /
    // pointercancel — the web's drag idiom, so a drag whose release lands
    // off-element still reaches the element that started it. Mouse events keep
    // normal hit-test targeting. Capture is per pointerId: the mouse pointer is
    // always id kMousePointerId (1); touch contacts get unique ids ≥ 2 (see
    // handleTouchDown), and each can be captured independently. Fires
    // gotpointercapture / lostpointercapture.
    static constexpr int kMousePointerId = 1;
    bool setPointerCapture(dom::Element* target, int pointerId = kMousePointerId);
    void releasePointerCapture(dom::Element* target, int pointerId = kMousePointerId);
    bool hasPointerCapture(const dom::Element* target, int pointerId = kMousePointerId) const;

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

    /// Register a C++ listener for a window event on the app realm — the C++
    /// half of `window.addEventListener(type, fn, opts)`, for a host whose app
    /// JS was compiled away and has nothing to attach from.
    ///
    ///     auto h = engine->addWindowEventListener("resize", [engine](dom::Event&) {
    ///         camera.setAspect(float(engine->contentWidth()) / engine->contentHeight());
    ///     });
    ///     ...
    ///     engine->removeWindowEventListener(h);   // attaching again? remove first
    ///
    /// Fires alongside the realm's JS window listeners, in registration order
    /// across both kinds. The callback gets a real dom::Event: `type`,
    /// `timeStamp`, `isTrusted`, and preventDefault() / stopPropagation() /
    /// stopImmediatePropagation() that cut short the rest of the dispatch
    /// exactly as they would from JS. `target()` and `currentTarget()` are null
    /// — the window is not an Element — so a resize listener reads the new size
    /// from the engine (contentWidth() / contentHeight() / viewportWidth()),
    /// the way a JS one reads window.innerWidth.
    ///
    /// Fires for every event dispatched at this realm's window, whoever fired
    /// it: engine-generated ones (resize, gamepadconnected, message,
    /// visibilitychange, DOMContentLoaded), JS `window.dispatchEvent(...)`, and
    /// events bubbling out of the DOM tree to the window. Payload a JS caller
    /// hung on the event object (CustomEvent.detail and the like) does not
    /// cross to the C++ side — see js::dispatchWindowEvent.
    ///
    /// This is the app realm only. <iframe> sub-documents and secondary windows
    /// are separate realms with separate window listeners; reach those through
    /// their own Document::windowListeners().
    ///
    /// Survives nothing that replaces the document: location.reload() builds a
    /// new Document, and the listeners go with the old one.
    dom::ListenerHandle addWindowEventListener(const std::string& type,
                                               dom::EventCallback cb,
                                               dom::ListenerOptions opts = {});

    /// Unregister a listener from addWindowEventListener. True if it was live.
    bool removeWindowEventListener(dom::ListenerHandle handle);

    /// Give `canvas` a 3D scene rendering context and return its SceneGraph —
    /// the exact thing `canvas.getContext('scene')` hands to JS. This IS the
    /// implementation of that factory branch: the JS binding calls straight
    /// through here, so a host that builds its scene from C++ and an app that
    /// builds it from script get identically-registered graphs. There is no
    /// second construction path, by design.
    ///
    /// A scene context is not just a SceneGraph. It is a CanvasScene with
    /// layout / detached / liveness callbacks aimed at `canvas`, registered
    /// with the engine; the graph itself parked in sceneGraphs_ so the frame
    /// loop renders it; and an FBO-texture callback wired back to the element
    /// so the compositor gets a layer for it. A graph built with make_unique
    /// outside this function has none of that — it is never rendered and never
    /// composited, which looks exactly like "the renderer is broken".
    ///
    /// Idempotent per element, matching getContext()'s spec'd behaviour: a
    /// canvas that already has a scene context gets that same SceneGraph back,
    /// no second registration.
    ///
    /// Returns nullptr when there is no scene to give:
    ///   - `canvas` is null,
    ///   - the build has BRO_WITH_3D off,
    ///   - or there is no GL context (--no-gpu, or a headless boot that fell
    ///     back to raster). The 3D renderer is GL from top to bottom, so this
    ///     is the same documented "no scene here" null getContext('scene')
    ///     already returns on that path — callers must branch on it.
    ///
    /// Works identically in windowed and headless mode; runHeadless() reaches
    /// it through the same Engine.
    scene::SceneGraph* createSceneContext(dom::Element* canvas);

    /// How many scene contexts are registered (i.e. how many SceneGraphs the
    /// frame loop will render). 0 without BRO_WITH_3D. Diagnostic: the one
    /// externally visible signal that a createSceneContext call did or did not
    /// register something.
    size_t sceneContextCount() const;

    /// Bring `doc`'s layout up to date NOW, because JS is about to read
    /// geometry out of it. CSSOM calls this flushing pending layout, and it is
    /// what makes `parent.appendChild(el); el.getBoundingClientRect()` answer
    /// with the box the element actually has rather than the box it had before
    /// it existed. Without it the only way to measure anything an app just
    /// built is to wait a frame, which every caller then has to know — and the
    /// stale answers are not even honestly zero (a fresh element reported its
    /// parent's width for clientWidth and 0 for getBoundingClientRect().width
    /// in the same turn).
    ///
    /// Cheap when nothing changed: a clean document returns immediately, so a
    /// read loop over an untouched tree costs one flag test per read. A write
    /// between every read is layout thrashing and costs a layout each time, on
    /// the web as much as here.
    ///
    /// Only lays out. No observers are notified, no events dispatched, no
    /// sub-documents built — those belong to the frame drain, and firing them
    /// from inside a property getter would run app code in the middle of an
    /// expression that only asked how wide something is.
    void flushLayoutForRead(dom::Document* doc);

    /// Request an <iframe> element's sub-document be reloaded from its current
    /// src attribute (tear down + rebuild). Public: driven from JS by
    /// iframe.reload() and by assigning iframe.src. The actual teardown/rebuild
    /// is DEFERRED to the raster-idle point in the frame loop (see
    /// processPendingIframeReloads) — doing it synchronously here would free a
    /// sub-doc the raster thread may be mid-replay on. Fires "load" on rebuild.
    void reloadIframe(dom::Element* el);

    /// Request a full reload of the TOP-LEVEL app document: tear down the
    /// document + its JS realm and re-parse/re-run the app in the same Engine
    /// and window — what location.reload() does on the web. Only QUEUES
    /// (repeat calls coalesce): the caller is JS inside the very realm being
    /// destroyed, so the actual work is deferred to a point with no JS on the
    /// stack — the frame-loop drain (windowed) or between the driver's
    /// evaluation units (headless). No-op in Server mode (no document).
    void requestAppReload();

    /// Perform a queued requestAppReload(), if any. Returns true if a reload
    /// ran (the primary JSContext has been replaced — callers holding the old
    /// pointer must re-fetch it from jsRuntime()). MUST be called only when no
    /// JS from the app realm is on the stack and, windowed, only when the
    /// layout + raster workers are idle (the frame loop's drain point does
    /// both). Public for the headless driver, which drains between scripts.
    bool processPendingAppReload();

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

    /// Real GPU time (milliseconds) spent rendering the 3D scene graph(s) in the
    /// most recent flush(). Measured with a GL_TIME_ELAPSED timer query wrapped
    /// around the scene render, so it reflects actual GPU cost — unlike wall-clock
    /// timing around flush(), which returns before the GPU finishes. Blocking:
    /// reads GL_QUERY_RESULT, forcing that frame's GPU work to complete, so each
    /// call yields an isolated per-frame GPU cost. Returns -1 if no GPU scene was
    /// rendered (2D-only page, --no-gpu, or before the first scene flush).
    /// Consumed by the headless perf.gpuFrameMs() binding.
    double gpuFrameMs();

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

    // --- bro.time: global pause + timescale (Godot Engine.time_scale /
    //     SceneTree.paused analog) ---
    // The engine owns one scaled clock, engineNowMs_, advanced at frame top
    // by wallDt * effectiveTimeScale(). Everything gameplay-visible reads it:
    // JS timers, rAF timestamps, performance.now, CSS transitions/animations
    // (via the layout snapshot), the physics accumulator, scene agents and
    // animations, and iframe sub-documents. Engine chrome (system panels,
    // menu, perf HUD, GC cadence, UI throttle) stays on wall time so pause
    // never freezes the shell. Audio: pause suspends output (broaudio master
    // pause); timescale never pitch-shifts.

    /// Time multiplier for the scaled clock. Clamped to [0, 100]. Default 1.
    double timeScale() const { return timeScale_; }
    void setTimeScale(double scale);

    /// Global pause — effective scale 0, rAF callbacks skipped, audio output
    /// suspended.
    bool timePaused() const { return timePaused_; }
    void setTimePaused(bool paused);

    /// Current scaled engine time in ms (the clock timers/rAF/transitions
    /// run on). Read-only from JS as bro.time.now.
    double timeNowMs() const { return engineNowMs_; }

    /// Web Animations (element.animate) records — created/controlled by the
    /// JS bindings, ticked/applied on the same seams as the CSS managers.
    WebAnimationManager& webAnimationManager() { return webAnimationManager_; }

    /// 0 while paused, else timeScale_.
    double effectiveTimeScale() const { return timePaused_ ? 0.0 : timeScale_; }

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

    /// Re-resolve the hovered element's computed `cursor` into an OS cursor
    /// shape: updates resolvedCursor_ in all modes, and applies the shape to
    /// the OS cursor in windowed mode (skipped under pointer lock — relative
    /// mouse mode owns cursor visibility there). Cheap when nothing changed:
    /// Window::setCursor no-ops on an unchanged shape.
    void updateCursorFromHover(dom::Element* target);

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
                            const std::string& inputType = "",
                            bool isComposing = false);
    // --- IME composition helpers (input_handling.cpp) ---
    /// True when the app document's focused input/textarea has a preedit,
    /// or a contenteditable composition is in progress (editComp_).
    bool compositionActive();
    /// Dispatch a bubbling compositionstart/update/end event.
    void dispatchCompositionEvent(dom::Element* el, const char* type,
                                  const std::string& data);
    /// Commit the in-progress preedit as final text (the browser's blur /
    /// caret-move behavior) with the full compositionupdate → input →
    /// compositionend sequence. No-op when nothing is composing. Called
    /// before focus changes, mouse presses, and caret-moving keys so
    /// provisional text is never stranded.
    void commitActiveComposition();
    /// Report the focused text control's caret rect — or, with no focused
    /// control, the DOM Selection caret inside a contenteditable host — to
    /// SDL (SDL_SetTextInputArea) so the native IME candidate window tracks
    /// the caret. No-op without a window or an eligible caret.
    void updateTextInputArea();
    // Contenteditable composition mutations (input_handling.cpp). Each
    // performs the DOM splice only; callers dispatch the composition/input
    // events (mirroring how the control paths consume KeyHandleResult).
    /// The live, still-valid text node carrying the contenteditable preedit,
    /// or nullptr (dropping the composition) when the node died or script
    /// rewrote its data out from under the composition.
    dom::TextNode* editableCompositionTarget();
    /// Replace (or start) the preedit at the Selection caret. On start,
    /// `replacedSel` receives the selected text the composition replaced
    /// (compositionstart.data) and `hostOut` the contenteditable host the
    /// events should target. Returns false when the caret isn't editable.
    bool editableCompositionUpdate(const std::string& text, int cursorCp,
                                   bool& wasComposing, std::string& replacedSel,
                                   dom::Element*& hostOut);
    /// Finalize the preedit as `text` (one coherent splice). False = no
    /// contenteditable composition in progress (or its node died).
    // `cancel` distinguishes an abandoned composition (restore the host,
    // record no undo entry) from a real commit (one discrete entry).
    bool editableCompositionCommit(const std::string& text,
                                   dom::Element*& hostOut, bool cancel = false);
    /// Remove the preedit, restoring the pre-composition DOM. A text node
    /// created for the composition is removed again.
    bool editableCompositionCancel(dom::Element*& hostOut);

    // --- contenteditable edit primitives ---------------------------------
    // One implementation per editing operation, shared by the keyboard path
    // and execCommand(). Each derives its own caret from the Selection (the
    // same derivation handleKeyDown does), dispatches beforeinput → mutate →
    // input, and records the undo entry, so a scripted command and the key
    // press it names cannot drift apart. All return false when the Selection
    // isn't in an editable host, which is what makes them safe to call
    // unconditionally from execCommand.
    /// Backspace (`backward`) / Delete. Deletes the selected range when the
    /// selection isn't collapsed, otherwise one character on that side.
    bool editDeleteAtCaret(bool backward);
    /// Enter — inserts a <br>, replacing any selected range first.
    bool editInsertLineBreak();
    /// Typing `text` at the caret, replacing any selected range.
    bool editInsertTextAtSelection(const std::string& text);
    /// Undo (`redo` false) / redo one history entry for the caret's host.
    /// False when the host has no history, or none in that direction.
    bool editHistoryStep(bool redo);
    /// Select the caret's contenteditable host's children, else the body's —
    /// the Ctrl+A rule, which deliberately works outside an editable too.
    bool editSelectAll();

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

    // ── App-realm lifecycle (engine_init.cpp + app_reload.cpp) ──────────────
    // The constructor and top-level location.reload() share these. Engine-level
    // objects (window, renderer, services, workers, audio) are created once in
    // the constructor; everything bound to the primary JSContext or the app
    // document goes through here so a reload can rebuild it on a fresh realm.
    /// Install the mode-independent bindings every context gets (brokit, timers,
    /// physics, mesh/flora/math/ai, the ML tower, net/steam/server). Called for
    /// the app realm (via initAppRealm) and the Server-mode context.
    void installCoreBindings(JSContext* ctx);
    /// Build the app realm on the CURRENT primary context: all bindings, the
    /// app document (manifest → parse → scripts → fonts → layout → iframes),
    /// and the DOMContentLoaded/load dispatch. Windowed + Headless only.
    void initAppRealm();
    /// Tear down the current app realm and rebuild it via initAppRealm() on a
    /// fresh context. The body behind processPendingAppReload().
    void performAppReload();
    /// (Re)build the engine's default menu tree (File/Edit/View). Used at
    /// construction and by performAppReload() to drop app-added items whose
    /// handlers died with the old realm.
    void resetMenuBarDefaults();

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
    // Catch a sub-document up with its <iframe> element's current content box:
    // media viewport (so CSS @media and matchMedia re-evaluate against the new
    // size), a restyle+relayout at that size, and the realm's innerWidth /
    // innerHeight + a 'resize' event. The iframe counterpart of
    // syncWindowHostBox; idempotent, returns immediately when nothing changed.
    void syncIframeBox(IframeDoc& d);
    void syncAllIframeBoxes();
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
    /// SubDocRef views of the two sub-document flavours — the bridge to the
    /// shared helpers in engine/sub_document.h.
    SubDocRef iframeSubDoc(IframeDoc& d);
    SubDocRef windowHostSubDoc(WindowHost& h);
    /// Block until the raster worker is idle so a synchronous capture can
    /// re-record sub-documents it may otherwise be mid-replay on.
    void quiesceRasterForCapture();
    /// Read back a sub-document's last published texture with no re-record —
    /// the fallback when there is no main-thread GPU Skia (--no-gpu).
    std::vector<uint8_t> readbackSubDocTexture(unsigned int tex, int w, int h,
                                               int& outW, int& outH);
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
    WebAnimationManager webAnimationManager_;

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
        // Highlight fill for this selection — ::selection background-color
        // resolved on the main thread (element-scoped at the selection start),
        // falling back to the translucent accent default. Snapshotted so the
        // raster thread never touches the cascade.
        bromath::Color highlight{0.0f, 0.0f, 0.0f, 0.0f};
        // Contenteditable IME preedit underline: one thin segment per line
        // the composition range covers (same visual as the controls' preedit
        // underline), in the text color of the composition's host element.
        std::vector<Rect> compUnderlines;
        bromath::Color compColor{0.0f, 0.0f, 0.0f, 1.0f};
    };
    SelectionSnapshot selectionSnapshot_;

    bool running_ = false;
    bool shutdownDone_ = false;  // shutdown() guard — run() and ~Engine() both call it
    int viewportWidth_;
    int viewportHeight_;
    // OS display scale (window.devicePixelRatio). Read from the window at
    // init and on SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED in windowed mode;
    // pinned to 1.0 in headless so tests are reproducible across desktops.
    float displayScale_ = 1.0f;
    // Resolved OS cursor shape name for the current hover target — see
    // resolvedCursor(). Maintained in headless too (the mapping is the
    // testable part; only the SDL apply is windowed-gated).
    std::string resolvedCursor_ = "default";

    // Pre-compiled observer check function (avoids JS_Eval parse per frame)
    JSValue observerCheckFn_ = JS_UNDEFINED;
    AppManifest manifest_;
    // Launch config the app realm is (re)built from: the app directory and
    // the optional window-title override. Stored so initAppRealm() can re-run
    // the full load on a location.reload() without the EngineConfig.
    std::string appDir_;
    /// Host application's binding installer (EngineConfig::installHostBindings).
    std::function<void(JSContext*)> installHostBindings_;
    std::string titleOverride_;
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

    /// Fit every WebGL canvas's drawing buffer to its element box, for canvases
    /// that have not set width/height themselves. Per HTML, those attributes
    /// ARE the drawing-buffer size, so an app that sets them (typically to
    /// clientWidth * devicePixelRatio) owns the size and the engine must not
    /// fight it every frame.
    void syncWebGLCanvasSizes();

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
        // Non-owning, and allowed to dangle. Nothing removes the entry when the
        // canvas Element is destroyed — the deferred-free drain and ~Document
        // both free Elements without consulting this list — so `element` must
        // never be dereferenced directly. `document` + `elementId` are what make
        // it usable: Document::resolveNode is a pointer-value lookup plus a
        // generation check, so it is safe on freed storage and cannot be fooled
        // by the allocator handing that address to a new node.
        dom::Element* element = nullptr;
        dom::Document* document = nullptr;
        uint32_t elementId = 0;
    };
    std::vector<SceneGraphEntry> sceneGraphs_;

    /// `entry.element` when it is still a live node of a still-live document,
    /// nullptr otherwise. Never dereferences a dangling pointer.
    dom::Element* liveElementOf(const SceneGraphEntry& entry) const;
    /// Drop every scene graph whose canvas has left the document tree (or has
    /// been destroyed outright), severing the Element→graph back-pointer before
    /// the graph goes. Shared by the frame loop and the headless flush; they
    /// used to carry a copy each, and only one of them can be fixed at a time
    /// when the rule changes.
    void pruneDetachedSceneGraphs();
    /// Drop every scene graph, severing back-pointers first. Teardown and
    /// app-reload path.
    void clearSceneGraphs();
#endif
    double physicsAccumMs_ = 0.0;
    double lastPhysicsTimeMs_ = 0.0;
    double lastFrameTimeMs_ = 0.0; // scaled-clock time of previous frame's start (for syncAgents dt)

    // --- bro.time scaled clock state ---
    double timeScale_ = 1.0;
    bool   timePaused_ = false;
    // The scaled clock (ms). Seeded from the same value as the first
    // timers_->tick() so timer deadlines and this clock never diverge;
    // advanced once per frame (windowed/server) or per advanceTime step
    // (headless, in lockstep with virtualTime_ at scale 1).
    double engineNowMs_ = 0.0;
    // Wall time of the previous scaled-clock advance (0 = not yet sampled).
    double lastWallTickMs_ = 0.0;
    // System panels (settings, perf, nav)
    std::vector<SystemDocument> systemDocs_;
    // Iframe sub-documents, keyed by their <iframe> element. unique_ptr so the
    // IframeDoc address is stable (captured by canvas getContext factories and
    // referenced from dom::Element::iframeDoc()).
    std::vector<std::unique_ptr<IframeDoc>> iframeDocs_;
    uint64_t nextIframeId_ = 1;
    // Secondary window hosts (see the public WindowHost section). unique_ptr
    // so host addresses stay stable across registry mutation.
    std::vector<std::unique_ptr<WindowHost>> windowHosts_;
    uint64_t nextWindowHostId_ = 1;
    // Which secondary window has input focus (0 = none / the main window).
    // Keyboard, text input and IME follow this; mouse events carry their own
    // windowID and never consult it.
    uint64_t focusedHostId_ = 0;
    // Host → app-realm messages awaiting the next drain, in post order across
    // ALL hosts (each entry names its sender). Parent-bound mail outlives its
    // window deliberately: a child that posts and then closes itself still gets
    // its last word delivered, as long as the handle is still around.
    std::vector<std::pair<uint64_t, std::unique_ptr<js::Message>>> hostToParentMessages_;
    /// Seed a queued host's options from the child app's own bro.json (window
    /// keys only), for every option the open() caller did not pass explicitly.
    void applyChildManifestDefaults(WindowHost& h, const std::string& appDir);
    /// Per-window composite pass (windowed frame loop, after the main
    /// composite): for each created, non-minimized host — MakeCurrent(host,
    /// main ctx), viewport, clear to the host's color, swap at interval 0 —
    /// then restore the main drawable + its vsync preference. The main swap
    /// runs after this and stays the frame's single pacing swap.
    void compositeWindowHosts();
    /// Build a host's document realm from opts.src (queued-create drain), and
    /// tear one down. Both run only at the raster-idle drain point.
    void createWindowHostDoc(WindowHost& h, struct SubDocSource& source);
    void teardownWindowHostDoc(WindowHost& h);
    /// Refresh a host's layout box from its OS window's current client size,
    /// pushing innerWidth/innerHeight + a 'resize' event into its realm when it
    /// changed. Called before each record.
    void syncWindowHostBox(WindowHost& h);
    /// Tick every host document's timers + rAF; true if any needs recording.
    bool tickWindowHosts(double nowMs);
    /// Main thread: record each host document into its own command buffer.
    void recordWindowHostLayers();
    /// Raster thread: replay each host document into its window-sized surface.
    void replayWindowHostLayers(render::SkiaRenderer* renderer);
    /// Destroy every host synchronously (window + registry entry).
    /// notifyJs fires each handle's 'close' first — app-reload teardown wants
    /// that false-with-cleanup handled by the bindings' own cleanup instead.
    /// Callers must hold the same quiescence the drain point has.
    void destroyAllWindowHosts(bool notifyJs);

    // ── Per-window input internals (src/engine/window_host_input.cpp) ───────
    /// Hit-test a host document at window coordinates. Simpler than
    /// iframeHitTest: there is no element box to translate out of, because a
    /// host surface IS the window (window-size units in v1, so no HiDPI
    /// scaling to undo either).
    dom::Element* windowHostHitTest(WindowHost& h, float x, float y);
    /// The ControlContext a host document's replaced elements run under: the
    /// host's own document/realm/window, no overlay manager (dropdowns and
    /// the colour picker are main-window chrome — a <select> in a secondary
    /// window focuses but does not open a popup in v1), viewport = the window.
    ControlContext windowHostControlContext(WindowHost& h);
    /// Dispatch a DOM event on the host's own JS realm.
    void windowHostDispatch(WindowHost& h, dom::Element* el, dom::Event& evt);
    void windowHostDispatchInput(WindowHost& h, dom::Element* el,
                                 const std::string& data = "",
                                 const std::string& inputType = "",
                                 bool isComposing = false);
    void windowHostDispatchComposition(WindowHost& h, dom::Element* el,
                                       const char* type, const std::string& data);
    void windowHostDispatchFocus(WindowHost& h, dom::Element* oldEl,
                                 dom::Element* newEl);
    /// applyKeyResult's per-host twin: dispatch the control's change/input
    /// events on the host realm, honour unfocus against the host's document
    /// and SDL window, and repaint the host.
    void windowHostApplyKeyResult(WindowHost& h, dom::Element* el,
                                  const layout::KeyHandleResult& r);
    /// Re-resolve the host's hovered element's computed `cursor` into
    /// h.resolvedCursor, and (windowed) apply it to THAT window's OS cursor —
    /// Window::setCursor caches per window, so this never fights the main one.
    void windowHostUpdateCursor(WindowHost& h, dom::Element* target);
    /// Report the host's focused text control's caret rect to SDL against the
    /// host's OWN window, so the native IME candidate box appears there.
    void windowHostUpdateTextInputArea(WindowHost& h);
    /// Tab focus navigation inside one host document.
    void windowHostAdvanceFocus(WindowHost& h, bool reverse);
    /// Commit any in-progress composition in the host's focused control.
    void windowHostCommitComposition(WindowHost& h);
    /// Shared body of hostDropFile/hostDropText: hit-test and fire the
    /// dragenter → dragover → drop triple with the given payload.
    void windowHostDispatchDrop(WindowHost& h, float x, float y,
                                const std::vector<std::string>* paths, const std::string* text);
    /// Mark a host document for re-record. Caret moves and control focus
    /// change no DOM, so tickSubDoc's isDirty() check alone would keep
    /// re-presenting the stale frame.
    void windowHostRepaint(WindowHost& h);
    /// Per-realm page visibility for one host (document.hidden +
    /// visibilitychange), through the same __bro_set_visibility hook the app
    /// realm uses.
    void windowHostSetVisibility(WindowHost& h, bool visible);
    /// System hotkeys (the settings-bound system_toggle_perf /
    /// system_toggle_settings actions) are GLOBAL: they fire whichever bro
    /// window has focus. Returns true when the keystroke was consumed.
    bool handleGlobalHotkey(int keycode, int mod, bool repeat);
    // Top-level location.reload() queued and not yet performed. Set by
    // requestAppReload() (from JS in the doomed realm), consumed by
    // processPendingAppReload() at the frame loop's idle-workers point
    // (windowed) or between evaluation units (headless driver). A bool, so
    // repeat calls within one frame naturally coalesce.
    bool pendingAppReload_ = false;
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

    // Pointer capture, per pointerId: while held, pointer events (not mouse
    // events) for that id route to the captured element. Cleared by
    // pointerup/pointercancel, an explicit releasePointerCapture(), or (mouse
    // pointer only) a buttons-free pointermove — self-heal when the release
    // was never delivered. Key kMousePointerId is the mouse; touch contacts
    // use their per-contact pointerIds.
    std::unordered_map<int, dom::ElementHandle> pointerCaptures_;
    /// The element currently capturing `pointerId`, or null.
    dom::Element* pointerCaptureFor(int pointerId) const;

    // --- Touch contact table (implementations in touch_input.cpp) ---
    // One live entry per finger on the surface. pointerIds are minted from
    // nextTouchPointerId_ (monotonic, starts at 2) so they are unique per
    // contact and never collide with the mouse's pointerId 1. The first
    // contact of a contact set (touch on an empty table) is the primary
    // pointer for its whole lifetime. Touch NEVER drives hover —
    // hoveredElement_ / :hover stay mouse-only.
    struct TouchContact {
        uint64_t fingerId = 0;      // SDL finger id / injector-chosen id
        int pointerId = 0;          // W3C PointerEvent.pointerId (≥ 2)
        bool primary = false;       // first contact of the current set
        float x = 0.0f, y = 0.0f;   // latest position (window space)
        float downX = 0.0f, downY = 0.0f;  // position at touch-down
        float pressure = 1.0f;
        bool moved = false;         // travelled past the tap slop
        bool compatSuppressed = false; // preventDefault on pointerdown/touchstart
        // touchstart hit target — Touch Events for this contact keep firing
        // here for its whole lifetime (W3C Touch Events targeting rule).
        dom::ElementHandle startTarget;
    };
    std::vector<TouchContact> touchContacts_;
    int nextTouchPointerId_ = 2;
    TouchContact* touchByFinger(uint64_t fingerId);
    /// Dispatch one pointer event for a touch contact. Routes to the contact's
    /// captured element if any (except pointerdown), else hit-tests the
    /// contact point. Returns true if a listener called preventDefault().
    bool dispatchTouchPointerEvent(const char* type, const TouchContact& c,
                                   bool cancelable);
    /// Dispatch one W3C Touch Event (touchstart/move/end/cancel) at the
    /// contact's start target, building JS Touch/TouchList/TouchEvent
    /// instances via the polyfill constructors. `changed` is the contact
    /// this event reports; the live lists come from touchContacts_.
    /// Returns true if a listener called preventDefault().
    bool dispatchTouchEvent(const char* type, const TouchContact& changed,
                            bool cancelable);
    /// Compat mouse sequence for a primary-contact tap:
    /// mousedown → mouseup → click through the standard doc dispatch helpers
    /// (focus semantics included), no pointer aliases.
    void dispatchCompatMouseForTap(const TouchContact& c);

    // --- Two-finger gesture recognition (pinch/pan/rotate) ---
    // SDL3 dropped SDL2's gesture subsystem, so the engine recognizes
    // gestures itself from the touch contact table and dispatches
    // WebKit-style gesturestart / gesturechange / gestureend events (scale,
    // rotation, clientX/clientY as event properties) on the hit target of
    // the gesture's start centroid. A gesture is anchored to its two
    // FOUNDING contacts (the two oldest fingers); lifting either founder
    // ends it (and immediately starts a fresh one if 2+ fingers remain —
    // with scale/rotation re-based to 1/0). Additional fingers beyond the
    // founding pair are ignored. Regular pointer/touch events fire
    // untouched. Implementations in touch_input.cpp.
    struct GestureState {
        bool active = false;
        uint64_t fingerA = 0, fingerB = 0;  // founding contacts
        float startDist = 1.0f;             // finger distance at start (px, >= 1)
        float startAngle = 0.0f;            // atan2 angle at start (radians)
        float scale = 1.0f;                 // last reported scale
        float rotation = 0.0f;              // last reported rotation (degrees,
                                            // unwrapped: continuous past 180)
        float cx = 0.0f, cy = 0.0f;         // last centroid (window space)
        dom::ElementHandle target;          // hit target of the start centroid
    };
    GestureState gesture_;
    /// Start a gesture if none is active and 2+ contacts are down.
    void gestureMaybeStart();
    /// Recompute scale/rotation/centroid and dispatch gesturechange if the
    /// moved finger is one of the founding pair.
    void gestureUpdate(uint64_t movedFinger);
    /// Dispatch gestureend and clear the gesture if the ended finger was a
    /// founder; then re-start over the remaining contacts if 2+ are left.
    void gestureEndIfFounder(uint64_t endedFinger);
    void dispatchGestureEvent(const char* type);

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
    /// Stick-axis value changed (real SDL path AND the headless virtual-axis
    /// seam): updates the slot, then evaluates "gamepad:<axis>+/-" action
    /// bindings (action_input.cpp) so both producers drive identical action
    /// dispatch.
    void gamepadAxisChanged(GamepadState& gp, int w3cAxis, float value);

    // --- Action binding dispatch (implementations in action_input.cpp) ---
    /// If `key` (a binding string: web key, "mouse:<button>", or
    /// "gamepad:<name>") is bound to an action, dispatch the "action"
    /// CustomEvent on document.body with detail {action, phase, key,
    /// strength, gamepad?}. `gamepadIndex` >= 0 adds detail.gamepad.
    void dispatchActionEventForKey(const std::string& key, const char* phase,
                                   float strength, int gamepadIndex = -1);
    /// Edge-detect "gamepad:<axis>+/-" bindings for one axis of one pad,
    /// with deadzone hysteresis (press at deadzone, release below
    /// deadzone * kActionAxisReleaseFactor).
    void evaluateAxisActions(GamepadState& gp, int w3cAxis);
    /// Mouse-button ("mouse:left" etc.) action edges. Down is dispatched only
    /// when the press reaches the app layer (overlay/system-consumed presses
    /// never start an action — mirroring how consumed keydowns skip the
    /// keyboard action dispatch); the matching up always fires once a down
    /// was seen (actionMouseDownMask_), keeping pairs balanced.
    void dispatchMouseButtonAction(int domButton, bool down);
    // DOM-convention buttons bitmask of mouse buttons that dispatched an
    // action "down" and still owe an "up".
    int actionMouseDownMask_ = 0;
    // Keyboard keys currently held (keycode -> webKey at press time), for
    // polled action state. Updated at the very top of handleKeyDown/Up so it
    // reflects physical key state regardless of DOM/overlay consumption.
    std::unordered_map<int, std::string> heldKeys_;
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

    // In-progress IME composition inside a contenteditable host of the app
    // document. Contenteditable has no control object (ElInput/ElTextarea
    // carry a TextComposition of their own), so the engine holds the state:
    // the preedit lives provisionally in `node`'s data at
    // [start, start + length) and is replaced on every TEXT_EDITING update,
    // finalized by TEXT_INPUT, removed by cancel. Like the controls, the
    // commit records ONE discrete undo entry spanning the pre-composition
    // state → the committed state, and a cancel records none — `hostBefore`
    // is the pre-composition snapshot both of those need, and is also what
    // lets a cancel resurrect a selection the composition replaced.
    struct EditableComposition {
        bool active = false;
        dom::TextNodeHandle node;   // text node carrying the preedit
        dom::ElementHandle host;    // contenteditable host (event target)
        bool createdNode = false;   // `node` was created for this composition
        int start = 0;              // byte offset of the preedit in `node`
        int length = 0;             // byte length of the current preedit
        std::string preedit;        // current preedit text
        std::string hostBefore;     // host innerHTML before the composition
        DomUndoStack::Sel selBefore;// selection before the composition
        bool replacedSelection = false;  // composition started over a selection
    };
    EditableComposition editComp_;

    // Undo/redo histories for contenteditable hosts, keyed by host element.
    DomUndoHistories editUndo_;

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

    // Headless GPU frame timing (GL_TIME_ELAPSED). flush() wraps the scene render
    // in this query; gpuFrameMs() reads it. unsigned int, not GLuint, to keep glad
    // out of engine.h. Query id is lazily created on first GPU scene flush.
    unsigned int gpuTimerQuery_ = 0;
    bool gpuTimerPending_ = false;   // a begin/end pair is awaiting a read
    double lastGpuFrameMs_ = -1.0;

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

/// The Engine whose realm `ctx` belongs to, or nullptr if `ctx` is not an
/// engine-owned realm (or the engine has already torn that realm down).
///
/// EngineConfig::installHostBindings and HeadlessHooks::installHostBindings are
/// both `void(JSContext*)` — a host binding is handed a realm and nothing else,
/// and in headless the host never sees the Engine at all (runHeadless builds it
/// internally). This is the supported way to get from the one to the other, so
/// a host binding can reach document(), createSceneContext(), and the rest of
/// the public Engine surface.
///
/// The mapping is the same per-realm engine pointer the engine's own DOM
/// bindings already use (js::DomBindings::setEngine), so this adds a reader,
/// not a second registry. Registered for the primary app realm before any
/// binding — including a host's — is installed, and dropped when that realm is
/// cleaned up. Sub-document and system-panel realms are NOT engine-owned in
/// this sense and answer nullptr; callers must check.
///
/// Main thread only. Do not cache the result across a location.reload(): the
/// realm is replaced and the host installer runs again on the new one.
Engine* engineForContext(JSContext* ctx);

} // namespace bro::engine
