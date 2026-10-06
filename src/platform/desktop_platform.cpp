#include "platform/desktop_platform.h"
#include <SDL3/SDL.h>
#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bro::platform::desktop {

void pumpHotkeyEvents();
void pumpSingleInstanceEvents();

namespace {
std::atomic<bool> s_headless{false};
}

bool isHeadless() {
    return s_headless.load(std::memory_order_relaxed);
}

void setHeadless(bool headless) {
    s_headless.store(headless, std::memory_order_relaxed);
}

#ifdef _WIN32
HWND hwndOf(SDL_Window* window) {
    if (!window) return nullptr;
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void copyWide(wchar_t* dst, size_t cap, const std::wstring& src) {
    if (!dst || cap == 0) return;
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = L'\0';
}
#endif

void pumpEvents() {
    pumpHotkeyEvents();
    pumpSingleInstanceEvents();
}

} // namespace bro::platform::desktop
