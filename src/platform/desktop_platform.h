#pragma once

#include <string>

namespace bro::platform { class Window; }

#ifdef _WIN32
// HWND without <windows.h>: this header reaches engine code, where windows.h's
// min/max macros break std::min/std::max in sibling headers. Matches
// DECLARE_HANDLE(HWND) under STRICT, so the .cpp files that do include
// windows.h see the same type.
struct HWND__;
typedef HWND__* HWND;
#endif

namespace bro::platform::desktop {

/// Whether running in headless mode.
bool isHeadless();
void setHeadless(bool headless);

#ifdef _WIN32
/// The HWND behind a window (null for none).
HWND hwndOf(const Window* window);

/// UTF-8 / UTF-16 conversions.
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& w);
void copyWide(wchar_t* dst, size_t cap, const std::wstring& src);
#endif

/// Pump any queued desktop events (hotkeys, single-instance forwarding, tray clicks).
/// Called from the engine main thread loop.
void pumpEvents();

} // namespace bro::platform::desktop
