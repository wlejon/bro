#pragma once
// The Wayland backend's Window: a browl::Window (xdg_toplevel) whose surface
// a Vulkan swapchain presents to. Internal to src/platform.
//
// Sizes: window coordinates are the compositor's logical px. The drawable is
// the logical size times the surface's preferred scale (fractional scale
// through wp_viewporter), so getPixelDensity is that scale and the engine
// renders at it.

#include "platform/window.h"
#include "platform/wayland/wayland_backend.h"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::platform::wl {

class WaylandWindow final : public Window, private TextInput, private Cursor {
public:
    /// Throws std::runtime_error when the compositor will not make the window.
    WaylandWindow(Connection& conn, const WindowConfig& config);
    ~WaylandWindow() override;

    browl::Window& wl() { return *window_; }
    browl::SurfaceId surfaceId() const { return window_->id(); }

    uint32_t windowId() const override { return static_cast<uint32_t>(window_->id()); }
    GraphicsBackend backend() const override { return backend_; }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override;
    bool presentPixels(const void* pixels, int width, int height, int stride, bool bgra) override;
    void setVSync(bool enabled) override { vsyncPref_ = enabled; }
    bool vsyncPreference() const override { return vsyncPref_; }
    void beforePresent(uint64_t tag) override;
    std::vector<PresentedFrame> takePresentedFrames() override;
    bool reportsPresentation() const override;
    bool waitForFrame(double timeoutMs) override;
    bool pacesPresents() const override { return vsyncPref_; }

    uint32_t getWidth() const override { return width_; }
    uint32_t getHeight() const override { return height_; }
    void setSize(uint32_t width, uint32_t height) override { width_ = width; height_ = height; }
    void getSize(int& w, int& h) const override;
    void getSizeInPixels(int& w, int& h) const override;
    float getPixelDensity() const override;
    float getDisplayScale() const override;
    void setWindowSize(uint32_t width, uint32_t height) override;
    void setPosition(int x, int y) override;
    void getPosition(int& x, int& y) const override;
    void setMinimumSize(int w, int h) override;
    void getMinimumSize(int& w, int& h) const override;
    void setMaximumSize(int w, int h) override;
    void getMaximumSize(int& w, int& h) const override;

    void setTitle(const std::string& title) override;
    std::string getTitle() const override { return title_; }
    float getOpacity() const override { return opacity_; }
    void setOpacity(float opacity) override;
    void setResizable(bool resizable) override;
    void setBorderless(bool borderless) override;
    bool isBorderless() const override { return borderless_; }
    void setAlwaysOnTop(bool onTop) override { alwaysOnTop_ = onTop; }
    bool isAlwaysOnTop() const override { return alwaysOnTop_; }
    void setFullscreen(bool fullscreen) override;
    bool isFullscreen() const override;
    void minimize() override;
    void maximize() override;
    void restore() override;
    // Wayland has no minimized state; a compositor that hides a window
    // (minimized, another workspace) says "suspended" and stops its frame
    // callbacks, after which a FIFO present would block until it is shown
    // again. Both read as minimized, so the swapchain stops presenting and
    // the loop keeps running (timers, a single-instance hand-off's raise).
    bool isMinimized() const override {
        return minimized_ || (reportedStates & browl::window_state::Suspended) != 0;
    }
    bool isMaximized() const override;
    bool isHidden() const override { return !window_->mapped(); }
    void show() override;
    void hide() override;
    void sync() override;
    void raise() override;
    bool isFocused() const override { return focused_; }
    bool flash(bool on = true) override;
    void setIcon(const std::string& pngPath) override;
    bool setIconPixels(int width, int height, const uint8_t* rgba) override;

    uint32_t currentDisplay() const override;
    bool moveToDisplay(uint32_t displayId) override;

    TextInput& textInput() override { return *this; }
    Cursor& cursor() override { return *this; }
    NativeHandle nativeHandle() const override;

    // --- Driven by Connection / the EventLoop ---

    /// A configure was acked: take its size. Returns true when the logical
    /// size changed.
    bool applyConfigure(const browl::WindowSnapshot& snap);
    /// The preferred scale changed; true when the drawable changed.
    bool applyScale(uint32_t scale120);
    void addPresentation(const browl::PresentationFeedbackEvent& ev);
    void frameDone(browl::RequestId request);
    void setFocused(bool focused) { focused_ = focused; }
    void setMinimizedFlag(bool m) { minimized_ = m; }
    bool textInputActive() const { return textInputActive_; }
    CursorShape cursorShape() const { return cursorShape_; }
    bool relativeMode() const { return relative_; }
    int logicalWidth() const { return logicalW_; }
    int logicalHeight() const { return logicalH_; }
    /// What changed since the EventLoop last asked, for it to report.
    bool takeResized() { return std::exchange(resized_, false); }
    bool takeScaleChanged() { return std::exchange(scaleChanged_, false); }
    bool takeMinimizedNow() { return std::exchange(minimizedNow_, false); }
    /// The state the EventLoop last reported to the engine, to turn
    /// configures into transitions.
    uint32_t reportedStates = 0;

private:
    // TextInput
    void start() override;
    void stop() override;
    void setArea(int x, int y, int w, int h, int cursor) override;
    // Cursor
    void setShape(CursorShape shape) override;
    void setRelativeMode(bool enabled) override;
    void warp(float x, float y) override;

    void waitForConfigure();
    void updateLogicalSize();
    void applyResizable();
    void spendLaunchToken();

    Connection& conn_;
    std::unique_ptr<browl::Window> window_;
    std::string title_;
    uint32_t width_ = 0, height_ = 0;  // last size set through this interface
    int logicalW_ = 0, logicalH_ = 0;  // what the compositor configured
    uint32_t scale120_ = 120;
    int minW_ = 0, minH_ = 0, maxW_ = 0, maxH_ = 0;
    float opacity_ = 1.0f;
    bool resizable_ = true;
    bool borderless_ = false;
    bool alwaysOnTop_ = false;
    bool vsyncPref_ = true;
    bool focused_ = false;
    bool minimized_ = false;
    bool textInputActive_ = false;
    bool relative_ = false;
    bool launchTokenSpent_ = false;
    bool resized_ = false;
    bool scaleChanged_ = false;
    bool minimizedNow_ = false;
    CursorShape cursorShape_ = CursorShape::Default;
    GraphicsBackend backend_ = GraphicsBackend::Vulkan;

    mutable std::mutex presentMu_;
    std::unordered_map<browl::RequestId, uint64_t> pendingFeedback_;  // request -> tag
    browl::RequestId frameRequest_ = 0;  // the frame callback waitForFrame waits on; 0 none
    bool earlyFrame_ = false;  // a frame went early for input since the last callback
    std::vector<PresentedFrame> presented_;
    bool feedbackSeen_ = false;

    // Software presentation: two shm buffers, the one not in use is drawn.
    std::shared_ptr<browl::ShmPool> shmPool_;
    std::shared_ptr<browl::ShmBuffer> shmBuffers_[2];
};

}  // namespace bro::platform::wl
