#pragma once

#include "engine/app_loader.h"
#include "engine/css_transitions.h"
#include "engine/dom_undo.h"
#include "engine/engine_config.h"
#include "engine/engine_types.h"
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
#include "dom/event_target.h"
#include "dom/node_handle.h"
#include "js/message_queue.h"
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
    class Renderer;
}
namespace bro::webgl { class WebGL2RenderingContext; }
namespace broaudio { class Engine; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::net { class NetService; }
namespace bro::steam { class SteamService; }
namespace bro::scene { class SceneGraph; class HtmlNode; struct CullStats; }
namespace bro::canvas { class CanvasScene; class CanvasRasterThread; }
namespace bro::platform { class Window; class EventLoop; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; class TextNode; }
namespace bro::layout { class DrawTraversal; class SkiaTextMetrics; }

namespace bro::engine {

class FramePresenter;
class LayoutPipeline;
class AudioInference;
struct SubDocRef;

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

    void stopBackgroundServices();
    void shutdown();

private:
    void removeModalEventWatch();

public:
    using ContentInsets = bro::engine::ContentInsets;
    using LoadedFont = bro::engine::LoadedFont;
    using SelectionSnapshot = bro::engine::SelectionSnapshot;
    using WebGLEntry = bro::engine::WebGLEntry;
#if BRO_WITH_3D
    using SceneGraphEntry = bro::engine::SceneGraphEntry;
#endif
    using TouchContact = bro::engine::TouchContact;
    using GestureState = bro::engine::GestureState;
    using EditableComposition = bro::engine::EditableComposition;
    using IframeDoc = bro::engine::IframeDoc;
    using SystemDocument = bro::engine::SystemDocument;
    using WindowHostOptions = bro::engine::WindowHostOptions;
    using WindowHost = bro::engine::WindowHost;

    void run();
    void handleResize(int w, int h);
    void handleDisplayScaleChanged();
    float displayScale() const { return displayScale_; }

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
    void handleDropFile(const std::string& path, float x = -1, float y = -1) {
        handleDropFile(std::vector<std::string>{ path }, x, y);
    }
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
    bool anyLiveWindowHosts() const;
    bool anyPresentableWindowHosts() const;
    bool anyWindowHostFocused() const;

    void handleWindowCloseRequested(uint32_t sdlWindowId);
    void handleHostResized(uint32_t sdlWindowId, int w, int h);
    void handleHostFocusChanged(uint32_t sdlWindowId, bool focused);
    void handleHostMinimized(uint32_t sdlWindowId, bool minimized);
    void handleHostOccluded(uint32_t sdlWindowId, bool occluded);
    void processPendingWindowHosts();

    // Messaging between window host and app realm
    bool postMessageToWindowHost(uint64_t id, std::unique_ptr<js::Message> msg);
    void postMessageToParent(uint64_t hostId, std::unique_ptr<js::Message> msg);
    void drainWindowHostMessages();
    uint64_t windowHostIdForContext(JSContext* ctx) const;

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

    const std::vector<GamepadState>& gamepads() const { return gamepads_; }

    // Polled action state (action_input.cpp)
    float actionStrength(const std::string& action) const;
    bool actionPressed(const std::string& action) const;
    float getActionStrength(const std::string& action) const { return actionStrength(action); }
    bool isActionPressed(const std::string& action) const { return actionPressed(action); }

    void handleProgrammaticFocus(dom::Document* doc, dom::Element* oldEl,
                                 dom::Element* newEl);

    // Clipboard simulation (for headless testing — bypasses system clipboard)
    void simulatePaste(const std::string& text);
    std::string simulateCopy();
    std::string simulateCut();

    bool execCommand(const std::string& name, bool showUI,
                     const std::string& value);
    bool queryCommandSupported(const std::string& name) const;
    bool queryCommandEnabled(const std::string& name);

    float getLastMouseX() const { return lastMouseX_; }
    float getLastMouseY() const { return lastMouseY_; }
    const std::string& resolvedCursor() const { return resolvedCursor_; }

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

    void setPageVisibility(bool visible);
    void setFullscreenState(bool fullscreen);

    // Headless & DOM API
    dom::Document* document() const { return document_.get(); }
    render::Renderer* renderer() const { return renderer_.get(); }

    dom::ListenerHandle addWindowEventListener(const std::string& type,
                                               dom::EventCallback cb,
                                               dom::ListenerOptions opts = {});
    bool removeWindowEventListener(dom::ListenerHandle handle);
    void dispatchElementEvent(dom::Element* target, dom::Event& event);
    void dispatchWindowEvent(dom::Event& event);

    scene::SceneGraph* createSceneContext(dom::Element* canvas);
    size_t sceneContextCount() const;
    webgl::WebGL2RenderingContext* createWebGL2Context(dom::Element* canvas);
    void flushLayoutForRead(dom::Document* doc);
    void reloadIframe(dom::Element* el);
    void requestAppReload();
    bool processPendingAppReload();
    std::vector<uint8_t> captureIframe(dom::Element* el, int& outW, int& outH);

    js::Runtime* jsRuntime() const { return jsRuntime_.get(); }
    js::Timers* timers() const { return timers_.get(); }
    void onFrame(std::function<void(double dtMs)> cb) {
        frameCallbacks_.push_back(std::move(cb));
    }

    broaudio::Engine* audioEngine() { return audioEngine_.get(); }
    const broaudio::Engine* audioEngine() const { return audioEngine_.get(); }
    AudioInference* audioInference() { return audioInference_.get(); }
    const AudioInference* audioInference() const { return audioInference_.get(); }
    steam::SteamService* steamService() { return steamService_.get(); }
    const steam::SteamService* steamService() const { return steamService_.get(); }
    physics::PhysicsWorld* physicsWorld() {
#if BRO_WITH_PHYSICS
        return physicsWorld_.get();
#else
        return nullptr;
#endif
    }
    const physics::PhysicsWorld* physicsWorld() const {
#if BRO_WITH_PHYSICS
        return physicsWorld_.get();
#else
        return nullptr;
#endif
    }
    net::NetService* netService() {
#if BRO_WITH_NET
        return netService_.get();
#else
        return nullptr;
#endif
    }
    const net::NetService* netService() const {
#if BRO_WITH_NET
        return netService_.get();
#else
        return nullptr;
#endif
    }

    bool isSystemVisible() const;
    Settings* settings() const { return settings_.get(); }
    OverlayManager& overlays() { return overlayMgr_; }

    void flush();
    void advanceTime(double ms);
    std::string eval(const std::string& code);
    bool screenshot(const std::string& path);
    bool screenshot(const std::string& path, int x, int y, int w, int h);
    std::vector<uint8_t> capturePixels();
    double gpuFrameMs();

private:
    std::vector<uint8_t> renderUnifiedToPixels();

public:
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
    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

    /// How far the app document is scrolled, in CSS px. Client coordinates
    /// (what an event's clientY carries, and what elementFromPoint takes) are
    /// this much above document coordinates, which is what hitTest() wants.
    float viewportScrollY() const { return scrollY_; }

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

    InspectorState& inspector() { return inspector_; }
    const InspectorState& inspector() const { return inspector_; }
    void toggleInspector();
    void inspectorSetDock(InspectorDock dock);
    void inspectorSetSize(int sizePx);
    void inspectorSetPickerMode(bool on);
    void inspectorPickElement(dom::Element* el);
    void inspectorSelectById(int id);
    JSValue inspectorBuildTreeJS(JSContext* ctx, int maxDepth);
    JSValue inspectorChildrenJS(JSContext* ctx, int parentId);
    JSValue inspectorSelectedJS(JSContext* ctx);

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
    void drawTexturedQuad(GLuint tex, float x, float y, float w, float h);
    void compositeLayers(const std::vector<UILayer>& layers, GLuint targetFBO = 0,
                         int offsetY = 0, int layerW = -1, int layerH = -1);

    void recordAppLayers(render::CommandBuffer& outBuffer,
                         int vpW, int vpH,
                         int insetTop, int insetRight, int insetBottom,
                         float scrollY,
                         const std::unordered_set<dom::Element*>* promotedSet = nullptr,
                         bool promotedOnly = false);

    void replayAppLayers(render::SkiaRenderer* renderer,
                         const render::CommandBuffer& buffer,
                         std::vector<render::SkiaRenderer::GPUSurface>& pool,
                         int& poolW, int& poolH,
                         int surfW, int surfH,
                         std::vector<UILayer>& outLayers,
                         const render::CommandBuffer* promotedBuffer = nullptr);

    void recordSystemPanelLayers(render::CommandBuffer& outBuffer,
                                 int vpW, int vpH);

    void replaySystemPanelLayers(render::SkiaRenderer* renderer,
                                 const render::CommandBuffer& buffer,
                                 std::vector<render::SkiaRenderer::GPUSurface>& pool,
                                 int& poolW, int& poolH,
                                 int vpW, int vpH,
                                 std::vector<UILayer>& outLayers);
    void ensureReplacedElements(dom::Element* elem);

    // App-realm lifecycle (engine_init.cpp + app_reload.cpp)
    void installCoreBindings(JSContext* ctx);
    void initAppRealm();
    void performAppReload();
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
    void drawSystemPanelDoc(render::Renderer* renderer,
                            layout::DrawTraversal& traversal,
                            SystemDocument& doc,
                            int vpW, int vpH);
    void stageSystemPanelCanvases();
    void resizeSystemPanels(int w, int h);
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
    IframeDoc* iframeDocForElement(const dom::Element* el);
    void processPendingIframeReloads();
    void recordIframeLayers();
    void replayIframeLayers(render::SkiaRenderer* renderer);
    SubDocRef iframeSubDoc(IframeDoc& d);
    SubDocRef windowHostSubDoc(WindowHost& h);
    void quiesceRasterForCapture();
    std::vector<uint8_t> readbackSubDocTexture(unsigned int tex, int w, int h,
                                               int& outW, int& outH);
    void queueIframeSurfaceFree(render::SkiaRenderer::GPUSurface&& surf);
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
    std::unique_ptr<render::GLContext> gl_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<js::Runtime> jsRuntime_;
    std::unique_ptr<js::Timers> timers_;
    std::unique_ptr<dom::Document> document_;
    TransitionManager transitionManager_;
    AnimationManager animationManager_;
    WebAnimationManager webAnimationManager_;

    std::unordered_set<dom::Element*> promotedElements_;
    render::CommandBuffer baseCommands_;
    std::unordered_set<dom::Element*> basePromotedSet_;
    bool baseValid_ = false;
    bool appBaseDirty_ = false;
    void markAppBaseDirty() { appBaseDirty_ = true; uiDirty_ = true; }
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
    float displayScale_ = 1.0f;
    std::string resolvedCursor_ = "default";

    JSValue observerCheckFn_ = JS_UNDEFINED;
    AppManifest manifest_;
    std::string appDir_;
    std::function<void(JSContext*)> installHostBindings_;
    std::string titleOverride_;
    util::AssetMounts assetMounts_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenesDetached_;
    std::unordered_map<uint64_t, canvas::CanvasScene*> canvasSceneRegistry_;
    canvas::CanvasScene* canvasSceneById(uint64_t id) const {
        if (!id) return nullptr;
        auto it = canvasSceneRegistry_.find(id);
        return it == canvasSceneRegistry_.end() ? nullptr : it->second;
    }
    std::unique_ptr<canvas::CanvasRasterThread> canvasRasterThread_;

    std::vector<WebGLEntry> webglEntries_;
    void syncWebGLCanvasSizes();

    std::unique_ptr<FramePresenter> framePresenter_;
    std::unique_ptr<LayoutPipeline>  layoutPipeline_;

    std::atomic<bool> rasterReady_{false};
    std::thread       rasterThread_;
    std::thread       layoutThread_;
    SDL_GLContext     rasterGLContext_ = nullptr;

    std::vector<render::SkiaRenderer::GPUSurface> htmlSurfacePool_[2];
    int htmlSurfacePoolW_[2] = {0, 0}, htmlSurfacePoolH_[2] = {0, 0};
    std::vector<render::SkiaRenderer::GPUSurface> systemSurfacePool_[2];
    int systemSurfacePoolW_[2] = {0, 0}, systemSurfacePoolH_[2] = {0, 0};

    std::vector<render::SkiaRenderer::GPUSurface> screenshotHtmlPool_;
    int screenshotHtmlPoolW_ = 0, screenshotHtmlPoolH_ = 0;
    std::vector<render::SkiaRenderer::GPUSurface> screenshotSystemPool_;
    int screenshotSystemPoolW_ = 0, screenshotSystemPoolH_ = 0;

    MenuBar menuBar_;
    InspectorState inspector_;
    std::unordered_map<int, dom::Element*> inspectorNodeMap_;
    int inspectorNextId_ = 0;
#if BRO_WITH_3D
    std::unique_ptr<GizmoManager> gizmo_;
#endif
    OverlayManager overlayMgr_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<broaudio::Engine> audioEngine_;
    std::unique_ptr<AudioInference> audioInference_;

    std::vector<std::function<void()>> framePumps_;
    std::vector<std::function<void(double)>> frameCallbacks_;
    void fireFrameCallbacks(double dtMs) {
        for (auto& cb : frameCallbacks_) cb(dtMs);
    }
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
    std::vector<std::pair<uint64_t, std::unique_ptr<js::Message>>> hostToParentMessages_;
    void applyChildManifestDefaults(WindowHost& h, const std::string& appDir);
    void compositeWindowHosts();
    void createWindowHostDoc(WindowHost& h, struct SubDocSource& source);
    void teardownWindowHostDoc(WindowHost& h);
    void syncWindowHostBox(WindowHost& h);
    bool tickWindowHosts(double nowMs);
    void recordWindowHostLayers();
    void replayWindowHostLayers(render::SkiaRenderer* renderer);
    void destroyAllWindowHosts(bool notifyJs);

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
    std::vector<dom::Element*> pendingIframeReloads_;
    std::vector<render::SkiaRenderer::GPUSurface> iframeSurfaceFrees_;
    bool iframeSyncNeeded_ = false;
    std::unordered_map<dom::Element*, std::string> iframeLoadFailed_;
    bool systemPerfVisible_ = false;
    bool systemSettingsVisible_ = false;
    bool splashVisible_ = false;
    bool splashEnabled_ = true;
    bool compiledApp_ = false;
    bool hostProvidesCompiledApp_ = false;
    bool splashDismissTriggered_ = false;
    double splashStartMs_ = 0.0;
    double lastSystemRafMs_ = 0.0;
    bool systemDirty_ = true;
    bool systemMouseConsumed_ = false;
    std::string systemActivePanel_;
    dom::ElementHandle systemHoverTarget_;
    SystemDocument* systemHoverDoc_ = nullptr;

    double virtualTime_ = 0.0;

    double serverTickRate_ = 60.0;
    double serverStartTime_ = 0.0;
    bool serverStopRequested_ = false;

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

    dom::ElementHandle hoveredElement_;
    scene::HtmlNode*   hoveredHtmlNode_ = nullptr;
    dom::ElementHandle hoveredHtmlElement_;
    scene::HtmlNode*   htmlNodeMouseDownNode_ = nullptr;
    dom::ElementHandle htmlNodeMouseDownElement_;

    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;

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

    float scrollY_ = 0.0f;
    float documentHeight_ = 0.0f;
    float wheelResidualY_ = 0.0f;

    Scrollbar viewportScrollbar_;
    Scrollbar elementScrollbar_;
    bool draggingViewportScrollbar_ = false;
    dom::ElementHandle scrollbarDragTarget_;
    dom::ElementHandle scrollbarHoveredElement_;
    SystemDocument* scrollbarDragSystemDoc_ = nullptr;

    double uiFrameIntervalMs_ = 8.0;
    double lastUIRenderMs_ = 0.0;
    double frameCapIntervalMs_ = 0.0;
    bool windowFocused_ = true;
    static constexpr double kUnfocusedFps = 30.0;

    static constexpr double kGCIntervalMs = 1000.0;
    double lastGCMs_ = 0.0;

    unsigned int gpuTimerQuery_ = 0;
    bool gpuTimerPending_ = false;
    double lastGpuFrameMs_ = -1.0;

    double phaseJsMs_ = 0.0;
    double phaseLayoutMs_ = 0.0;
    double phaseRasterMs_ = 0.0;
    double phaseGpuMs_ = 0.0;
    double phaseGlStateMs_ = 0.0;
    double phaseDrawMs_ = 0.0;
    double phaseUploadMs_ = 0.0;
    double accumJsMs_ = 0.0;
    double accumLayoutMs_ = 0.0;
    double accumRasterMs_ = 0.0;
    double accumGpuMs_ = 0.0;
    double accumGlStateMs_ = 0.0;
    double accumDrawMs_ = 0.0;
    double accumUploadMs_ = 0.0;

    unsigned int uiQuadVAO_ = 0;
    unsigned int uiQuadVBO_ = 0;
};

Engine* engineForContext(JSContext* ctx);

} // namespace bro::engine
