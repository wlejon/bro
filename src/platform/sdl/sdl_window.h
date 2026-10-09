#pragma once
// The SDL backend's Window. Internal to src/platform.

#include "platform/window.h"

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
    bool presentPixels(const void* pixels, int width, int height, int stride, bool bgra) override;
    void setVSync(bool enabled) override { m_vsyncPref = enabled; }
    bool vsyncPreference() const override { return m_vsyncPref; }

    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    void setSize(uint32_t width, uint32_t height) override { m_width = width; m_height = height; }
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

    uint32_t currentDisplay() const override;
    bool moveToDisplay(uint32_t displayId) override;

    TextInput& textInput() override { return *this; }
    Cursor& cursor() override { return *this; }
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
    bool m_fullscreen = false;
    bool m_flashing = false;
    bool m_vsyncPref = true;
    bool m_alwaysOnTop = false;
    bool m_borderless = false;
    GraphicsBackend m_backend = GraphicsBackend::Vulkan;
    SDL_Cursor* m_cursors[static_cast<int>(CursorShape::Count_)] = {};
    CursorShape m_cursorShape = CursorShape::Default;
};

/// The SDL window behind a platform Window, or null when it is not one.
SDL_Window* sdlWindowOf(const Window* window);

} // namespace bro::platform
