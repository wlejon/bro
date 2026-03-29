#include "platform/sdl_window.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <stdexcept>

namespace bro::platform {

Window::Window(const std::string& title, uint32_t width, uint32_t height)
    : m_width(width), m_height(height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("Failed to initialize SDL: %s", SDL_GetError());
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    m_window = SDL_CreateWindow(title.c_str(),
                                static_cast<int>(width),
                                static_cast<int>(height),
                                flags);

    if (!m_window) {
        LOG_ERROR("Failed to create SDL window: %s", SDL_GetError());
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    LOG_INFO("Created window \"%s\" (%ux%u)", title.c_str(), width, height);
}

Window::~Window() {
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
}

} // namespace bro::platform
