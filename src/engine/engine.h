#pragma once

#include "engine/app_loader.h"
#include "engine/css_transitions.h"
#include "engine/app_permissions.h"
#include "engine/device_scale.h"
#include "engine/scroll_layers.h"
#include "engine/engine_config.h"
#include "engine/engine_fwd.h"
#include "engine/engine_types.h"
#include "engine/frame_presenter.h"
#include "engine/frame_stats.h"
#include "engine/gamepad.h"
#include "engine/iframe.h"
#include "engine/inspector_state.h"
#include "engine/menu_bar.h"
#include "engine/mouse_dispatch_state.h"
#include "engine/scrollbar.h"
#include "engine/settings.h"
#include "engine/system_document.h"
#include "engine/ui_layer.h"
#include "engine/web_animations.h"
#include "engine/window_host.h"
#include "engine/engine_drm.h"
#include "platform/window.h"
#include "dom/event_target.h"
#include "dom/node_handle.h"
#include "dom/scroll_absorber.h"
#include "engine/drag_drop.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "render/skia_backend.h"
#include "render/damage.h"
#include "render/kernel_canvas.h"
#include "util/asset_mounts.h"
#include "webgl/webgl_types.h"


#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <include/core/SkSurface.h>
#include <vulkan/vulkan.h>

namespace bro::render {
struct PresentFrame; struct PresentImage;
class VulkanContext; class VulkanSwapchain; class VulkanPresenter;
}
namespace bro::a11y { class AccessibilityBridge; }
namespace bro::layout { class ElRemoteView; }

namespace bro::engine {

class RemoteViewHost;
class GizmoManager;  // engine/gizmo.h
class DomUndoHistories;  // engine/dom_undo.h
class OverlayManager;  // engine/overlay.h
struct ControlContext;  // engine/replaced_elements.h

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();
private:
    // The constructor's work. A throw from it (no index.html, Vulkan or the
    // renderer failing to start) runs teardown() before the members go, in
    // the order ~Engine keeps: left to member destruction, Skia's context
    // would go before the renderer that draws with it.
    void construct(const EngineConfig& config);
    // ~Engine's body: what is torn down, in what order.
    void teardown();
public:

    void stopBackgroundServices();
    void shutdown();
private:
    void removeModalEventWatch();
public:
    using ContentInsets = bro::engine::ContentInsets; using LoadedFont = bro::engine::LoadedFont;
    using SelectionSnapshot = bro::engine::SelectionSnapshot; using WebGLEntry = bro::engine::WebGLEntry;
#if BRO_WITH_3D
    using SceneGraphEntry = bro::engine::SceneGraphEntry;
#endif
    using TouchContact = bro::engine::TouchContact; using GestureState = bro::engine::GestureState;
    using EditableComposition = bro::engine::EditableComposition; using IframeDoc = bro::engine::IframeDoc;
    using SystemDocument = bro::engine::SystemDocument; using WindowHost = bro::engine::WindowHost; using WindowHostOptions = bro::engine::WindowHostOptions;

    void run();
    void handleResize(int w, int h);
    void handleDisplayScaleChanged();
    float displayScale() const { return deviceScale_.ratio; }
    /// Device px rasterized per CSS px, and the frame's size in device px
    /// (what capturePixels() returns). See DeviceScale.
    float renderScale() const { return deviceScale_.render; }
    int framePixelWidth() const { return deviceScale_.drawableW; }
    int framePixelHeight() const { return deviceScale_.drawableH; }
    /// Headless: render as a display with `scale` device px per CSS px.
    void setDeviceScaleFactor(float scale);
    render::VulkanContext* vulkanContext() const { return vulkanContext_.get(); }
    render::SkiaGpu* skiaGpu() const { return skiaGpu_.get(); }
    render::VulkanPresenter* vulkanPresenter() const { return vulkanPresenter_.get(); }
    std::string effectiveColorScheme() const;
    void applyColorScheme();
    void deliverMediaQueryChangesAllRealms();

    // Input events forwarded from the event loop
    void handleMouseDown(float x, float y, int button);
    void handleMouseUp(float x, float y, int button);
    void handleMouseMove(float x, float y, float xrel, float yrel);
    void handleMouseMove(float x, float y) { handleMouseMove(x, y, 0.0f, 0.0f); }
    void handleKeyDown(int keycode, int scancode, int mod, bool repeat);
    void handleKeyUp(int keycode, int scancode, int mod, bool repeat);
    void handleTextInput(const std::string& text);
    void handleTextEditing(const std::string& text, int start, int length);
    void handleWheel(float x, float y, float dx, float dy);
    void drainWheelSmoothing(float frameDtSec);
    void handleDropFile(const std::vector<std::string>& paths, float x = -1, float y = -1);
    void handleDropFile(const std::string& path, float x = -1, float y = -1) { handleDropFile(std::vector<std::string>{ path }, x, y); }
    void handleDropText(const std::string& text, float x = -1, float y = -1);
    // A page's drag handed to the window system (engine_native_drag.cpp):
    // offered there when dragstart went through, then followed through the
    // window system's reports while it carries it.
    void beginNativeDrag();
    void handleOwnDragMotion(float x, float y);
    void handleOwnDragLeave();
    void handleOwnDragDrop(float x, float y);
    void handleOwnDragEnd(const std::string& action);

    /// Input from a device bro's platform layer does not own (a remote
    /// viewer, bro.remote), as such a device reports it: evdev KEY_* / BTN_*
    /// codes, an absolute pointer in frame pixels (framePixelWidth/Height),
    /// wheel in 120ths of a detent (+y down, libinput's sense). It is routed
    /// as local input is: under DRM through the libinput path (hotkeys, the
    /// shell or a client window, sharing the seat's modifiers and pointer);
    /// otherwise through the handle* entry points above, with key text the
    /// way the DRM path derives it. RelativeMotion (x, y: a delta in frame
    /// pixels) is what a viewer sends while the pointer is locked: it moves
    /// a locked client's pointer under DRM, and elsewhere reaches a page's
    /// pointer lock as movementX/Y. engine_device_input.cpp.
    struct DeviceInput {
        enum class Kind : uint8_t { Key, Motion, Button, Wheel, RelativeMotion } kind = Kind::Key;
        uint32_t code = 0;
        bool pressed = false;
        float x = 0.0f;
        float y = 0.0f;
        int32_t wheelX = 0;
        int32_t wheelY = 0;
    };
    void injectDeviceInput(const DeviceInput& in);
    /// Present the next frame even if it is unchanged (an on-demand readback
    /// of a window that does not read back every present).
    void presentNextFrame() { presentedKeyValid_ = false; }
    /// A key a hotkey grab holds for the desktop shell while another app has
    /// the foreground (Windows; platform/desktop_hotkeys.h ShellKeySink): to
    /// the document, with `mods` (hotkey_mod bits) as the modifiers held.
    void deliverShellGrabKey(uint32_t evdevCode, bool down, bool repeat, uint32_t mods);
    /// A key (platform Scancode) to the document with deviceInputMods_ held.
    void deliverDocumentKey(int32_t scancode, bool pressed, bool repeat);
    /// Where the pointer last was, CSS px.
    float pointerX() const { return lastMouseX_; }
    float pointerY() const { return lastMouseY_; }
    /// The pointer shape the screen shows (a CSS cursor name; "none" when
    /// hidden): the document's, or under DRM a client window's while the
    /// pointer is on one.
    std::string screenCursorShape() const;
    /// Whether the screen's pointer is locked: the page holds pointer lock,
    /// or under DRM the client window the pointer is on locked it. A remote
    /// viewer then sends relative motion.
    bool screenPointerLocked() const;

    /// What drives <remoteview> elements (remote_view_host.h); null: none,
    /// and a view shows only its background.
    void setRemoteViewHost(RemoteViewHost* host) { remoteViewHost_ = host; }
    RemoteViewHost* remoteViewHost() const { return remoteViewHost_; }
    /// The pointer shape over the hovered element, again (a remote view's
    /// screen changed its pointer).
    void refreshHoverCursor();

    // Gamepads (gamepad.cpp)
    void handleGamepadAdded(uint32_t instanceId);
    void handleGamepadRemoved(uint32_t instanceId);
    void handleGamepadButton(uint32_t instanceId, int button, bool down);  // platform::GamepadButton
    void handleGamepadAxis(uint32_t instanceId, int axis, float value);    // platform::GamepadAxis
    int  gamepadConnectVirtual(const std::string& id);
    bool gamepadDisconnectVirtual(int index);
    bool gamepadSetVirtualButton(int index, int w3cButton, bool pressed, float value);
    bool gamepadSetVirtualAxis(int index, int w3cAxis, float value);
    bool gamepadRumble(int index, float strongMagnitude, float weakMagnitude, int durationMs);
    bool gamepadRumbleTriggers(int index, float leftMagnitude, float rightMagnitude, int durationMs);

    // Touch input (touch_input.cpp)
    void handleTouchDown(uint64_t fingerId, float x, float y, float pressure = 1.0f);
    void handleTouchMove(uint64_t fingerId, float x, float y, float pressure = 1.0f);
    void handleTouchUp(uint64_t fingerId, float x, float y);
    void handleTouchCancel(uint64_t fingerId, float x, float y);

    // Secondary window hosts (window_host.cpp)
    uint64_t openWindowHost(const WindowHostOptions& opts);
    void closeWindowHost(uint64_t id);
    WindowHost* windowHostById(uint64_t id);
    WindowHost* windowHostBySdlId(uint32_t sdlId);
    WindowHost* windowHostForDocument(const dom::Document* doc);
    bool isWindowHostDocument(const dom::Document* doc) const;
    IframeDoc* iframeForDocument(const dom::Document* doc);
    // The other direction: the sub-document an <iframe> element hosts, which
    // is what iframe.contentDocument answers (host_iframe.cpp).
    IframeDoc* iframeDocForElement(const dom::Element* el);
    bool isIframeDocument(const dom::Document* doc) const;
    bool anyLiveWindowHosts() const;
    bool anyPresentableWindowHosts() const;
    bool anyWindowHostFocused() const;

    void handleWindowCloseRequested(uint32_t sdlWindowId);
    void handleHostResized(uint32_t sdlWindowId, int w, int h);
    void handleHostFocusChanged(uint32_t sdlWindowId, bool focused);
    void handleHostMinimized(uint32_t sdlWindowId, bool minimized);
    void handleHostOccluded(uint32_t sdlWindowId, bool occluded);
    void handleHostDisplayScaleChanged(uint32_t sdlWindowId);
    bool documentPageVisible(const dom::Document* doc) const;
    void processPendingWindowHosts();

    // Per-window input routing (window_host_input.cpp)
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
    uint64_t focusedWindowHostId() const { return focusedHostId_; }
    const std::string& resolvedCursor(uint64_t hostId) const;
    std::vector<uint8_t> captureWindowHost(uint64_t id, int& outW, int& outH);
    /// What was last put on screen (RGBA8, device px): the main window's
    /// frame for `hostId` 0, else that secondary window's. Windowed with
    /// BRO_CAPTURE_PRESENTS=1 only (every present is then also read back, for
    /// a test harness); empty otherwise.
    std::vector<uint8_t> presentedPixels(uint64_t hostId, int& outW, int& outH);
    // Headless: the frame the windowed loop (runFrames) last presented, in
    // device px (RGBA8), or empty when none was.
    std::vector<uint8_t> headlessPresentedPixels();
    /// Draws and presents the page again at the next frame though nothing
    /// changed: for a reader that just turned on present readback
    /// (bro.remote), which otherwise waits for the page's next change.
    void requestPresent() { uiDirty_ = true; }
    const std::vector<GamepadState>& gamepads() const { return gamepads_; }
    /// The page uses gamepads (getGamepads, a gamepad event listener, a
    /// "gamepad:" action binding): start reading controllers, once. Not at
    /// startup, where finding them would cost every app ~200 ms.
    void useGamepads();
    // Polled action state (action_input.cpp)
    float actionStrength(const std::string& action) const;
    bool actionPressed(const std::string& action) const;
    float getActionStrength(const std::string& action) const { return actionStrength(action); }
    bool isActionPressed(const std::string& action) const { return actionPressed(action); }

    void handleProgrammaticFocus(dom::Document* doc, dom::Element* oldEl,
                                 dom::Element* newEl);

    // Clipboard simulation (for headless testing — these do not touch the
    // system clipboard themselves; `execCommand` is what pairs them with it)
    void simulatePaste(const std::string& text);
    std::string simulateCopy();
    // `commit` is handed the text as soon as it is known and BEFORE
    // anything is deleted; returning false abandons the cut and answers
    // with an empty string. That is the ordering a clipboard cut needs:
    // the system clipboard can refuse a write (see
    // platform::setClipboardText), and a cut that deleted anyway would be
    // the text gone with nowhere to paste it back from. Headless `cut()`
    // passes nothing and keeps the old behaviour.
    std::string simulateCut(
        const std::function<bool(const std::string&)>& commit = {});

    bool execCommand(const std::string& name, bool showUI,
                     const std::string& value);
    bool queryCommandSupported(const std::string& name) const;
    bool queryCommandEnabled(const std::string& name);
    bool queryCommandState(const std::string& name) const;
    std::string queryCommandValue(const std::string& name) const;

    float getLastMouseX() const { return lastMouseX_; }
    float getLastMouseY() const { return lastMouseY_; }
    const std::string& resolvedCursor() const { return resolvedCursor_; }
    bool isCursorVisible() const { return cursorVisible_; }
    void setCursorVisible(bool v) { cursorVisible_ = v; }

    // Pointer lock
    bool requestPointerLock(dom::Element* target);
    void exitPointerLock();
    void setPointerLock(dom::Element* element) { requestPointerLock(element); }
    bool hasPointerLock() const { return lockedElement_.get() != nullptr; }
    dom::Element* pointerLockElement() const { return lockedElement_.get(); }
    dom::Element* lockedElement() const { return lockedElement_.get(); }

    // Pointer capture (Element.setPointerCapture)
    static constexpr int kMousePointerId = 1;
    bool setPointerCapture(dom::Element* target, int pointerId = kMousePointerId);
    void releasePointerCapture(dom::Element* target, int pointerId = kMousePointerId);
    bool hasPointerCapture(const dom::Element* target, int pointerId = kMousePointerId) const;
    void releaseAllPointerCaptures();

    // Document lifecycle
    const std::string& documentReadyState() const { return documentReadyState_; }
    void setDocumentReadyState(const std::string& state);
    void dispatchDocumentReadyEvents();

    // Page visibility (document.hidden) and window focus tracking.
    void setPageVisibility(bool visible);
    bool pageVisible() const { return pageVisible_; }
    void setFullscreenState(bool fullscreen);
    bool isWindowFocused() const { return windowFocused_; }
    void setWindowFocused(bool f) { windowFocused_ = f; }
    void dispatchWindowFocusChange(bool focused);

    // Headless & DOM API
    dom::Document* document() const { return document_.get(); }
#if BRO_WITH_A11Y
    a11y::AccessibilityBridge* a11yBridge() const { return a11yBridge_.get(); }
#else
    a11y::AccessibilityBridge* a11yBridge() const { return nullptr; }
#endif
    render::Renderer* renderer() const { return renderer_.get(); }

    dom::ListenerHandle addWindowEventListener(const std::string& type,
                                               dom::EventCallback cb,
                                               dom::ListenerOptions opts = {});
    bool removeWindowEventListener(dom::ListenerHandle handle);
    void dispatchElementEvent(dom::Element* target, dom::Event& event);
    void dispatchWindowEvent(dom::Event& event);

    // Close requests (Escape) for a document's top layer. The host answers for
    // the kinds of entry it owns — a modal dialog fires `cancel` and closes —
    // returning true when it took the request. requestTopLayerClose offers it
    // to the topmost entry, then the next, until one is taken.
    using TopLayerCloseRequestFn = std::function<bool(dom::Element*)>;
    void setTopLayerCloseRequestHandler(TopLayerCloseRequestFn fn) {
        topLayerCloseRequest_ = std::move(fn);
    }
    void requestTopLayerClose(dom::Document* doc);

    scene::SceneGraph* createSceneContext(dom::Element* canvas);
    size_t sceneContextCount() const;
    webgl::WebGL2RenderingContext* createWebGL2Context(dom::Element* canvas,
                                                       const webgl::ContextAttributes& attributes = {});
    /// The display of a 'kernel' canvas (docs/kernel-api.js): null without a
    /// GPU device.
    render::KernelCanvas* createKernelCanvas(dom::Element* canvas);
    canvas::CanvasScene* createCanvasContext(dom::Element* canvas);
    void flushLayoutForRead(dom::Document* doc);
    void reloadIframe(dom::Element* el);
    /// An <appview>'s lifecycle: stop tears its sub-document (and compartment)
    /// down at the next drain and keeps it down; start loads it again.
    void stopIframe(dom::Element* el);
    void startIframe(dom::Element* el);
    bool iframeStopped(const dom::Element* el) const;
    bool reloadIframeForDocument(const dom::Document* doc);
    /// Reloads the app at the next frame: the page's own `location.reload()`,
    /// a source change the watcher saw, or the `system_reload_app` action (F5).
    void requestAppReload();
    bool processPendingAppReload();
    std::vector<uint8_t> captureIframe(dom::Element* el, int& outW, int& outH);

    void onFrame(std::function<void(double dtMs)> cb) {
        frameCallbacks_.push_back(std::move(cb));
    }
    /// A per-frame pump that runs on the engine thread every frame — windowed,
    /// server, and headless advanceTime() alike — and, unlike onFrame(), is
    /// NOT gated by bro.time pause: a sibling library's async-job registry
    /// (background loads / inferences delivering their JS callbacks) has to
    /// drain whether or not app time is running.
    void addFramePump(std::function<void()> pump) {
        framePumps_.push_back(std::move(pump));
    }
    /// User activity, for idle detection (bro.seat.setIdleTimeout). Every
    /// input path stamps it — libinput under DRM, SDL in a window, injected
    /// input headless — on activityClockMs(): the wall clock, or the virtual
    /// one in headless so advanceTime() drives idleness deterministically.
    void noteUserActivity() { lastUserActivityMs_ = activityClockMs(); }
    double lastUserActivityMs() const { return lastUserActivityMs_; }
    double activityClockMs() const;
    /// Whether a client of the engine's own Wayland compositor holds an idle
    /// inhibitor on a visible surface (zwp_idle_inhibit_v1: a playing video).
    bool hostIdleInhibited() const;
    /// Runs once at the top of shutdown(), before the runtime, brotensor and
    /// the audio engine go away: where a sibling cancels + joins its
    /// in-flight jobs and drops the JS values they root.
    void addShutdownHook(std::function<void()> hook) {
        shutdownHooks_.push_back(std::move(hook));
    }

    // Optional subsystems are made on first use (engine.cpp): the non-const
    // accessors make one, the const ones and existing*() only report it.
    broaudio::Engine* audioEngine();
    const broaudio::Engine* audioEngine() const { return audioEngine_.get(); }
    broaudio::Engine* existingAudioEngine() const { return audioEngine_.get(); }
    /// audioEngine() as a callable, for a consumer that may never need it.
    AudioEngineSource audioEngineSource();
    AudioInference* audioInference();
    const AudioInference* audioInference() const { return audioInference_.get(); }
    AudioInference* existingAudioInference() const { return audioInference_.get(); }
    steam::SteamService* steamService();
    const steam::SteamService* steamService() const { return steamService_.get(); }
    physics::PhysicsWorld* physicsWorld();
    const physics::PhysicsWorld* physicsWorld() const;
    /// What this process costs and who holds it (engine_memory.cpp, `bro-ctl
    /// mem`): private/shared memory by kind of mapping, threads by name, the C
    /// heap, GPU memory, the JS heap, Skia's caches, the subsystems running.
    std::string memoryReport(bool detail);
    /// Hands the C heap's free pages back to the system; says how much.
    std::string trimMemory();
    /// The same, silently (engine_memory.cpp).
    static void releaseFreeHeapPages();
    /// A held frame (nothing changed on screen) in a window: hands the heap's
    /// free pages back when frames were drawn since the last time, on a
    /// backing-off schedule (engine_idle.cpp).
    void releaseHeapOnHeldFrame();
    /// Caps the C heap's arenas (engine_memory.cpp): glibc otherwise makes
    /// up to eight per core as threads contend, each holding its own free
    /// memory. Four arenas.
    static void configureHeap();
    void dismissSplash() {
        if (splashVisible_) {
            splashVisible_ = false;
            systemDirty_ = true;
        }
    }
    bool splashVisible() const { return splashVisible_; }
    void setSplashVisible(bool val) { splashVisible_ = val; }
    bool appCompiling() const { return appCompiling_; }
    void setAppCompiling(bool val) { appCompiling_ = val; }
    void pumpSplashFrame(double dtMs = 16.67);
    void pumpEventsOnly();
    /// Off-thread page compile (engine_compile_frame.cpp, docs/compile-progress.md):
    /// one live frame, and the `bro-compiling` / `--bro-compile-progress` state on <html>.
    void pumpCompileFrame(double progress);
    void setCompileProgress(bool compiling, double progress);
    const std::vector<SystemDocument>& systemDocs() const { return systemDocs_; }
    std::vector<SystemDocument>& systemDocs() { return systemDocs_; }
    const std::string& systemActivePanel() const { return systemActivePanel_; }
    void toggleSystemPerf();
    void toggleSystemSettings();
    void showSystemPanel(const std::string& name);
    net::NetService* netService();
    const net::NetService* netService() const;

    bool isSystemVisible() const;
    // True for a system panel's document while the panel is not shown. Its
    // requestAnimationFrame callbacks wait until it is, as a hidden page's do
    // on the web: a panel that animates a canvas nobody rasterizes would
    // otherwise record into it forever.
    bool isSystemDocumentHidden(const dom::Document* doc) const;
    Settings* settings() const { return settings_.get(); }
    OverlayManager& overlays() { return *overlayMgr_; }

    void flush();
    /// Draw the 3D scenes a flush left to draw. An advanceTime step's flush
    /// lays the scenes out for its frame and leaves their render pending;
    /// it runs before the clock moves on (the next step), inside a capture
    /// (which renders anyway) or before a read of what a render drew
    /// (gpuFrameMs, the plants' or grass's stats), so advanceTime(50) and a
    /// frameImage() draw the scene once, not four times. A read inside a
    /// step (a frame callback) sees the last frame drawn, as a window's
    /// frame callback does: `forRead` draws nothing then. A flush outside a
    /// step draws at once.
    void renderPendingScenes(bool forRead = false);
    void advanceTime(double ms);
    /// Headless: run `frames` frames of the windowed frame loop itself (the
    /// layout and raster threads, the event pump, the source watcher, the
    /// presenter presenting to an offscreen target, idle holds and waits).
    /// `stepMs` > 0 steps the clock that much per frame with no wall-clock
    /// waits; 0 runs in real time, paced like a 60 Hz FIFO swapchain. The
    /// first call starts the pipeline; it stays up until shutdown, idle
    /// between calls. docs/headless.md, "The windowed pipeline".
    struct PipelineRun {
        int frames = 0;      // frames run
        int presented = 0;   // presented to the offscreen target
        int held = 0;        // held unchanged (not presented)
        double wallMs = 0.0;
    };
    PipelineRun runPipelineFrames(int frames, double stepMs);
    std::string eval(const std::string& code);
    bool hasTestFailure() const { return testFailure_; }
    void setTestFailure(bool f = true) { testFailure_ = f; }
    void clearTestFailure() { testFailure_ = false; }
    bool screenshot(const std::string& path);
    /// What is on screen, as a PNG at `path` (docs/screen-capture.md). Under
    /// DRM: the last frame handed to KMS scanout, read back from the scanout
    /// buffer, so client windows and the shell exactly as composited.
    /// Elsewhere: the engine's own composite (screenshot()). False, with
    /// `why`, when there is nothing to read or the file cannot be written.
    bool captureScreen(const std::string& path, std::string* why = nullptr);
    /// The out-of-process triggers for captureScreen under DRM: SIGUSR2, or a
    /// request file in the runtime dir. Polled by the DRM frame loop.
    void pollScreenCaptureTriggers();
    static void installScreenCaptureSignal();
    static std::string defaultScreenCapturePath();
    static std::string screenCaptureRequestPath();
    bool screenshot(const std::string& path, int x, int y, int w, int h);
    std::vector<uint8_t> capturePixels();
    double gpuFrameMs(bool wait = true);
    /// The GPU milliseconds a frame's refinement (the background work held
    /// for the gap after it: render::VulkanCompute::beginBackground) may
    /// take: what the frame target leaves after the heaviest scene's newest
    /// render and a margin, and never under a seventh of the target, so a
    /// view heavy because it is still coarse refines anyway. -1 without a
    /// timed scene render. Waits for nothing. What the tile caches share.
    double refineBudgetMs();
    /// The last timed scene render's GPU milliseconds pass by pass (the
    /// page's scenes added by name), and its async compute stream's
    /// (`computed`: there was one). The first call turns the passes' stamps
    /// on, so it finds none; false while no render with them has been read.
    bool gpuPasses(std::vector<std::pair<std::string, double>>& passes,
                   std::vector<std::pair<std::string, double>>& compute, bool& computed);
    /// The page's scenes' settle state (SceneRenderer::pendingWork) as of
    /// the newest render (a render a flush left pending is drawn first):
    /// what each still streams or converges for its view; empty: settled.
    std::vector<std::string> scenePendingWork();
    /// Settle state beyond the scenes' own: work a sibling streams for the
    /// view (a tile cache's productions), each call adding a line a kind of
    /// work still outstanding. scenePendingWork adds every source's lines
    /// after the scenes', so perf.settle waits for them too. The id removes
    /// the source.
    using PendingWorkSource = std::function<void(std::vector<std::string>&)>;
    uint64_t addPendingWorkSource(PendingWorkSource source);
    void removePendingWorkSource(uint64_t id);
    /// Let the scenes' exposure adapt over `seconds` with their clock held
    /// (perf.settle: the exposure settles while the scene does not move).
    void advanceSceneExposure(float seconds);

    dom::Element* querySelector(const std::string& selector) const;
    // Builds the panel first if it has not been shown yet.
    dom::Element* overlayQuerySelector(const std::string& panelName,
                                       const std::string& selector);
    std::vector<std::string> overlayPanelNames() const;
    void dispatchClickOn(dom::Element* target);

#if BRO_WITH_3D
    GizmoManager& gizmo() { return *gizmo_; }
    const GizmoManager& gizmo() const { return *gizmo_; }
    scene::CullStats sceneCullStats() const;
#endif

    MenuBar& menuBar() { return menuBar_; }
    const MenuBar& menuBar() const { return menuBar_; }
    void triggerMenuAction(const std::string& id);
    void onMenuChanged();

    DisplayMode displayMode() const { return displayMode_; }
    /// The session's desktop shell over a system compositor (EngineConfig::desktopShell).
    bool isDesktopShell() const { return desktopShell_; }

    /// The bronze module handles of the running app: its compiled `app.dll`
    /// and/or the program its `<script>` tags were compiled into. Both are
    /// bracketed loads (bronze embed.h), so a reload can retire their GC roots
    /// and let the old realm's heap die instead of leaking it per reload.
    const std::vector<uint64_t>& appModuleHandles() const { return appModuleHandles_; }
    void addAppModuleHandle(uint64_t handle) { if (handle) appModuleHandles_.push_back(handle); }
    void unloadAppModules();

    /// The platform window, or nullptr in Server mode (Headless still has one
    /// — a hidden SDL window, which is what keeps the GPU path real).
    platform::Window* window() const { return window_.get(); }

    // --- Transparent, click-through windows (click_through.cpp) ---

    /// Whether the window composites with per-pixel alpha over the desktop
    /// (bro.json `transparent`, where the window system and the swapchain
    /// give it; headless: as declared, for its captures).
    bool windowTransparent() const;
    /// Click-through on or off (bro.window.clickThrough): pointer input
    /// outside inputRegion() passes to whatever lies beneath the window.
    /// False where the window system cannot route input past a window.
    bool setClickThrough(bool on);
    bool clickThrough() const { return clickThrough_; }
    /// Whether the page takes a pointer event at window point (x, y): the
    /// hit test lands on an element other than <html> and <body> (or on the
    /// menu bar, an open dropdown, a system panel). Where it does not, a
    /// click-through window lets the event through.
    bool takesPointerAt(float x, float y);
    /// The window rects where takesPointerAt holds (hit_region.h), from the
    /// current layout.
    std::vector<platform::WindowRect> computeInputRegion();
    /// The region last handed to the window.
    const std::vector<platform::WindowRect>& inputRegion() const { return inputRegion_; }

    /// The app directory this Engine was booted from, absolute. Exposed for
    /// the same reason as window() above: a host that has to act on the app's
    /// own files — loading the compiled module a folder carries
    /// (bronze_host/app_module.h) — is handed the Engine, not the config that
    /// built it.
    const std::string& appDir() const { return appDir_; }
    /// EngineConfig::aotDir / aotBuildDir (empty when not set).
    const std::string& aotDir() const { return aotDir_; }
    const std::string& aotBuildDir() const { return aotBuildDir_; }
    /// The page's script units, in the order the page runs them: every classic
    /// script as ONE unit named for the page (they share one global scope),
    /// then each module script — an external one named for its file, once per
    /// file; an inline one named for the page. Empty when bro-server skips
    /// the page.
    struct PageScriptUnit {
        std::string code;
        std::string name;
        bool moduleFile = false;
    };
    std::vector<PageScriptUnit> pageScriptUnits() const;
    /// The app is the desktop shell: it was granted the `shell` capability,
    /// which only the host executable grants (app_permissions.h).
    bool isShellApp() const { return appPermissions().granted("shell"); }
    /// Whether the app holds `cap` (capabilities.h) now, without asking.
    bool hasCapability(std::string_view cap) const { return appPermissions().granted(cap); }
    const std::function<void(Engine&)>& installHostBindings() const { return installHostBindings_; }
    const std::function<void()>& installWorkerHostBindings() const { return installWorkerHostBindings_; }
    /// Engine-supplied virtual path prefixes (`/app`, `/lib`, `/system`, ...),
    /// so a compile of the app's scripts resolves the same `import "/lib/x.js"`
    /// the asset loader does.
    const util::AssetMounts& assetMounts() const { return assetMounts_; }

    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

    /// How far the app document is scrolled, in CSS px. Client coordinates
    /// (what an event's clientY carries, and what elementFromPoint takes) are
    /// this much above document coordinates, which is what hitTest() wants.
    float viewportScrollY() const { return scrollY_; }
    /// Scroll the app document's root scroller (the viewport) to `y`, clamped
    /// to its scrollable range once layout is current: window.scrollTo and
    /// friends. Fires `scroll` on the document element when the offset moves.
    void scrollViewportTo(float y);

    ContentInsets contentInsets() const;
    int contentTop() const { return contentInsets().top; }
    int contentLeft() const { return contentInsets().left; }
    int contentRight() const { return contentInsets().right; }
    int contentBottom() const { return contentInsets().bottom; }
    int contentWidth() const { auto i = contentInsets(); return viewportWidth_ - i.left - i.right; }
    int contentHeight() const { auto i = contentInsets(); return viewportHeight_ - i.top - i.bottom; }

    InspectorState& inspector() { return inspector_; }
    const InspectorState& inspector() const { return inspector_; }
    void toggleInspector();
    void inspectorSetDock(InspectorDock dock);
    void inspectorSetSize(int sizePx);
    void inspectorSetPickerMode(bool on);
    void inspectorPickElement(dom::Element* el);
    void inspectorSelectById(int id);
    /// The app document's element tree for the inspector panel, as JSON:
    /// `{id, tag, idAttr, classes, hasChildren, children?}` per node, the
    /// whole tree (maxDepth < 0) or that many levels deep. Resets the
    /// per-fetch id map inspectorSelectById resolves against. "null" with no
    /// document. The text form is the seam to the panel's JavaScript: a
    /// tree is not a scalar, and this is the engine's own serializer rather
    /// than one written per binding.
    std::string inspectorAppTreeJson(int maxDepth);
    /// One level of children of a node the last fetch numbered, as a JSON
    /// array; "[]" for an id it does not know or a node no longer in the tree.
    std::string inspectorChildrenJson(int parentId);
    /// The selected node as one JSON node object (its id is -1 when the last
    /// fetch did not number it), or "null" with nothing selected.
    std::string inspectorSelectedJson();

    /// The 500 ms rolling frame statistics the perf HUD shows: frames per
    /// second, mean wall time per frame, and the mean per-phase times. Read
    /// by `__bro.perf`; the engine writes no panel DOM itself.
    double perfFps() const { return frameStats_.statsFps; }
    double perfFrameTimeMs() const { return frameStats_.statsFrameTimeMs; }
    double perfJsMs() const { return frameStats_.phaseJsMs; }
    double perfLayoutMs() const { return frameStats_.phaseLayoutMs; }
    double perfRasterMs() const { return frameStats_.phaseRasterMs; }
    double perfGpuMs() const { return frameStats_.phaseGpuMs; }
    double perfDrawMs() const { return frameStats_.phaseDrawMs; }
    const FrameStats& frameStats() const { return frameStats_; }
    // Lifetime span-cache counters of the document traversal (paint_cache.h).
    layout::PaintCacheStats paintCacheStats() const {
        return drawTraversal_ ? drawTraversal_->paintCacheStats() : layout::PaintCacheStats{};
    }
    // And its scroll layers: scrollers composited as layers, and why the
    // others were painted in place, pass by pass (layout/scroll_layer.h).
    layout::DrawTraversal::ScrollLayerStats scrollLayerStats() const {
        return drawTraversal_ ? drawTraversal_->scrollLayerStats() : layout::DrawTraversal::ScrollLayerStats{};
    }
    // Scroll layer surfaces rastered (frame passes and captures), since startup.
    uint64_t scrollLayerRasters() const { return baseScrollLayers_.rasters() + captureScrollLayers_.rasters(); }
    // The same of the page's own partial raster (replayAppLayers), checked
    // under ScrollLayerSet::setVerifyPartial too.
    uint64_t pagePartialMismatch() const { return pagePartialMismatch_.load(std::memory_order_relaxed); }
    // Passes that drew only the page's damage (and what reaches it), since startup.
    uint64_t pagePartialRasters() const { return pagePartialRasters_.load(std::memory_order_relaxed); }
    // Why a pass of a changed page was whole instead: its pool copy held no
    // recording the damage history reaches from (history), what reaches the
    // damage could not be bounded (cover: a path clip, a layer), or it
    // reached most of the surface (area).
    uint64_t pageWholeHistory() const { return pageWholeHistory_.load(std::memory_order_relaxed); }
    uint64_t pageWholeCover() const { return pageWholeCover_.load(std::memory_order_relaxed); }
    uint64_t pageWholeArea() const { return pageWholeArea_.load(std::memory_order_relaxed); }

    /// The frame flight recorder (frame_trace.h) and the agent control
    /// channel (control.h, docs/agent-control.md). startControl serves the
    /// socket; it is on by default under DRM, and elsewhere with BRO_CONTROL.
    FrameTrace& frameTrace() { return *frameTrace_; }
    ControlServer& control() { return *control_; }
    void startControl();
    /// Monotonic count of trips round the frame loop.
    uint64_t frameNumber() const { return frameNumber_; }

    /// Every secondary window (bro.window.open), live or pending, in
    /// creation order. The perf HUD lists them.
    const std::vector<std::unique_ptr<WindowHost>>& windowHosts() const { return windowHosts_; }

    /// Observe settings changes AFTER the engine has applied them: called
    /// with the category and field of every setUser / reset, "*" standing for
    /// all, on the same synchronous path the engine's own reaction runs.
    /// One observer; a later call replaces the earlier one. bro.settings.
    /// onChange is the JS face of it.
    using SettingsObserver = std::function<void(const std::string& category, const std::string& key)>;
    void setSettingsObserver(SettingsObserver observer) { settingsObserver_ = std::move(observer); }

    double virtualTime() const { return virtualTime_; }

    // bro.time: global pause + timescale
    double timeScale() const { return timeScale_; }
    void setTimeScale(double scale);
    bool timePaused() const { return timePaused_; }
    void setTimePaused(bool paused);
    double timeNowMs() const { return engineNowMs_; }
    WebAnimationManager& webAnimationManager() { return webAnimationManager_; }
    // The one that animates `doc`: an <iframe>'s or a secondary window's
    // own (engine/sub_doc_animations.h), else the app document's.
    WebAnimationManager& webAnimationManagerFor(const dom::Document* doc);
    // The one holding animation `id` (ids are unique across them), or null.
    WebAnimationManager* webAnimationManagerOf(uint64_t id);
    // Each of them: the app document's, then every sub-document's.
    void forEachWebAnimationManager(const std::function<void(WebAnimationManager&)>& fn);
    double effectiveTimeScale() const { return timePaused_ ? 0.0 : timeScale_; }

    void requestServerStop() { serverStopRequested_ = true; }
    double serverTickRate() const { return serverTickRate_; }
    bool hasExplicitServerTickRate() const { return serverTickRateExplicit_; }
    void setServerTickRate(double hz) {
        if (hz <= 0.0) {
            serverTickRateExplicit_ = false;
            serverTickRate_ = 60.0;
        } else {
            serverTickRateExplicit_ = true;
            serverTickRate_ = hz;
        }
    }
    double serverUptime() const;

    layout::SkiaTextMetrics* textMetrics() const { return textMetrics_.get(); }
    float docContentOffsetY() const;

    /// Deepest element at a point in document space, or the document element
    /// when the point lands on no box at all. Public because JS asks the same
    /// question through document.elementFromPoint().
    dom::Element* hitTest(float x, float y);

    /// DRM shell-host routing rules, read from the document's markup and
    /// style (shell_input_rules.cpp): whether the pointer at (x, y) belongs
    /// to the shell rather than the client window under it, and whether the
    /// keyboard does rather than the focused client window.
    bool shellClaimsPointerAt(float x, float y);
    bool shellClaimsKeyboard();

    /// Headless shell-host testing (engine_shell_compositor.cpp): a pointer
    /// event ("move", "down", "up", "wheel") through the DRM input router,
    /// as libinput would deliver it, when the headless engine runs a
    /// compositor (BRO_HEADLESS_COMPOSITOR=1). Returns whether the shell
    /// document received it. Without a compositor it goes to the document.
    bool injectHostPointer(const std::string& type, float x, float y, int button, float wheelDy = 0.0f);
    /// The Wayland socket the shell host's compositor serves ("": none).
    std::string shellCompositorSocket() const;

private:
    #include "engine/engine_private.h"
};

} // namespace bro::engine
