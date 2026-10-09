// The system clipboard through SDL, with the one thing SDL leaves to the
// caller: a retry.
//
// **The clipboard is opened exclusively, one process at a time.** On Windows
// `OpenClipboard` fails outright while somebody else holds it — a clipboard
// manager polling it, an editor, another bro — and SDL reports that faithfully
// and gives up. Measured with six bro processes contending: 259 failed writes
// in 300, and one process that failed all 300. The same loop alone: 300 for
// 300. So this is not a rare corner; it is what a copy does on a desk with a
// clipboard manager running.
//
// The contention is transient by nature — a holder opens, reads and closes in
// microseconds — so a few attempts spread over a few milliseconds turn "copy
// silently does nothing" into "copy works". Bounded deliberately: this runs on
// the thread that draws, and a copy that cannot be made in about ten
// milliseconds is better reported to the caller than waited on.
//
// Without the video subsystem (a server, or headless whose window could not
// be made) SDL still reaches the system clipboard on Windows. Where it cannot
// (a Linux box with no X server or Wayland) the clipboard is a string in this
// process instead: still one clipboard for the page (navigator.clipboard,
// execCommand) and its terminals.
#include "platform/clipboard.h"
#include "platform/sdl/sdl_backend.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <mutex>

namespace bro::platform {

namespace {

constexpr int kAttempts = 8;
constexpr Uint32 kBackoffMs = 2;

bool videoUp() { return SDL_WasInit(SDL_INIT_VIDEO) != 0; }

class SdlClipboard final : public Clipboard {
public:
    bool setText(const std::string& text) override {
        if (!videoUp()) {
            const bool system = SDL_SetClipboardText(text.c_str());
            std::lock_guard<std::mutex> g(m_localMu);
            m_useLocal = !system;
            m_local = system ? std::string() : text;
            return true;
        }
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (SDL_SetClipboardText(text.c_str())) return true;
            if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
        }
        return false;
    }

    // `SDL_GetClipboardText` answers "" both for a clipboard that is empty and
    // for one it could not open, so the retry is also the only way to tell
    // those apart.
    std::string getText(bool* ok) override {
        if (!videoUp()) {
            std::lock_guard<std::mutex> g(m_localMu);
            if (m_useLocal) {
                if (ok) *ok = true;
                return m_local;
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
            // says there is text to be had, this read lost a race worth
            // retrying. If it cannot get in either it answers false, and an
            // empty clipboard is what this reports — exactly what it reported
            // before the retry existed, so the read is never made worse by
            // asking.
            if (!SDL_HasClipboardText()) {
                if (ok) *ok = true;
                return std::string();
            }
            if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
        }
        if (ok) *ok = false;
        return std::string();
    }

    std::optional<std::vector<uint8_t>> getData(const std::string& mimeType) override {
        if (!SDL_HasClipboardData(mimeType.c_str())) return std::nullopt;
        size_t n = 0;
        void* p = SDL_GetClipboardData(mimeType.c_str(), &n);
        std::optional<std::vector<uint8_t>> out;
        if (p && n > 0) {
            auto* bp = static_cast<const uint8_t*>(p);
            out.emplace(bp, bp + n);
        }
        if (p) SDL_free(p);
        return out;
    }

    // The OS primary selection exists on X11 and Wayland only.
    bool setPrimaryText(const std::string& text) override {
#if defined(__linux__) || defined(__FreeBSD__)
        return SDL_SetPrimarySelectionText(text.c_str());
#else
        (void)text;
        return false;
#endif
    }

    std::optional<std::string> getPrimaryText() override {
#if defined(__linux__) || defined(__FreeBSD__)
        if (SDL_HasPrimarySelectionText()) {
            if (char* t = SDL_GetPrimarySelectionText()) {
                std::string s(t);
                SDL_free(t);
                return s;
            }
        }
#endif
        return std::nullopt;
    }

private:
    std::mutex m_localMu;
    std::string m_local;
    bool m_useLocal = false;
};

} // namespace

Clipboard& sdlClipboard() {
    static SdlClipboard c;
    return c;
}

} // namespace bro::platform
