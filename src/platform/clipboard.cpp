#include "platform/clipboard.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <mutex>

namespace bro::platform {

namespace {

// Eight attempts two milliseconds apart: about ten milliseconds of patience in
// the worst case, and none at all in the overwhelmingly common one where the
// first attempt succeeds. See the header for why the ceiling is low.
constexpr int kAttempts = 8;
constexpr Uint32 kBackoffMs = 2;

// The clipboard when there is no system one to reach: no video subsystem
// (headless), and SDL could not set the clipboard without one (Windows can;
// a Linux box with no display cannot). g_useLocal: the last write went here.
std::mutex g_localMu;
std::string g_local;
bool g_useLocal = false;

bool videoUp() { return SDL_WasInit(SDL_INIT_VIDEO) != 0; }

} // namespace

bool setClipboardText(const std::string& text) {
    if (!videoUp()) {
        const bool system = SDL_SetClipboardText(text.c_str());
        std::lock_guard<std::mutex> g(g_localMu);
        g_useLocal = !system;
        g_local = system ? std::string() : text;
        return true;
    }
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (SDL_SetClipboardText(text.c_str())) return true;
        if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
    }
    return false;
}

std::string getClipboardText(bool* ok) {
    if (!videoUp()) {
        std::lock_guard<std::mutex> g(g_localMu);
        if (g_useLocal) {
            if (ok) *ok = true;
            return g_local;
        }
    }
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        char* raw = SDL_GetClipboardText();
        std::string text = raw ? raw : "";
        if (raw) SDL_free(raw);
        if (!text.empty()) {
            if (ok) *ok = true;
            return text;
        }
        // Empty — which is either the truth or a read that never got in.
        // `SDL_HasClipboardText` is the question that separates them; if it
        // says there is text to be had, this read lost a race worth retrying.
        // If it cannot get in either it answers false, and an empty clipboard
        // is what this reports — exactly what it reported before the retry
        // existed, so the read is never made worse by asking.
        if (!SDL_HasClipboardText()) {
            if (ok) *ok = true;
            return std::string();
        }
        if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
    }
    if (ok) *ok = false;
    return std::string();
}

} // namespace bro::platform
