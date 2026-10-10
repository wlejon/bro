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
// Several representations at once (setData: text and an image) go through
// SDL_SetClipboardData on X11, Wayland and macOS, which offer every MIME type
// given. On Windows SDL writes only one image format and reaches the
// clipboard only with its video subsystem up, so there the whole clipboard is
// Win32 (win32_clipboard.cpp), text included, with the same retry.
//
// Without the video subsystem (a server) SDL cannot reach the clipboard at
// all; Windows does not need it to. Elsewhere (a Linux box with no X server
// or Wayland) the clipboard is the in-process one instead (localClipboard):
// still one clipboard for the page (navigator.clipboard, execCommand) and its
// terminals.
#include "platform/clipboard.h"
#include "platform/sdl/sdl_backend.h"
#ifdef _WIN32
#include "platform/sdl/win32_clipboard.h"
#endif

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <atomic>
#include <memory>

namespace bro::platform {

namespace {

constexpr int kAttempts = 8;
constexpr Uint32 kBackoffMs = 2;

#ifndef _WIN32
bool videoUp() { return SDL_WasInit(SDL_INIT_VIDEO) != 0; }

// The data SDL_SetClipboardData hands out on request, per MIME type. SDL
// owns one of these from the call on and frees it through the cleanup
// callback when the clipboard is next replaced, so each attempt gets its own.
struct Offer {
    std::vector<std::string> mimeTypes;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> bytes;  // parallel to mimeTypes
};

const void* SDLCALL offerData(void* userdata, const char* mimeType, size_t* size) {
    auto* o = static_cast<Offer*>(userdata);
    for (size_t i = 0; i < o->mimeTypes.size(); ++i) {
        if (o->mimeTypes[i] == mimeType) {
            *size = o->bytes[i]->size();
            return o->bytes[i]->data();
        }
    }
    *size = 0;
    return nullptr;
}

void SDLCALL offerCleanup(void* userdata) { delete static_cast<Offer*>(userdata); }

Offer makeOffer(const std::vector<ClipboardData>& items) {
    Offer o;
    for (const auto& it : items) {
        auto bytes = std::make_shared<const std::vector<uint8_t>>(it.bytes);
        if (it.mimeType == "text/plain") {
            // The names X11 and Wayland readers ask for text by.
            for (const char* t : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"}) {
                o.mimeTypes.emplace_back(t);
                o.bytes.push_back(bytes);
            }
        } else {
            o.mimeTypes.push_back(it.mimeType);
            o.bytes.push_back(bytes);
        }
    }
    return o;
}
#endif

class SdlClipboard final : public Clipboard {
public:
    bool setText(const std::string& text) override {
#ifdef _WIN32
        return setData({{"text/plain", std::vector<uint8_t>(text.begin(), text.end())}});
#else
        if (!videoUp()) {
            const bool system = SDL_SetClipboardText(text.c_str());
            m_useLocal = !system;
            if (!system) localClipboard().setText(text);
            return true;
        }
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (SDL_SetClipboardText(text.c_str())) return true;
            if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
        }
        return false;
#endif
    }

    bool setData(const std::vector<ClipboardData>& items) override {
#ifdef _WIN32
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            switch (win32clip::setData(items)) {
                case win32clip::Result::Ok: return true;
                case win32clip::Result::Failed: return false;
                case win32clip::Result::Busy: break;
            }
            if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
        }
        return false;
#else
        if (!videoUp()) {
            m_useLocal = true;
            return localClipboard().setData(items);
        }
        const Offer offer = makeOffer(items);
        if (offer.mimeTypes.empty()) return SDL_ClearClipboardData();
        std::vector<const char*> names;
        for (const auto& m : offer.mimeTypes) names.push_back(m.c_str());
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            // SDL takes the offer whether or not the write lands, and frees
            // it when the clipboard is next replaced (a retry included).
            auto* mine = new Offer(offer);
            if (SDL_SetClipboardData(offerData, offerCleanup, mine, names.data(), names.size()))
                return true;
            if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
        }
        return false;
#endif
    }

    // `SDL_GetClipboardText` answers "" both for a clipboard that is empty and
    // for one it could not open, so the retry is also the only way to tell
    // those apart.
    std::string getText(bool* ok) override {
#ifdef _WIN32
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            bool busy = false;
            std::string text = win32clip::getText(&busy);
            if (!busy) {
                if (ok) *ok = true;
                return text;
            }
            if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
        }
        if (ok) *ok = false;
        return std::string();
#else
        if (!videoUp() && m_useLocal) return localClipboard().getText(ok);
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
#endif
    }

    std::optional<std::vector<uint8_t>> getData(const std::string& mimeType) override {
#ifdef _WIN32
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            bool busy = false;
            auto out = win32clip::getData(mimeType, &busy);
            if (!busy) return out;
            if (attempt + 1 < kAttempts) SDL_Delay(kBackoffMs);
        }
        return std::nullopt;
#else
        if (!videoUp() && m_useLocal) return localClipboard().getData(mimeType);
        if (!videoUp()) return std::nullopt;
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
#endif
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
    // The system clipboard could not be reached without video; the
    // in-process one stands in until a write reaches the system again.
    std::atomic<bool> m_useLocal{false};
};

} // namespace

Clipboard& sdlClipboard() {
    static SdlClipboard c;
    return c;
}

} // namespace bro::platform
