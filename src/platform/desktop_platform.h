#pragma once

#include <string>

struct SDL_Window;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace bro::platform::desktop {

/// Whether running in headless mode.
bool isHeadless();
void setHeadless(bool headless);

#ifdef _WIN32
/// Retrieves HWND from an SDL_Window pointer.
HWND hwndOf(SDL_Window* window);

/// UTF-8 / UTF-16 conversions.
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& w);
void copyWide(wchar_t* dst, size_t cap, const std::wstring& src);
#endif

/// Pump any queued desktop events (hotkeys, single-instance forwarding, tray clicks).
/// Called from the engine main thread loop.
void pumpEvents();

} // namespace bro::platform::desktop
