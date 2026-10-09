// Native message boxes and file dialogs through SDL.
#include "platform/dialogs.h"
#include "platform/sdl/sdl_backend.h"
#include "platform/sdl/sdl_window.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_messagebox.h>

#include <atomic>

namespace bro::platform {

namespace {

#ifdef _WIN32
constexpr int kMaxDialogCallbacks = 2;
#else
constexpr int kMaxDialogCallbacks = 1;
#endif

struct DialogResult {
    std::atomic<bool> haveResult{false};
    std::atomic<int> callbackCount{0};
    std::vector<std::string> files;
    // Why the dialog was refused, read out of SDL on the thread that set it.
    // SDL's error is thread-local and the refusal that matters here — a filter
    // SDL will not accept — is delivered synchronously from
    // `SDL_ShowOpenFileDialog` itself, but a backend may refuse from a thread
    // of its own. Taking it here rather than after the wait is the only
    // spelling that is right in both cases.
    std::string error;
};

void SDLCALL dialogCallback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* result = static_cast<DialogResult*>(userdata);
    if (filelist) {
        for (const char* const* p = filelist; *p; ++p) {
            result->files.emplace_back(*p);
        }
        result->haveResult.store(true, std::memory_order_release);
    } else if (result->error.empty()) {
        const char* msg = SDL_GetError();
        result->error = msg && *msg ? msg : "the dialog was refused";
    }
    result->callbackCount.fetch_add(1, std::memory_order_release);
}

// How long a dialog that has already answered once is given to answer again.
// Any second callback is delivered from the same place as the first and
// arrives immediately; this is a bound, not a poll interval.
constexpr Uint64 kExtraCallbackGraceMs = 500;

// Block until the dialog has answered, keeping timers running while it is up.
//
// An error answers once, and waiting for a second answer that is not coming is
// a window that never comes back. SDL calls back with a null `filelist` when it
// refuses the request, and on Windows this loop wants two callbacks before it
// returns; one bad filter string therefore used to hang the application with
// its last frame on the screen and no dialog to close. The second callback is
// still waited for — `DialogResult` lives on the caller's stack, and a callback
// arriving after this returns would write into a frame that is gone — but the
// wait is bounded, and the reason is logged.
//
// False when refused, which is NOT the same as cancelled: SDL hands over an
// empty list for a cancel and a null one for a refusal.
bool waitForDialog(DialogResult& result, const std::function<void()>& tick) {
    Uint64 answeredAt = 0;
    for (;;) {
        const bool have = result.haveResult.load(std::memory_order_acquire);
        const int count = result.callbackCount.load(std::memory_order_acquire);
        if (have || count >= kMaxDialogCallbacks) break;
        if (count > 0) {
            const Uint64 now = SDL_GetTicks();
            if (!answeredAt) {
                answeredAt = now;
                LOG_WARN("dialog: refused — %s", result.error.c_str());
            } else if (now - answeredAt >= kExtraCallbackGraceMs) {
                break;
            }
        }
        SDL_PumpEvents();
        if (tick) tick();
        SDL_Delay(8);
    }
    return result.haveResult.load(std::memory_order_acquire);
}

enum : int { kBtnCancel = 0, kBtnOk = 1 };

class SdlDialogs final : public DialogBackend {
public:
    std::optional<bool> messageBox(Window* parent, const std::string& message,
                                   bool withCancel) override {
        SDL_MessageBoxButtonData buttons[2];
        int nButtons = 0;
        if (withCancel) {
            buttons[nButtons++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kBtnCancel, "Cancel"};
        }
        buttons[nButtons++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kBtnOk, "OK"};

        SDL_MessageBoxData data{};
        data.flags = SDL_MESSAGEBOX_INFORMATION;
        data.window = sdlWindowOf(parent);
        data.title = "";
        data.message = message.c_str();
        data.numbuttons = nButtons;
        data.buttons = buttons;

        int pressed = kBtnCancel;
        if (!SDL_ShowMessageBox(&data, &pressed)) {
            LOG_WARN("[dialog] message box unavailable: %s", SDL_GetError());
            return std::nullopt;
        }
        return pressed == kBtnOk;
    }

    bool fileDialog(Window* parent, const FileDialogRequest& request,
                    const std::function<void()>& tick,
                    std::vector<std::string>& picked, std::string& refusal) override {
        std::vector<SDL_DialogFileFilter> filters;
        filters.reserve(request.filters.size());
        for (const auto& f : request.filters) filters.push_back({f.name.c_str(), f.pattern.c_str()});
        const SDL_DialogFileFilter* filterData = filters.empty() ? nullptr : filters.data();
        const int filterCount = static_cast<int>(filters.size());
        const char* defaultLoc =
            request.defaultLocation.empty() ? nullptr : request.defaultLocation.c_str();
        SDL_Window* window = sdlWindowOf(parent);

        DialogResult result;
        switch (request.kind) {
            case FileDialogKind::OpenFile:
                SDL_ShowOpenFileDialog(dialogCallback, &result, window, filterData, filterCount,
                                       nullptr, request.allowMultiple);
                break;
            case FileDialogKind::OpenFolder:
                SDL_ShowOpenFolderDialog(dialogCallback, &result, window, defaultLoc,
                                         request.allowMultiple);
                break;
            case FileDialogKind::SaveFile:
                SDL_ShowSaveFileDialog(dialogCallback, &result, window, filterData, filterCount,
                                       defaultLoc);
                break;
        }
        if (!waitForDialog(result, tick)) {
            refusal = "file dialog refused: " +
                      (result.error.empty() ? std::string("unknown reason") : result.error);
            return false;
        }
        picked = std::move(result.files);
        return true;
    }
};

} // namespace

DialogBackend& sdlDialogs() {
    static SdlDialogs d;
    return d;
}

} // namespace bro::platform
