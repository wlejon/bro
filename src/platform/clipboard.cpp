#include "platform/clipboard.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

namespace bro::platform {

namespace {

// Eight attempts two milliseconds apart: about ten milliseconds of patience in
// the worst case, and none at all in the overwhelmingly common one where the
// first attempt succeeds. See the header for why the ceiling is low.
constexpr int kAttempts = 8;
constexpr Uint32 kBackoffMs = 2;

} // namespace

bool setClipboardText(const std::string& text) {
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (SDL_SetClipboardText(text.c_str())) return true;
        if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
    }
    return false;
}

std::string getClipboardText(bool* ok) {
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
