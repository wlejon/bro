#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct SDL_Window;

namespace bro::platform {

class Window {
public:
    Window(const std::string& title, uint32_t width, uint32_t height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    SDL_Window* getSDLWindow() const { return m_window; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

    void setSize(uint32_t width, uint32_t height) { m_width = width; m_height = height; }

    std::vector<const char*> getVulkanInstanceExtensions() const;

private:
    SDL_Window* m_window = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace bro::platform
