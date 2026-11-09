// Private members and helpers of bro::engine::Engine (included inside class Engine in engine.h).
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
    // Windowed, before the frame is recorded: when this frame sent a
    // terminal's program input, wait up to kTerminalEchoWaitMs for the echo
    // and take it, so a typed key shows in the frame that sent it.
    void awaitTerminalEcho(double frameStart);
    std::shared_ptr<TerminalLayers> terminalLayers_;  // (terminal_layers.h)
    dom::ElementHandle terminalCapture_;               // the terminal a press is captured by

    // <remoteview> (input_remote.cpp): a captured view takes every key before
    // anything else; the pointer over a view goes to it as well as the page.
    layout::ElRemoteView* capturedRemoteView();
    bool remoteKey(int scancode, int mod, bool pressed, bool repeat);
    void remotePointer(float x, float y, float xrel, float yrel, int button, bool pressed);
    bool remoteWheel(float x, float y, float dx, float dy);
    void pumpRemoteViews();  // once per frame: capture, keyboard grab, relative mode
    RemoteViewHost* remoteViewHost_ = nullptr;
    uint64_t remotePointerGrab_ = 0;   // the view a held button went to
    uint32_t remotePointerButtons_ = 0;
    uint64_t remoteKeyboardView_ = 0;  // the captured view (keyboard grab on)
    uint64_t remoteLockedView_ = 0;    // the view holding relative mode
    std::vector<uint64_t> frameRemoteViews_;  // views composited this frame

    float overlayMouseY(float y) const;
    void applyKeyResult(dom::Element* el, const layout::KeyHandleResult& r);

    // <datalist> suggestions (input_datalist.cpp). updateDatalist: after the
    // user edits a field with a list (or presses Down / Alt+Down on it,
    // explicitOpen), show its matching options, refresh the list already
    // open, or close it when nothing matches. syncDatalist: once a frame,
    // close the list of a field that lost the focus or left the document.
    void updateDatalist(dom::Element* input, bool explicitOpen);
    void syncDatalist();
    bool datalistPicking_ = false;  // a pick's own `input` must not reopen the list
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
    /// `mayHold`: an unchanged frame is not presented (holdUnchangedFrame).
    /// False when nothing was presented.
    bool presentCurrentFrame(bool mayHold = false);
    std::vector<uint8_t> readCompositedFrame();
    /// Frames composite over nothing (a transparent window, or headless
    /// declared so), not over opaque black.
    bool transparentFrames() const {
        return graphicsConfig_.transparent &&
               (displayMode_ == DisplayMode::Windowed || displayMode_ == DisplayMode::Headless);
    }
    static void unpremultiplyRgba(std::vector<uint8_t>& rgba);
    /// Recompute the input region and hand it to the window when it changed
    /// (a click-through window, after a frame that laid out or painted).
    void updateInputRegion();
    bool clickThrough_ = false;
    std::vector<platform::WindowRect> inputRegion_;
    bool inputRegionStale_ = false;    // a change not yet in the region (its budget)
    double inputRegionAtMs_ = 0.0;     // when, and how long, the last one took
    double inputRegionCostMs_ = 0.0;
    render::PresentFrame describeCompositedFrame();
    // BRO_CAPTURE_PRESENTS=1: windowed presenters read back each frame they
    // present from the start (agent control turns that on only while it
    // needs it: a screenshot's one present, a recording's length).
    static bool capturePresentsRequested();
    FramePresenter::Snapshot buildRasterSnapshot() const;
    void renderAndPresentFrame(double frameStart, double now, double wallFrameDtMs,
                               bool layoutSignaled, bool baseWasDirty);

    void recordAppLayers(render::CommandBuffer& outBuffer,
                         int vpW, int vpH,
                         int insetTop, int insetRight, int insetBottom,
                         float scrollY,
                         const std::unordered_set<dom::Element*>* promotedSet = nullptr,
                         bool promotedOnly = false,
                         ScrollLayerSet* scrollLayers = nullptr);

    // A pass's scroll layers (engine/scroll_layers.h): the frame passes'
    // (pass 0) and the headless captures' (pass 1).
    const ScrollLayerSet* scrollLayerSet(uint32_t pass) const {
        return pass == 0 ? &baseScrollLayers_ : &captureScrollLayers_;
    }

    // What a pool copy's HTML surfaces hold of the base: the recording it was
    // replayed from (baseGeneration_), at what scale, and the layers that
    // replay produced. A raster pass whose base is unchanged keeps them.
    struct HtmlPoolState {
        uint64_t baseGen = 0;
        float scale = 0.0f;
        int htmlLayerIdx = 0;
        std::vector<UILayer> layers;
        // A partial pass's scratch surfaces, one per HTML surface it redraws.
        std::vector<render::SkiaRenderer::LayerSurface> scratch;
    };

    // The damage a new base recording brings over the last (render/damage.h);
    // everywhere with the paint cache off, so such a pass is a full raster.
    render::Damage recordDamage(const render::CommandBuffer& older, const render::CommandBuffer& newer) const;

    // `held` (with `baseGen` and `scale`): skip the base replay when the pool
    // already holds this recording, redraw only the damage since the one it
    // holds when `history` has it, and note what it holds after.
    void replayAppLayers(render::SkiaRenderer* renderer,
                         const render::CommandBuffer& buffer,
                         std::vector<render::SkiaRenderer::LayerSurface>& pool,
                         int& poolW, int& poolH,
                         int surfW, int surfH,
                         std::vector<UILayer>& outLayers,
                         const render::CommandBuffer* promotedBuffer = nullptr,
                         HtmlPoolState* held = nullptr, uint64_t baseGen = 0, float scale = 0.0f,
                         const render::DamageHistory* history = nullptr);

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
    std::string combinedClassicScripts() const;
    // At launch, while the GPU comes up: the realm's host globals installed
    // and the page's first script compiling on its own thread, for
    // initAppRealm's run of it to take.
    void beginPageCompile();
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
    void loadSystemPanel(SystemDocument& doc);
    void loadVisibleSystemPanels();
    bool isSystemDocVisible(const SystemDocument& doc) const;
    void tickSystemPanels(double nowMs);
    /// The scene's shaders and pipelines still in progress (shader_status.cpp).
    bool backgroundWorkPending() const;
    /// Writes the work in progress into the splash and the status panel, and
    /// shows / hides the status panel.
    void tickWorkStatus(double nowMs);
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
    // The hover transitions (boundary events, :hover) to `target`, shared by a
    // move and recheckHover: the app document's, an iframe's (the element
    // under the pointer in it, returned), a window's, the system panels'.
    void setAppHover(dom::Element* target, float x, float y, float xrel, float yrel);
    dom::Element* iframeSetHover(dom::Element* frameEl, float docX, float docY);
    void hostSetHover(WindowHost& h, dom::Element* target, float x, float y, float xrel, float yrel);
    dom::Element* systemHoverHit(float x, float y, SystemDocument*& doc);
    void systemSetHover(dom::Element* target, SystemDocument* doc, float x, float y);
public:
    // A still pointer hit tested again after layout, a DOM change or a
    // scroll (input_mouse_move.cpp): due when one happened since the last.
    bool hoverRecheckDue() const;
    void recheckHover();
    void noteHoverRecheck() { hoverRecheckPending_ = true; }
private:
    bool systemHandleWheel(float x, float y, float dx, float dy);
    // Run `draw` (the traversal's walk of `root`) with every scroll
    // container's bars under `root` painted where the stacking walk puts its
    // scroller (DrawTraversal::setScrollbarPainter), so a box painted over a
    // scroller (a popup, a menu) is over its bars too; the ones the walk does
    // not reach are painted after it. Thumbs on whole device pixels at `scale`
    // (0: as laid out). With `layers`, the thumb of a scroller composited as
    // a scroll layer is a layer break `recorder` records over its track
    // (ScrollLayerSet::setThumb); only its track is painted.
    void drawWithElementScrollbars(layout::DrawTraversal& traversal, render::Renderer* renderer,
                                   dom::Element* root, float offsetX, float offsetY, float scale,
                                   ScrollLayerSet* layers, render::RecordingRenderer* recorder,
                                   const std::function<void()>& draw);
    void updateSelectionSnapshot();
    void drawSelectionHighlight(render::Renderer* renderer, float docOffsetY);
    bool systemHandleKeyDown(int keycode, int scancode, int mod, bool repeat);
    bool systemHandleKeyUp(int keycode, int scancode, int mod, bool repeat);

    void rasterThreadFunc();
    void layoutThreadFunc();
    DisplayMode displayMode_;
    bool desktopShell_ = false;
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
    void drmPaceNextFrame(double frameStart);
    double drmPresentTargetMs(double frameStart) const;
    double drmWorkPeakMs_ = 0.0;  // a decaying peak of the frame's work before its commit
    // Input and agent commands handled while the loop waits between frames;
    // true when they changed the shell document (the next frame starts now).
    bool drmDispatchBetweenFrames();
    // A KMS flip landed (the presenter's flip listener): the frame it showed
    // gets its vblank, and the client frames it sampled go back.
    void drmFlipLanded(double vblankMs, uint32_t sequence);
    uint64_t drmFlipFrame_ = 0;  // the frame whose flip was committed last
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
    void compositeDragIcon();  // engine_drm_cursor.cpp
    // The page's drag carried to the clients (engine_shell_compositor.cpp):
    // offered as the host's wl_data_source, then followed by the pointer
    // (routeShellDragPointer) until the compositor says it ended.
    bool startShellDrag(const platform::DragSource& drag);
    bool routeShellDragPointer(const platform::DrmInputEvent& ev);  // engine_drm_input.cpp
    void finishShellDrags();
    // A client's own cursor picture (wl_pointer.set_cursor), drawn into the
    // frame where the cursor plane cannot take it (engine_drm_cursor.cpp).
    bool drawClientCursorIntoFrame();
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
    // Bumped by every write of baseCommands_ (a record, a clear): a raster
    // pool copy replayed from an older one is stale (HtmlPoolState).
    uint64_t baseGeneration_ = 1;
    // The recording before baseCommands_ (swapped in at each record), which
    // the new one is diffed against, and the damage each generation brought:
    // a pool copy holding an older generation redraws only their union
    // (render/damage.h). Written by the main thread while the raster thread
    // is idle, read by it at replay, like baseCommands_.
    render::CommandBuffer prevBaseCommands_;
    render::DamageHistory baseDamage_;
    // Headless capture (capturePixels) keeps the same, over two pools it
    // alternates between as the raster thread's double-buffered ones do.
    render::CommandBuffer captureCommands_, capturePrevCommands_;
    uint64_t captureGeneration_ = 1;
    render::DamageHistory captureDamage_;
    int captureNext_ = 0;
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
    std::string titleOverride_;
    std::string fallbackTitle_ = "Bro";   // the window's title while the page has none
    void applyWindowIcon(const EngineConfig& config);
    std::function<void(Engine&)> installHostBindings_;
    std::function<void()> installWorkerHostBindings_;
    util::AssetMounts assetMounts_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes_;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenesDetached_;
    std::unordered_map<uint64_t, canvas::CanvasScene*> canvasSceneRegistry_;
    canvas::CanvasScene* canvasSceneById(uint64_t id) const;

    std::vector<WebGLEntry> webglEntries_;
    // The composite under way is the live loop's frame (not a capture, a
    // compile or a headless step): a WebGL canvas may show the frame before
    // the one its script just ended while the WebGL thread finishes
    // replaying it (WebGL2RenderingContext::frameEnd's mayLag).
    bool webglFramesMayLag_ = false;
    // The thread every WebGL context's backend lives on: made with the first
    // context, kept until shutdown (after the contexts, before the device).
    std::shared_ptr<webgl::WebGLThread> webglThread_;
    std::vector<KernelCanvasEntry> kernelCanvases_;
    // The kernel canvases' shared store pipelines: alive while one is.
    std::weak_ptr<render::KernelCanvasBlit> kernelCanvasBlit_;
    // The entry's canvas while it is alive (in the document or not), else null.
    dom::Element* liveElementOf(const WebGLEntry& entry) const;
    dom::Element* liveElementOf(const KernelCanvasEntry& entry) const;
    // Drops the contexts whose canvas is gone. A canvas out of the document
    // keeps its context, as in a browser: put back, it shows what it drew.
    void pruneDeadWebGLContexts();
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
    HtmlPoolState htmlPoolState_[2];
    ScrollLayerSet baseScrollLayers_{0};
    ScrollLayerSet captureScrollLayers_{1};
    // The app document's scroll absorber: a scroll of a scroller the frame
    // passes composite as a scroll layer moves its picture with no pass, when
    // nothing else the page paints follows that scroll (absorbLayerScroll).
    struct LayerScrollAbsorber final : dom::ScrollAbsorber {
        Engine* engine;
        explicit LayerScrollAbsorber(Engine* e) : engine(e) {}
        bool absorbScroll(dom::Element* e, float scrollLeft, float scrollTop) override {
            return engine->absorbLayerScroll(e, scrollLeft, scrollTop);
        }
    };
    LayerScrollAbsorber layerScrollAbsorber_{this};
    bool absorbLayerScroll(dom::Element* e, float scrollLeft, float scrollTop);
    // Whether the presenter last presented a windowed-loop frame, not a
    // capture's (headlessPresentedPixels).
    bool pipelinePresentedLast_ = false;
    std::vector<render::SkiaRenderer::LayerSurface> systemSurfacePool_[2];
    int systemSurfacePoolW_[2] = {0, 0}, systemSurfacePoolH_[2] = {0, 0};

    std::vector<render::SkiaRenderer::LayerSurface> screenshotHtmlPool_;
    int screenshotHtmlPoolW_ = 0, screenshotHtmlPoolH_ = 0;
    std::vector<render::SkiaRenderer::LayerSurface> capturePool_[2];
    int capturePoolW_[2] = {0, 0}, capturePoolH_[2] = {0, 0};
    HtmlPoolState capturePoolState_[2];
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
    std::unique_ptr<OverlayManager> overlayMgr_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<broaudio::Engine> audioEngine_;
    std::unique_ptr<AudioInference> audioInference_;
    // What audioEngine() opens the device with, kept from the config.
    struct AudioStartup {
        std::string appId, appName;
        bool realAudio = false;
    } audioStartup_;

    std::vector<std::function<void()>> framePumps_;
    std::vector<std::function<void()>> shutdownHooks_;
    std::vector<std::pair<uint64_t, PendingWorkSource>> pendingWorkSources_;
    uint64_t nextPendingWorkSource_ = 1;
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
    // The files changed since the last reload or `sourcechange` event, by path
    // from the app dir ('/lib/...' under the project's lib); unknown when a
    // burst overflowed the watcher or the folder was replaced.
    std::vector<std::string> appWatchChanged_;
    bool appWatchChangedUnknown_ = false;
    std::string appWatchDeferReason_;   // why the last watcher reload was held back
    bool appWatchRearm_ = false;        // a watcher died; re-create it once the dir is back
    // A page initAppRealm parses instead of index.html, with no scripts or
    // stylesheets: the error page a failed reload leaves up (app_reload.cpp).
    std::string appHtmlOverride_;
    std::vector<dom::Element*> pendingIframeReloads_;
    std::vector<render::SkiaRenderer::LayerSurface> iframeSurfaceFrees_;
    bool iframeSyncNeeded_ = false;
    std::unordered_map<dom::Element*, std::string> iframeLoadFailed_;
    // <appview>s their page stopped: not loaded until start().
    std::unordered_set<dom::Element*> stoppedIframes_;
    bool systemPerfVisible_ = false;
    bool systemSettingsVisible_ = false;
    bool splashVisible_ = false;
    bool splashEnabled_ = true;
    bool compiledApp_ = false;
    bool hostProvidesCompiledApp_ = false;
    std::string aotDir_;
    bool aotRequired_ = false;
    std::string aotBuildDir_;
    std::vector<uint64_t> appModuleHandles_;
    bool splashDismissTriggered_ = false;
    double splashStartMs_ = 0.0;
    double splashDismissStartMs_ = 0.0;
    // Background work shown in the panels (shader_status.cpp): the page's
    // compile progress, the splash's wait for the shaders, and the corner
    // status panel (system/status.html) after the splash.
    double compileProgress_ = -1.0;     // 0..1 while a page script compiles
    double splashReadyMs_ = -1.0;       // when only the shaders held the splash
    bool statusEnabled_ = false;
    bool statusVisible_ = false;
    bool statusBusy_ = false;
    double statusBusySinceMs_ = -1.0;
    double statusIdleSinceMs_ = -1.0;
    std::string splashWorkText_, statusWorkText_;
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
    bool serverTickRateExplicit_ = false;
    double serverStartTime_ = 0.0;
    bool serverStopRequested_ = false;

    FrameStats frameStats_;
    std::unique_ptr<FrameTrace> frameTrace_;
    std::unique_ptr<ControlServer> control_;
    uint64_t frameNumber_ = 0;
    // The last free-page release on a held frame (releaseHeapOnHeldFrame),
    // and whether frames were drawn since.
    double lastTrimMs_ = -1.0;
    bool drawnSinceTrim_ = true;
    // What the layout and raster threads measured for the pass the main
    // thread last claimed / consumed (written by those threads before they
    // publish, read by the main thread after it claims), and the content
    // generation in flight / on screen.
    struct PassTimes {
        double styleMs = 0, layoutMs = 0, animTickMs = 0, rasterMs = 0;
        uint32_t activeAnimations = 0, promoted = 0;
        bool layoutPerformed = false;
    };
    PassTimes layoutPassTimes_, rasterPassTimes_;
    // Lifetime raster passes and their time in µs (the raster thread adds,
    // perf.stats().paint reads).
    std::atomic<uint64_t> rasterPasses_{0}, rasterUs_{0};
    std::atomic<uint64_t> pagePartialMismatch_{0}, pagePartialRasters_{0};
    std::atomic<uint64_t> pageWholeHistory_{0}, pageWholeCover_{0}, pageWholeArea_{0};

public:
    uint64_t rasterPassCount() const { return rasterPasses_.load(std::memory_order_relaxed); }
    double rasterPassMsTotal() const { return rasterUs_.load(std::memory_order_relaxed) / 1000.0; }

private:
    // The flight recorder's hooks into the frame loops (engine_frame_trace.cpp).
    void traceFrameBegin(double frameStart);
    void traceLayoutSignalled();
    void traceLayoutClaimed();
    void traceRasterSignalled();
    void traceRasterConsumed();
    void traceKeyPress();
    void traceFrameEnd();
    uint64_t flipCountAtFrameStart_ = 0;
    double ptyOutputTraced_ = 0.0;  // the terminal output stamp the trace last reported
    uint64_t contentGenSignalled_ = 0, contentGenShown_ = 0;
    double contentTimeSignalled_ = 0.0, contentTimeShown_ = 0.0, layoutSignalTimeMs_ = 0.0;
    bool uiDirty_ = true;
    // The page cancelled the last keydown (preventDefault): the text the
    // platform sends after it for the same key (SDL's TEXT_INPUT, the DRM and
    // device paths' own) is dropped, as the web drops a cancelled keydown's
    // keypress and input. A shortcut that moves focus into a field (N names
    // an animal) must not type its own letter there. Cleared by the next
    // keydown or keyup.
    bool dropTextAfterKeyDown_ = false;
    bool hasRenderedOnce_ = false;
    bool mediaEventsArmed_ = false;
    // Set inside a headless advanceTime step once media is at its instant:
    // pumpVideoEvents then moves no media clock or picture (advanceTime).
    bool mediaHeldForStep_ = false;

    dom::ElementHandle hoveredElement_;
    // The pointer is in the main window (moved in, not left), and a layout,
    // DOM change or scroll since its hover was last hit tested (recheckHover).
    bool pointerInWindow_ = false;
    bool hoverRecheckPending_ = false;
    uint64_t hoverScrollGen_ = 0;
    scene::HtmlNode*   hoveredHtmlNode_ = nullptr;
    dom::ElementHandle hoveredHtmlElement_;
    scene::HtmlNode*   htmlNodeMouseDownNode_ = nullptr;
    dom::ElementHandle htmlNodeMouseDownElement_;

    // HTML5 drag and drop between elements (see engine/drag_drop.h). Distinct
    // from the OS file drop, which arrives from outside the window.
    DragDrop dragDrop_;
    bool nativeDrag_ = false;  // the window system carries the page's drag (beginNativeDrag)

    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;
    bool cursorVisible_ = false;
    int deviceInputMods_ = 0;  // injectDeviceInput's modifier state, off DRM

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
    bool gamepadsStarted_ = false;

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
    std::unique_ptr<DomUndoHistories> editUndo_;

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
    // Windowed: since when the newest layer set has been drawn at another
    // viewport than the window's (0: it is not), so the frame is held back.
    double resizeHoldSinceMs_ = 0.0;
    static constexpr double kResizeHoldMaxMs = 250.0;
    // Windowed: the longest the loop waits for the window system to want a
    // frame (Window::waitForFrame) before building one anyway.
    static constexpr double kFrameWaitMaxMs = 100.0;
    // Windowed: after input went to a <terminal>'s program, how long the
    // frame waits for the echo, so it shows in this frame rather than the
    // next (awaitTerminalEcho).
    static constexpr double kTerminalEchoWaitMs = 3.0;
    double lastGCMs_ = 0.0, lastGpuFrameMs_ = -1.0;
    bool sceneRenderPending_ = false;  // renderPendingScenes
    bool sceneDrawnForRead_ = false;   // a read drew the frame the last flush left: a capture shows that render
    bool inTimeStep_ = false;          // inside an advanceTime step
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
    // The client buffer last placed this frame (an index into frameImages_,
    // SIZE_MAX: none), with its source and where it went in device px: the
    // frame KMS can show as that buffer alone when it is all there is.
    size_t directImage_ = SIZE_MAX;
    render::DmabufLayerSource directSrc_;
    render::LayerQuad directQuad_;
    // DRM: presents this frame by scanning that buffer out directly, when
    // nothing else of the frame would show; false to composite it.
    bool presentDirectScanout();

    // What this frame shows, as a key built while it composites (the raster
    // generation, every placed image and where it went, each client frame's
    // sequence, each sub-document's publish, the drawn cursor): a frame
    // whose key matches the last presented one shows nothing new, and is not
    // presented. A layer whose content cannot be keyed (a 3D scene, a WebGL
    // canvas: drawn every frame) makes the frame volatile, always presented.
    void frameKeyAdd(uint64_t v);
    uint64_t frameKey_ = 0;
    bool frameVolatile_ = false;
    uint64_t presentedKey_ = 0;
    bool presentedKeyValid_ = false;
    // Present nothing for an unchanged frame: DRM commits the cursor plane
    // alone if the pointer moved; a window keeps its last buffer (where its
    // window system paces without a present). True when held.
    bool holdUnchangedFrame();
    uint64_t heldFrame_ = 0;  // the last frame held (frameNumber_)

    // engine_idle.cpp: a window whose event loop can wait (SDL) holds an
    // unchanged frame only when nothing is about to change it (windowIdle),
    // and the loop then waits for work (idleWait) instead of presenting the
    // same picture every vblank. Input, a wake from another thread
    // (util/main_loop_wake.h), the next timer or terminal blink, or the poll
    // bound ends the wait.
    bool windowIdle() const;
    void idleWait();

    // The windowed frame loop in parts (engine_frame.cpp), so headless can
    // run the same frame (runPipelineFrames, headless_pipeline.cpp).
    void installWindowEventHandlers();
    void startFramePipeline();
    void windowedFrame(bool modal = false);
    // A frame run from inside the OS's modal move/resize loop, which keeps
    // the frame that polled from returning (setModalWindowEventHook).
    void modalFrame();
    bool pollingEvents_ = false;    // the frame is in eventLoop_->pollEvents()
    bool inModalFrame_ = false;
    bool modalFramesRan_ = false;   // during this frame's pollEvents()
    bool windowedPipeline() const { return displayMode_ == DisplayMode::Windowed || headlessPipeline_; }
    bool presentHeadlessPipelineFrame(const render::PresentFrame& frame);
    bool headlessPipeline_ = false;         // inside runPipelineFrames
    bool headlessPipelineStarted_ = false;  // its threads are up
    double pipelineStepMs_ = 0.0;           // > 0: stepped clock
    double pipelineVblankMs_ = 0.0;         // the last emulated vblank
    PipelineRun pipelineRun_;
    void installMainLoopWaker();
    void removeMainLoopWaker();
    bool idleHeld_ = false;       // this frame was held by windowIdle
    // The longest idle wait: what is only polled each frame (fetch and
    // socket completions, ML jobs, audio events) is picked up within it.
    static constexpr double kIdlePollMs = 16.0;

    // engine_drm_cursor.cpp: the pointer on the KMS cursor plane. Places
    // this frame's cursor there; false when it must be drawn into the frame
    // (no plane, a shape too big for it, a driver that refused it).
    bool drmPlaceHardwareCursor();
    std::string hwCursorKey_;  // shape@scale whose image the plane holds
    bool hwCursorFits_ = false;
    int32_t hwCursorHotX_ = 0, hwCursorHotY_ = 0;
