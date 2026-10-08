#pragma once

#include "engine/app_loader.h"
#include "engine/css_transitions.h"
#include "engine/desktop_trust.h"
#include "engine/device_scale.h"
#include "engine/dom_undo.h"
#include "engine/engine_config.h"
#include "engine/engine_fwd.h"
#include "engine/engine_types.h"
#include "engine/frame_presenter.h"
#include "engine/frame_stats.h"
#include "engine/gamepad.h"
#include "engine/iframe.h"
#include "engine/inspector_state.h"
#include "engine/menu_bar.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "engine/scrollbar.h"
#include "engine/settings.h"
#include "engine/system_document.h"
#include "engine/ui_layer.h"
#include "engine/web_animations.h"
#include "engine/window_host.h"
#include "engine/engine_drm.h"
#include "dom/event_target.h"
#include "dom/node_handle.h"
#include "engine/drag_drop.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "render/skia_backend.h"
#include "util/asset_mounts.h"

#if BRO_WITH_3D
#include "engine/gizmo.h"
#endif

#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
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

namespace bro::engine {

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

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

    // Gamepads (gamepad.cpp)
    void handleGamepadAdded(uint32_t instanceId);
    void handleGamepadRemoved(uint32_t instanceId);
    void handleGamepadButton(uint32_t instanceId, int sdlButton, bool down);
    void handleGamepadAxis(uint32_t instanceId, int sdlAxis, float value);
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
    const std::vector<GamepadState>& gamepads() const { return gamepads_; }
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
    webgl::WebGL2RenderingContext* createWebGL2Context(dom::Element* canvas);
    canvas::CanvasScene* createCanvasContext(dom::Element* canvas);
    void flushLayoutForRead(dom::Document* doc);
    void reloadIframe(dom::Element* el);
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

    broaudio::Engine* audioEngine() { return audioEngine_.get(); }
    const broaudio::Engine* audioEngine() const { return audioEngine_.get(); }
    AudioInference* audioInference() { return audioInference_.get(); }
    const AudioInference* audioInference() const { return audioInference_.get(); }
    steam::SteamService* steamService() { return steamService_.get(); }
    const steam::SteamService* steamService() const { return steamService_.get(); }
    physics::PhysicsWorld* physicsWorld();
    const physics::PhysicsWorld* physicsWorld() const;
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
    OverlayManager& overlays() { return overlayMgr_; }

    void flush();
    void advanceTime(double ms);
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
    double gpuFrameMs();

    dom::Element* querySelector(const std::string& selector) const;
    dom::Element* overlayQuerySelector(const std::string& panelName,
                                       const std::string& selector) const;
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

    /// The app directory this Engine was booted from, absolute. Exposed for
    /// the same reason as window() above: a host that has to act on the app's
    /// own files — loading the compiled module a folder carries
    /// (bronze_host/app_module.h) — is handed the Engine, not the config that
    /// built it.
    const std::string& appDir() const { return appDir_; }
    const DesktopTrustInfo& desktopTrust() const { return desktopTrust_; }
    bool isShellApp() const { return desktopTrust_.isShell; }
    bool hasPrivilege(const std::string& ns) const { return desktopTrust_.hasPrivilege(ns); }
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
    double effectiveTimeScale() const { return timePaused_ ? 0.0 : timeScale_; }

    void requestServerStop() { serverStopRequested_ = true; }
    double serverTickRate() const { return serverTickRate_; }
    void setServerTickRate(double hz) { serverTickRate_ = hz; }
    double serverUptime() const;

    void tickTimersOnly();
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
    GraphicsConfig graphicsConfig_;
    InputConfig inputConfig_;

    void updateCursorFromHover(dom::Element* target);
    scene::SceneGraph* findSceneGraphAt(float x, float y,
                                        float& outLocalX, float& outLocalY) const;

    bool gizmoHandleMouseDown(float x, float y, int button);
    bool gizmoHandleMouseMove(float x, float y);
    bool gizmoHandleMouseUp(float x, float y, int button);

    void dispatchEvent(dom::Element* target, dom::Event& event);
    void dispatchPointerAlias(const char* type, dom::Element* target,
                              const dom::MouseEvent& src);
    void pumpVideoEvents();

    // <terminal> (input_terminal*.cpp): keys, text, IME and paste go to the
    // focused one before the page's handling; the mouse after the page's.
    layout::ElTerminal* focusedTerminal(dom::Element** elOut = nullptr);
    bool terminalKeyDown(int keycode, int scancode, int mod, bool repeat);
    bool terminalKeyUp(int keycode, int scancode, int mod, bool repeat);
    bool terminalTextInput(const std::string& text);
    bool terminalTextEditing(const std::string& text);
    bool terminalPaste(const std::string& text);
    void terminalMouseDown(dom::Element* target, float docX, float docY, int button, int mod, int clicks);
    void terminalMouseMove(dom::Element* target, dom::Element* prevHover, float docX, float docY);
    void terminalMouseUp(float docX, float docY, int button);
    bool terminalWheel(dom::Element* target, float docX, float docY, float dy);
    void pumpTerminals();  // once per frame / headless step: also installs ElTerminal::Host
    std::shared_ptr<TerminalLayers> terminalLayers_;  // (terminal_layers.h)
    dom::ElementHandle terminalCapture_;               // the terminal a press is captured by

    float overlayMouseY(float y) const;
    void applyKeyResult(dom::Element* el, const layout::KeyHandleResult& r);
    void dispatchInputEvent(dom::Element* el, const std::string& data = "",
                            const std::string& inputType = "",
                            bool isComposing = false);

    // IME composition helpers (input_handling.cpp)
    bool compositionActive();
    void dispatchCompositionEvent(dom::Element* el, const char* type,
                                  const std::string& data);
    void commitActiveComposition();
    void updateTextInputArea();

    dom::TextNode* editableCompositionTarget();
    bool editableCompositionUpdate(const std::string& text, int cursorCp,
                                   bool& wasComposing, std::string& replacedSel,
                                   dom::Element*& hostOut);
    bool editableCompositionCommit(const std::string& text,
                                   dom::Element*& hostOut, bool cancel = false);
    bool editableCompositionCancel(dom::Element*& hostOut);

    // contenteditable edit primitives
    bool editDeleteAtCaret(bool backward);
    bool editInsertLineBreak();
    bool editInsertTextAtSelection(const std::string& text);
    bool editHistoryStep(bool redo);
    bool editSelectAll();

    void dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget);
    void dispatchScrollEvent(dom::Element* el);

    scene::SceneGraph* sceneGraphForElement(const dom::Element* el) const;
    bool elementAbsoluteOrigin(dom::Element* el, float& outX, float& outY) const;
    bool pickHtmlNodeUnderMouse(dom::Element* canvasEl, float docX, float docY,
                                scene::HtmlNode*& outNode, dom::Element*& outEl,
                                float& outLocalPxX, float& outLocalPxY);
    void dispatchHtmlNodeMouseEvent(const std::string& type,
                                    dom::Element* target,
                                    float localPxX, float localPxY,
                                    int button, int pressedButtons, int mods,
                                    float movX, float movY, bool bubbles,
                                    dom::Element* relatedTarget = nullptr);
    void advanceFocus(bool reverse);
    void addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene);
    // Frame composition (engine_frame_composite.cpp). Each frame:
    // beginGpuFrame() before any GPU work, beginFrameComposite(), then
    // compositeLayers() per layer set, then presentCurrentFrame() (windowed)
    // or readCompositedFrame() (headless capture).
    void beginGpuFrame();
    void beginFrameComposite();
    /// `offsetY`: where the layers' top edge sits in the frame (the app's inset).
    void compositeLayers(const std::vector<UILayer>& layers, int offsetY = 0);
    void presentCurrentFrame();
    std::vector<uint8_t> readCompositedFrame();
    render::PresentFrame describeCompositedFrame();
    // BRO_CAPTURE_PRESENTS=1: windowed presenters also read back each frame
    // they present (presentedPixels).
    static bool capturePresentsRequested();
    FramePresenter::Snapshot buildRasterSnapshot() const;
    void renderAndPresentFrame(double frameStart, double now, double wallFrameDtMs,
                               bool layoutSignaled, bool baseWasDirty);

    void recordAppLayers(render::CommandBuffer& outBuffer,
                         int vpW, int vpH,
                         int insetTop, int insetRight, int insetBottom,
                         float scrollY,
                         const std::unordered_set<dom::Element*>* promotedSet = nullptr,
                         bool promotedOnly = false);

    void replayAppLayers(render::SkiaRenderer* renderer,
                         const render::CommandBuffer& buffer,
                         std::vector<render::SkiaRenderer::LayerSurface>& pool,
                         int& poolW, int& poolH,
                         int surfW, int surfH,
                         std::vector<UILayer>& outLayers,
                         const render::CommandBuffer* promotedBuffer = nullptr);

    void recordSystemPanelLayers(render::CommandBuffer& outBuffer,
                                 int vpW, int vpH);

    void replaySystemPanelLayers(render::SkiaRenderer* renderer,
                                 const render::CommandBuffer& buffer,
                                 std::vector<render::SkiaRenderer::LayerSurface>& pool,
                                 int& poolW, int& poolH,
                                 int vpW, int vpH,
                                 std::vector<UILayer>& outLayers);
    void ensureReplacedElements(dom::Element* elem);

    // App-realm lifecycle (engine_init.cpp + app_reload.cpp)
    void initAppRealm();
    void performAppReload();
    // After a reload whose page failed (index.html unloadable, or a top-level
    // script threw): the error, logged and shown over whatever the page drew.
    void showAppReloadFailure(const std::string& what);
    void resetMenuBarDefaults();
    // Source watching (app_reload.cpp): recursive watchers on the app dir and
    // the project's /lib mount that turn a changed .js/.mjs/.cjs/.html/.htm/
    // .css into a Dev reload once the edits have been quiet for a moment.
    void initDevLoopConfig(const EngineConfig& config);
    void initAppWatcher();
    void pollAppWatcher(double nowMs);

    void initSystemPanels();
    void loadCustomFonts();
    void destroySystemPanels();
    void loadSystemPanels(const std::string& systemDir);
    void scanSystemPanelDir(const std::string& baseDir, const std::string& relPath);
    bool isSystemDocVisible(const SystemDocument& doc) const;
    void tickSystemPanels(double nowMs);
    void layoutSystemPanels(layout::SkiaTextMetrics& metrics);
    void drawSystemPanels(render::Renderer* renderer,
                          layout::DrawTraversal& traversal);
    void drawSystemPanelDoc(render::Renderer* renderer,
                            layout::DrawTraversal& traversal,
                            SystemDocument& doc,
                            int vpW, int vpH);
    void stageSystemPanelCanvases();
    void resizeSystemPanels(int w, int h);
    void renderSplashImmediate();
    dom::Element* systemHitTest(SystemDocument& doc, float x, float y);
    bool systemHandleMouseDown(float x, float y, int button);
    bool systemHandleMouseUp(float x, float y, int button);

    // Iframe sub-documents (iframe.cpp)
    void syncIframes();
    void createIframeDoc(dom::Element* el, const std::string& srcAttr);
    void teardownIframeDoc(IframeDoc* doc);
    void destroyAllIframes();
    bool tickIframes(double nowMs);
    void syncIframeBox(IframeDoc& d);
    void syncAllIframeBoxes();
    IframeDoc* iframeDocById(uint64_t id);
    void processPendingIframeReloads();
    void recordIframeLayers();
    void replayIframeLayers(render::SkiaRenderer* renderer);
    SubDocRef iframeSubDoc(IframeDoc& d);
    SubDocRef windowHostSubDoc(WindowHost& h);
    void quiesceRasterForCapture();
    std::vector<uint8_t> readPublishedFrame(const PublishedFrame& frame, int& outW, int& outH);
    void queueIframeSurfaceFree(render::SkiaRenderer::LayerSurface&& surf);
    void drainIframeSurfaceFrees(render::SkiaRenderer* renderer);

    dom::Element* iframeHitTest(IframeDoc* dp, float localX, float localY);
    bool iframeHandleMouseDown(dom::Element* frameEl, float docX, float docY,
                               int button, float movementX, float movementY, int mod);
    bool iframeHandleMouseUp(dom::Element* frameEl, float docX, float docY,
                             int button, float movementX, float movementY, int mod);
    bool iframeHandleMouseMove(dom::Element* frameEl, float docX, float docY,
                               float movementX, float movementY, int mod);
    bool systemHandleMouseMove(float x, float y);
    bool systemHandleWheel(float x, float y, float dx, float dy);
    void drawElementScrollbars(render::Renderer* renderer,
                               dom::Element* root,
                               float offsetX, float offsetY);
    void updateSelectionSnapshot();
    void drawSelectionHighlight(render::Renderer* renderer, float docOffsetY);
    bool systemHandleKeyDown(int keycode, int scancode, int mod, bool repeat);
    bool systemHandleKeyUp(int keycode, int scancode, int mod, bool repeat);

    void rasterThreadFunc();
    void layoutThreadFunc();
    DisplayMode displayMode_;
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<render::VulkanContext> vulkanContext_;
    std::unique_ptr<render::VulkanSwapchain> vulkanSwapchain_;
    std::unique_ptr<render::VulkanPresenter> vulkanPresenter_;
    std::unique_ptr<DrmPlatformContext> drmCtx_;
    void initDrm(const EngineConfig& config);
    void runDrm();
    // engine_run_drm.cpp: one frame of the DRM loop, in stages.
    void drmFrame();
    void drmDrainLayoutEvents();
    void drmPollPlatform();
    double drmTickWorld(double scaledFrameDtMs);  // returns the wall time it ticked panels at
    bool drmSignalLayout(bool baseWasDirty);
    // engine_drm_input.cpp: input routing between the shell document and
    // client windows.
    void dispatchDrmInput(const platform::DrmInputEvent& ev);
    bool routeDrmKey(const platform::DrmInputEvent& ev);
    bool routeDrmPointer(const platform::DrmInputEvent& ev);
    void deliverDrmInputToShell(const platform::DrmInputEvent& ev);
    bool shellOwnsDrmPointerAt(float x, float y);
    // Whether the pointer at (x, y) belongs to a client surface rather than
    // the shell; `frameWindow` gets the window whose shell-drawn frame is
    // under it (0: none).
    bool drmPointerOnClient(float x, float y, uint64_t* frameWindow);
    void blurShellFocus();
    // engine_shell_compositor.cpp: the compositor a shell host runs (DRM, or
    // headless with BRO_HEADLESS_COMPOSITOR=1), and the frames it keeps on
    // the windows.
    bool startShellCompositor(uint32_t width, uint32_t height, const std::string& socketName, bool xwayland);
    bool pollShellCompositor();
    void syncShellWindowFrames();
    void compositeRemainingClientWindows();
    void releaseClientWindowFrames();
    // Skia's GPU context (null: Skia draws on CPU).
    std::unique_ptr<render::SkiaGpu> skiaGpu_;
    render::SkiaGpu* createSkiaGpu();
    std::unique_ptr<dom::Document> document_;
#if BRO_WITH_A11Y
    std::unique_ptr<a11y::AccessibilityBridge> a11yBridge_;
#endif
    TransitionManager transitionManager_;
    AnimationManager animationManager_;
    WebAnimationManager webAnimationManager_;

    std::unordered_set<dom::Element*> promotedElements_;
    render::CommandBuffer baseCommands_;
    std::unordered_set<dom::Element*> basePromotedSet_;
    bool baseValid_ = false;
    bool appBaseDirty_ = false;
    void markAppBaseDirty() { appBaseDirty_ = true; uiDirty_ = true; ++frameStats_.baseInvalidations; }
    float baseScrollY_ = 0.0f;
    int baseInsetTop_ = -1, baseInsetRight_ = -1, baseInsetBottom_ = -1;

    int lastLayoutContentW_ = -1, lastLayoutContentH_ = -1;

    std::vector<LoadedFont> loadedFonts_;
    std::unique_ptr<render::RecordingRenderer> recordingRenderer_;
    std::unique_ptr<layout::DrawTraversal> drawTraversal_;
    std::unique_ptr<layout::SkiaTextMetrics> textMetrics_;
    std::unique_ptr<platform::EventLoop> eventLoop_;

    SelectionSnapshot selectionSnapshot_;

    bool running_ = false;
    bool shutdownDone_ = false;
    int viewportWidth_;
    int viewportHeight_;
    DeviceScale deviceScale_;
    bool updateDeviceScale();
    void applyMediaResolution();
    std::string resolvedCursor_ = "default";

    AppManifest manifest_;
    std::string appDir_;
    DesktopTrustInfo desktopTrust_;
    std::string titleOverride_;
    std::function<void(Engine&)> installHostBindings_;
    std::function<void()> installWorkerHostBindings_;
    util::AssetMounts assetMounts_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenesDetached_;
    std::unordered_map<uint64_t, canvas::CanvasScene*> canvasSceneRegistry_;
    canvas::CanvasScene* canvasSceneById(uint64_t id) const;

    std::vector<WebGLEntry> webglEntries_;
    void syncWebGLCanvasSizes();
    // Fires the webglcontextlost / webglcontextrestored events a context owes
    // its canvas (WEBGL_lose_context), a task after the call that caused them.
    void pumpWebGLContextEvents();

    std::unique_ptr<FramePresenter> framePresenter_;
    std::unique_ptr<LayoutPipeline>  layoutPipeline_;

    std::atomic<bool> rasterReady_{false};
    std::thread       rasterThread_;
    std::thread       layoutThread_;

    std::vector<render::SkiaRenderer::LayerSurface> htmlSurfacePool_[2];
    int htmlSurfacePoolW_[2] = {0, 0}, htmlSurfacePoolH_[2] = {0, 0};
    std::vector<render::SkiaRenderer::LayerSurface> systemSurfacePool_[2];
    int systemSurfacePoolW_[2] = {0, 0}, systemSurfacePoolH_[2] = {0, 0};

    std::vector<render::SkiaRenderer::LayerSurface> screenshotHtmlPool_;
    int screenshotHtmlPoolW_ = 0, screenshotHtmlPoolH_ = 0;
    std::vector<render::SkiaRenderer::LayerSurface> screenshotSystemPool_;
    int screenshotSystemPoolW_ = 0, screenshotSystemPoolH_ = 0;

    MenuBar menuBar_;
    InspectorState inspector_;
    std::unordered_map<int, dom::Element*> inspectorNodeMap_;
    int inspectorNextId_ = 0;
    SettingsObserver settingsObserver_;
#if BRO_WITH_3D
    std::unique_ptr<GizmoManager> gizmo_;
#endif
    OverlayManager overlayMgr_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<broaudio::Engine> audioEngine_;
    std::unique_ptr<AudioInference> audioInference_;

    std::vector<std::function<void()>> framePumps_;
    std::vector<std::function<void()>> shutdownHooks_;
    std::vector<std::function<void(double)>> frameCallbacks_;
    void fireFrameCallbacks(double dtMs);
#if BRO_WITH_PHYSICS
    std::unique_ptr<physics::PhysicsWorld> physicsWorld_;
#endif
#if BRO_WITH_NET
    std::unique_ptr<net::NetService> netService_;
#endif
    std::unique_ptr<steam::SteamService> steamService_;
#if BRO_WITH_3D
    std::vector<SceneGraphEntry> sceneGraphs_;

    dom::Element* liveElementOf(const SceneGraphEntry& entry) const;
    void pruneDetachedSceneGraphs();
    void clearSceneGraphs();
#endif
    double physicsAccumMs_ = 0.0;
    double lastPhysicsTimeMs_ = 0.0;
    double lastFrameTimeMs_ = 0.0;

    double timeScale_ = 1.0;
    bool   timePaused_ = false;
    double engineNowMs_ = 0.0;
    double lastWallTickMs_ = 0.0;
    std::vector<SystemDocument> systemDocs_;
    std::vector<std::unique_ptr<IframeDoc>> iframeDocs_;
    uint64_t nextIframeId_ = 1;
    std::vector<std::unique_ptr<WindowHost>> windowHosts_;
    uint64_t nextWindowHostId_ = 1;
    uint64_t focusedHostId_ = 0;
    void applyChildManifestDefaults(WindowHost& h, const std::string& appDir);
    void compositeWindowHosts();
    void createWindowHostPresenter(WindowHost& h);
    void createWindowHostDoc(WindowHost& h, struct SubDocSource& source);
    void teardownWindowHostDoc(WindowHost& h);
    void syncWindowHostBox(WindowHost& h);
    bool tickWindowHosts(double nowMs);
    void recordWindowHostLayers();
    void replayWindowHostLayers(render::SkiaRenderer* renderer);
    void destroyAllWindowHosts();

    // Window host input internals (window_host_input.cpp)
    dom::Element* windowHostHitTest(WindowHost& h, float x, float y);
    ControlContext windowHostControlContext(WindowHost& h);
    void windowHostDispatch(WindowHost& h, dom::Element* el, dom::Event& evt);
    void windowHostDispatchInput(WindowHost& h, dom::Element* el,
                                 const std::string& data = "",
                                 const std::string& inputType = "",
                                 bool isComposing = false);
    void windowHostDispatchComposition(WindowHost& h, dom::Element* el,
                                       const char* type, const std::string& data);
    void windowHostDispatchFocus(WindowHost& h, dom::Element* oldEl,
                                 dom::Element* newEl);
    void windowHostApplyKeyResult(WindowHost& h, dom::Element* el,
                                  const layout::KeyHandleResult& r);
    void windowHostUpdateCursor(WindowHost& h, dom::Element* target);
    void windowHostUpdateTextInputArea(WindowHost& h);
    void windowHostAdvanceFocus(WindowHost& h, bool reverse);
    void windowHostCommitComposition(WindowHost& h);
    void windowHostDispatchDrop(WindowHost& h, float x, float y,
                                const std::vector<std::string>* paths, const std::string* text);
    void windowHostRepaint(WindowHost& h);
    void windowHostSetVisibility(WindowHost& h, bool visible);
    bool handleGlobalHotkey(int keycode, int mod, bool repeat);

    bool pendingAppReload_ = false;
    bool watchSources_ = true;
    std::vector<std::unique_ptr<brokit::api::FsWatcher>> appWatchers_;  // app dir, then /lib
    double appWatchLastChangeMs_ = 0.0;
    bool appWatchPending_ = false;
    std::string appWatchDeferReason_;   // why the last watcher reload was held back
    bool appWatchRearm_ = false;        // a watcher died; re-create it once the dir is back
    // A page initAppRealm parses instead of index.html, with no scripts or
    // stylesheets: the error page a failed reload leaves up (app_reload.cpp).
    std::string appHtmlOverride_;
    std::vector<dom::Element*> pendingIframeReloads_;
    std::vector<render::SkiaRenderer::LayerSurface> iframeSurfaceFrees_;
    bool iframeSyncNeeded_ = false;
    std::unordered_map<dom::Element*, std::string> iframeLoadFailed_;
    bool systemPerfVisible_ = false;
    bool systemSettingsVisible_ = false;
    bool splashVisible_ = false;
    bool splashEnabled_ = true;
    bool compiledApp_ = false;
    bool hostProvidesCompiledApp_ = false;
    std::vector<uint64_t> appModuleHandles_;
    bool splashDismissTriggered_ = false;
    double splashStartMs_ = 0.0;
    double splashDismissStartMs_ = 0.0;
    bool appCompiling_ = false;
    double lastSystemRafMs_ = 0.0;
    bool systemDirty_ = true;
    bool systemMouseConsumed_ = false;
    std::string systemActivePanel_;
    dom::ElementHandle systemHoverTarget_;
    SystemDocument* systemHoverDoc_ = nullptr;

    double virtualTime_ = 0.0;
    double lastUserActivityMs_ = -1.0;   // activityClockMs() of the last input; -1: none yet
    double audioFrameCarry_ = 0.0;

    double serverTickRate_ = 60.0;
    double serverStartTime_ = 0.0;
    bool serverStopRequested_ = false;

    FrameStats frameStats_;
    bool uiDirty_ = true;
    bool hasRenderedOnce_ = false;
    bool mediaEventsArmed_ = false;
    // Set inside a headless advanceTime step once media is at its instant:
    // pumpVideoEvents then moves no media clock or picture (advanceTime).
    bool mediaHeldForStep_ = false;

    dom::ElementHandle hoveredElement_;
    scene::HtmlNode*   hoveredHtmlNode_ = nullptr;
    dom::ElementHandle hoveredHtmlElement_;
    scene::HtmlNode*   htmlNodeMouseDownNode_ = nullptr;
    dom::ElementHandle htmlNodeMouseDownElement_;

    // HTML5 drag and drop between elements (see engine/drag_drop.h). Distinct
    // from the OS file drop, which arrives from outside the window.
    DragDrop dragDrop_;

    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;
    bool cursorVisible_ = false;

    dom::ElementHandle lockedElement_;
    float lockedMouseX_ = 0.0f;
    float lockedMouseY_ = 0.0f;

    std::unordered_map<int, dom::ElementHandle> pointerCaptures_;
    dom::Element* pointerCaptureFor(int pointerId) const;

    std::vector<TouchContact> touchContacts_;
    int nextTouchPointerId_ = 2;
    TouchContact* touchByFinger(uint64_t fingerId);
    bool dispatchTouchPointerEvent(const char* type, const TouchContact& c,
                                   bool cancelable);
    bool dispatchTouchEvent(const char* type, const TouchContact& changed,
                            bool cancelable);
    void dispatchCompatMouseForTap(const TouchContact& c);

    GestureState gesture_;
    void gestureMaybeStart();
    void gestureUpdate(uint64_t movedFinger);
    void gestureEndIfFounder(uint64_t endedFinger);
    void dispatchGestureEvent(const char* type);

    std::string documentReadyState_ = "loading";
    MouseDispatchState appMouseState_;
    int pressedButtons_ = 0;

    std::vector<GamepadState> gamepads_;
    GamepadState* gamepadByInstance(uint32_t instanceId);
    GamepadState* connectedGamepadAt(int index);
    GamepadState& allocateGamepadSlot();
    void gamepadButtonChanged(GamepadState& gp, int w3cIndex, float value);
    void gamepadAxisChanged(GamepadState& gp, int w3cAxis, float value);

    void dispatchActionEventForKey(const std::string& key, const char* phase,
                                   float strength, int gamepadIndex = -1);
    void evaluateAxisActions(GamepadState& gp, int w3cAxis);
    void dispatchMouseButtonAction(int domButton, bool down);
    int actionMouseDownMask_ = 0;
    std::unordered_map<int, std::string> heldKeys_;
    void dispatchGamepadConnectionEvent(const GamepadState& gp, bool connected);
    void closeAllGamepads();

    int heldModifierMask_ = 0;
    int currentModState() const;

    bool selectionDragging_ = false;
    dom::TextNodeHandle selectionAnchorNode_;
    int selectionAnchorOffset_ = 0;
    float selectionPressX_ = 0.0f;
    float selectionPressY_ = 0.0f;
    bool  selectionPastThreshold_ = false;

    dom::ElementHandle controlDragElement_;
    bool controlDragIsPanel_ = false;

    EditableComposition editComp_;
    DomUndoHistories editUndo_;

    // The viewport scroll. Written only through setViewportScrollY, which
    // mirrors it into the document (Document::setViewportScroll) for the
    // element geometry of fixed boxes.
    float scrollY_ = 0.0f;
    void setViewportScrollY(float y);
    // How far the root scroller can scroll: the document's scrollable
    // overflow height (<html>'s margin box and every box that overflows it,
    // less what an overflow clip hides and the fixed boxes, which sit in the
    // viewport rather than the page). Refreshed by updateDocumentHeight after
    // each layout of the app document.
    float documentHeight_ = 0.0f;
    void updateDocumentHeight();
    float wheelResidualY_ = 0.0f;

    TopLayerCloseRequestFn topLayerCloseRequest_;

    Scrollbar viewportScrollbar_;
    Scrollbar elementScrollbar_;
    bool draggingViewportScrollbar_ = false;
    dom::ElementHandle scrollbarDragTarget_;
    dom::ElementHandle scrollbarHoveredElement_;
    SystemDocument* scrollbarDragSystemDoc_ = nullptr;

    double uiFrameIntervalMs_ = 8.0, lastUIRenderMs_ = 0.0, frameCapIntervalMs_ = 0.0;
    bool windowFocused_ = true, pageVisible_ = true;
    static constexpr double kUnfocusedFps = 30.0;
    static constexpr double kGCIntervalMs = 1000.0;
    double lastGCMs_ = 0.0, lastGpuFrameMs_ = -1.0;
    bool testFailure_ = false;

    // The frame's composite, bottom to top: CPU layers composite into
    // frameSegments_[0] until a GPU layer; its image joins frameImages_ and
    // the layers after it go into the next segment, blended over it, and so
    // on. frameSkiaImages_ keeps those images alive until the submit.
    std::vector<sk_sp<SkSurface>> frameSegments_;
    std::vector<bool> frameSegmentUsed_;
    std::vector<render::PresentImage> frameImages_;
    std::vector<render::SkiaImageRef> frameSkiaImages_;
    int frameCompositeW_ = 0, frameCompositeH_ = 0;
    SkCanvas* frameSegmentCanvas();  // the segment being composited into
};

} // namespace bro::engine
