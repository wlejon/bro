#pragma once
// The SDL backend's Window. Internal to src/platform.

#include "platform/window.h"

#include <cmath>

struct SDL_Window;
struct SDL_Cursor;

namespace bro::platform {

class SdlWindow final : public Window, private TextInput, private Cursor {
public:
    /// Throws std::runtime_error when SDL cannot make the window.
    explicit SdlWindow(const WindowConfig& config);
    ~SdlWindow() override;

    SDL_Window* sdlWindow() const { return m_window; }

    uint32_t windowId() const override;
    GraphicsBackend backend() const override { return m_backend; }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override;
    bool displayAdapterLuid(uint8_t luid[8]) const override;
    void setVSync(bool enabled) override { m_vsyncPref = enabled; }
    bool vsyncPreference() const override { return m_vsyncPref; }

    /// Windows (OLE) and macOS (NSDraggingSession); see sdl_drag.h.
    bool startDrag(const DragSource& drag) override;

    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    void setSize(uint32_t width, uint32_t height) override { m_width = width; m_height = height; }
    void getSize(int& w, int& h) const override;
    void getSizeInPixels(int& w, int& h) const override;
    float getPixelDensity() const override;
    float getDisplayScale() const override;
    void refreshDisplayScale();
    /// CSS px to SDL window coordinates (m_cssScale).
    int toWindowUnits(float css) const { return static_cast<int>(std::lround(css * m_cssScale)); }
    int toCss(int units) const { return static_cast<int>(std::lround(units / m_cssScale)); }
    void setWindowSize(uint32_t width, uint32_t height) override;
    void setPosition(int x, int y) override;
    void getPosition(int& x, int& y) const override;
    void setMinimumSize(int w, int h) override;
    void getMinimumSize(int& w, int& h) const override;
    void setMaximumSize(int w, int h) override;
    void getMaximumSize(int& w, int& h) const override;

    void setTitle(const std::string& title) override;
    std::string getTitle() const override;
    float getOpacity() const override;
    void setOpacity(float opacity) override;
    void setResizable(bool resizable) override;
    void setBorderless(bool borderless) override;
    bool isBorderless() const override;
    void setAlwaysOnTop(bool onTop) override;
    bool isAlwaysOnTop() const override;
    void setFullscreen(bool fullscreen) override;
    bool isFullscreen() const override;
    void minimize() override;
    void maximize() override;
    void restore() override;
    bool isMinimized() const override;
    bool isMaximized() const override;
    bool isHidden() const override;
    void show() override;
    void hide() override;
    void sync() override;
    void raise() override;
    bool isFocused() const override;
    bool flash(bool on = true) override;
    void setIcon(const std::string& pngPath) override;
    bool setIconPixels(int width, int height, const uint8_t* rgba) override;

    uint32_t currentDisplay() const override;
    bool moveToDisplay(uint32_t displayId) override;

    bool isTransparent() const override { return m_transparent; }
    bool setClickThrough(bool on) override;
    bool isClickThrough() const override { return m_clickThroughOn; }
    bool wantsInputRegion() const override;
    void setInputRegion(const std::vector<WindowRect>& rects) override;
    void setInputTest(std::function<bool(float x, float y)> takesInput) override;
    void refreshInputTest() override;
    /// Apply what the click-through tracker decided off the main thread
    /// (Windows: the pointer crossed into or out of the input region).
    void applyClickThrough();

    TextInput& textInput() override { return *this; }
    Cursor& cursor() override { return *this; }
    void setKeyboardGrab(bool on) override;
    NativeHandle nativeHandle() const override;

private:
    // TextInput
    void start() override;
    void stop() override;
    void setArea(int x, int y, int w, int h, int cursor) override;
    // Cursor
    void setShape(CursorShape shape) override;
    void setRelativeMode(bool enabled) override;
    void warp(float x, float y) override;

    SDL_Window* m_window = nullptr;
    std::string m_title;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    float m_opacity = 1.0f;
    float m_displayScale = 1.0f;
    // Window coordinates (SDL's) per CSS px: on Windows, where SDL's window
    // coordinates are physical pixels, the display scale (a 4K screen at
    // 150% shows the page at 150%, as macOS does with points); 1 elsewhere.
    // Sizes, positions in the window and input are converted at this class
    // and the event loop, so the engine sees CSS px and a pixel density
    // (getPixelDensity) it renders at.
    float m_cssScale = 1.0f;
    bool m_fullscreen = false;
    bool m_flashing = false;
    bool m_vsyncPref = true;
    bool m_alwaysOnTop = false;
    bool m_borderless = false;
    bool m_keepHidden = false;  // standing in for the offscreen driver (SdlRuntime::offscreenFallback)
    bool m_transparent = false;
    bool m_clickThroughOn = false;
    std::vector<WindowRect> m_inputRegion;
    std::function<bool(float, float)> m_inputTest;
    std::unique_ptr<class SdlClickThrough> m_clickThrough;
    GraphicsBackend m_backend = GraphicsBackend::Vulkan;
    SDL_Cursor* m_cursors[static_cast<int>(CursorShape::Count_)] = {};
    CursorShape m_cursorShape = CursorShape::Default;
};

/// The SDL window behind a platform Window, or null when it is not one.
SDL_Window* sdlWindowOf(const Window* window);
void sdlRefreshWindowDisplayScale(uint32_t windowId);
/// SDL window coordinates per CSS px of a window (SdlWindow::m_cssScale; 1
/// for a window that is not ours): divide an event's position by it.
float sdlCssScale(SDL_Window* window);
float sdlCssScaleOf(uint32_t windowId);
/// Every click-through window's applyClickThrough (the event loop, on each pump).
void sdlApplyClickThrough();

} // namespace bro::platform
