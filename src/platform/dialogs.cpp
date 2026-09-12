#include "platform/dialogs.h"
#include "util/log.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_messagebox.h>
#include <atomic>
#include <cctype>
#include <cstring>
#include <utility>

namespace bro::platform {

namespace {

SDL_Window* s_window = nullptr;
bool s_interactive = true;
bool s_autoAccept = true;
Dialogs::TickCallback s_tickCb;
std::vector<std::string> s_queuedPicks;

std::string normalizeSeparators(std::string s) {
#ifdef _WIN32
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return s;
}

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

// A filter string as SDL's filter array. `"Images|png;jpg"` is one filter and
// `"Documents|json|Media|mp4;mkv|All files|*"` is three: names and patterns
// alternating. A string with no `|` at all is the pattern of a filter called
// "Files". A trailing name with no pattern is dropped rather than guessed at.
struct FileFilters {
    std::vector<std::string> parts;          // name, pattern, name, pattern, …
    std::vector<SDL_DialogFileFilter> list;  // built once `parts` has stopped growing

    const SDL_DialogFileFilter* data() const { return list.empty() ? nullptr : list.data(); }
    int count() const { return static_cast<int>(list.size()); }
};

FileFilters filtersFrom(const std::string& filterStr) {
    FileFilters f;
    if (filterStr.empty()) return f;

    for (size_t start = 0;;) {
        const size_t bar = filterStr.find('|', start);
        if (bar == std::string::npos) {
            f.parts.push_back(filterStr.substr(start));
            break;
        }
        f.parts.push_back(filterStr.substr(start, bar - start));
        start = bar + 1;
    }
    if (f.parts.size() == 1) f.parts.insert(f.parts.begin(), "Files");
    if (f.parts.size() % 2 != 0) f.parts.pop_back();

    f.list.reserve(f.parts.size() / 2);
    for (size_t i = 0; i + 1 < f.parts.size(); i += 2) {
        SDL_DialogFileFilter one;
        one.name = f.parts[i].c_str();
        one.pattern = f.parts[i + 1].c_str();
        f.list.push_back(one);
    }
    return f;
}

// SDL's own rule, applied here so a bad pattern is refused with a sentence
// even on a backend that would answer it with a silent null callback.
const char* validateFilterPattern(const char* list) {
    if (!list || !*list) return "Empty pattern not allowed";
    if (std::strcmp(list, "*") == 0) return nullptr;
    for (const char* c = list; *c; ++c) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            *c == '-' || *c == '_' || *c == '.') {
            continue;
        } else if (*c == ';') {
            if (c == list || c[-1] == ';') return "Empty pattern not allowed";
        } else {
            return "Invalid character in pattern (Only [a-zA-Z0-9_.-] allowed, or a single *)";
        }
    }
    if (list[std::strlen(list) - 1] == ';') return "Empty pattern not allowed";
    return nullptr;
}

bool refusedFilters(const FileFilters& filters, std::string& refusal) {
    for (const auto& filter : filters.list) {
        if (const char* err = validateFilterPattern(filter.pattern)) {
            refusal = std::string("file dialog refused: Invalid dialog file filters: ") + err;
            return true;
        }
    }
    return false;
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
bool waitForDialog(DialogResult& result) {
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
        if (s_tickCb) s_tickCb();
        SDL_Delay(8);
    }
    return result.haveResult.load(std::memory_order_acquire);
}

std::string refusalOf(const DialogResult& result) {
    return "file dialog refused: " + (result.error.empty() ? std::string("unknown reason")
                                                            : result.error);
}

std::vector<std::string> takeQueuedPicks() {
    std::vector<std::string> picked;
    picked.swap(s_queuedPicks);
    return picked;
}

enum : int { kBtnCancel = 0, kBtnOk = 1 };

bool showMessageBox(const std::string& message, bool withCancel) {
    SDL_MessageBoxButtonData buttons[2];
    int nButtons = 0;
    if (withCancel) {
        buttons[nButtons++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kBtnCancel, "Cancel"};
    }
    buttons[nButtons++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kBtnOk, "OK"};

    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = s_window;
    data.title = "";
    data.message = message.c_str();
    data.numbuttons = nButtons;
    data.buttons = buttons;

    int pressed = kBtnCancel;
    if (!SDL_ShowMessageBox(&data, &pressed)) {
        LOG_WARN("[dialog] message box unavailable: %s", SDL_GetError());
        return !withCancel;
    }
    return pressed == kBtnOk;
}

// `<input accept>`'s comma list of `.ext` and `type/*` tokens as one
// `;`-separated pattern.
std::string patternFromAccept(const std::string& accept) {
    std::string pattern;
    auto add = [&pattern](const std::string& ext) {
        if (ext.empty()) return;
        if (!pattern.empty()) pattern += ';';
        pattern += ext;
    };
    size_t start = 0;
    while (start <= accept.size() && !accept.empty()) {
        size_t comma = accept.find(',', start);
        std::string tok = accept.substr(start, comma == std::string::npos ? std::string::npos
                                                                          : comma - start);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
        if (!tok.empty()) {
            if (tok[0] == '.') {
                add(tok.substr(1));
            } else if (tok == "image/*") {
                add("png"); add("jpg"); add("jpeg"); add("gif"); add("webp"); add("bmp");
            } else if (tok == "audio/*") {
                add("wav"); add("mp3"); add("ogg"); add("flac");
            } else if (tok == "video/*") {
                add("webm"); add("mp4");
            } else if (auto slash = tok.find('/');
                       slash != std::string::npos && tok.substr(slash + 1) != "*") {
                add(tok.substr(slash + 1));
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return pattern;
}

} // namespace

void Dialogs::setWindow(SDL_Window* window) {
    s_window = window;
}

void Dialogs::setInteractive(bool interactive) {
    s_interactive = interactive;
}

void Dialogs::setTickCallback(TickCallback cb) {
    s_tickCb = std::move(cb);
}

void Dialogs::setPickedFiles(std::vector<std::string> paths) {
    s_queuedPicks = std::move(paths);
}

void Dialogs::setAutoDialogAnswer(bool accept) {
    s_autoAccept = accept;
}

void Dialogs::showAlert(const std::string& message) {
    if (!s_interactive) {
        LOG_INFO("[alert] %s", message.c_str());
        return;
    }
    showMessageBox(message, false);
}

bool Dialogs::showConfirm(const std::string& message) {
    if (!s_interactive) {
        LOG_INFO("[confirm] %s -> %s", message.c_str(), s_autoAccept ? "OK" : "Cancel");
        return s_autoAccept;
    }
    return showMessageBox(message, true);
}

std::optional<std::string> Dialogs::showPrompt(const std::string& message,
                                               const std::string& defaultText) {
    if (!s_interactive) {
        LOG_INFO("[prompt] %s -> %s", message.c_str(),
                 s_autoAccept ? defaultText.c_str() : "(cancelled)");
        if (!s_autoAccept) return std::nullopt;
        return defaultText;
    }
    std::string full = message;
    if (!defaultText.empty()) full += "\n\n[" + defaultText + "]";
    if (!showMessageBox(full, true)) return std::nullopt;
    return defaultText;
}

bool Dialogs::showOpenFileDialog(const std::string& filter, bool allowMultiple,
                                 std::vector<std::string>& picked, std::string& refusal) {
    const FileFilters filters = filtersFrom(filter);
    if (refusedFilters(filters, refusal)) return false;

    if (!s_interactive) {
        picked = takeQueuedPicks();
        return true;
    }

    DialogResult result;
    SDL_ShowOpenFileDialog(dialogCallback, &result, s_window, filters.data(), filters.count(),
                           nullptr, allowMultiple);
    if (!waitForDialog(result)) {
        refusal = refusalOf(result);
        return false;
    }
    picked = std::move(result.files);
    return true;
}

bool Dialogs::showOpenFolderDialog(const std::string& defaultLocation, bool allowMultiple,
                                   std::vector<std::string>& picked, std::string& refusal) {
    if (!s_interactive) {
        picked = takeQueuedPicks();
        return true;
    }

    const std::string defaultLoc = normalizeSeparators(defaultLocation);
    DialogResult result;
    SDL_ShowOpenFolderDialog(dialogCallback, &result, s_window,
                             defaultLoc.empty() ? nullptr : defaultLoc.c_str(), allowMultiple);
    if (!waitForDialog(result)) {
        refusal = refusalOf(result);
        return false;
    }
    picked = std::move(result.files);
    return true;
}

bool Dialogs::showSaveFileDialog(const std::string& filter, const std::string& defaultName,
                                 std::optional<std::string>& saved, std::string& refusal) {
    const FileFilters filters = filtersFrom(filter);
    if (refusedFilters(filters, refusal)) return false;

    const std::string defaultLoc = normalizeSeparators(defaultName);
    if (!s_interactive) {
        if (!s_autoAccept) {
            saved = std::nullopt;
            return true;
        }
        if (!s_queuedPicks.empty()) {
            saved = s_queuedPicks.front();
            s_queuedPicks.erase(s_queuedPicks.begin());
            return true;
        }
        saved = defaultLoc.empty() ? std::string("untitled") : defaultLoc;
        return true;
    }

    DialogResult result;
    SDL_ShowSaveFileDialog(dialogCallback, &result, s_window, filters.data(), filters.count(),
                           defaultLoc.empty() ? nullptr : defaultLoc.c_str());
    if (!waitForDialog(result)) {
        refusal = refusalOf(result);
        return false;
    }
    if (result.files.empty()) {
        saved = std::nullopt;
    } else {
        saved = result.files[0];
    }
    return true;
}

std::vector<std::string> Dialogs::pickFiles(const std::string& accept, bool allowMultiple) {
    const std::string pattern = patternFromAccept(accept);
    std::vector<std::string> picked;
    std::string refusal;
    if (!showOpenFileDialog(pattern.empty() ? std::string() : "Accepted files|" + pattern,
                            allowMultiple, picked, refusal)) {
        LOG_WARN("[dialog] <input type=file> picker: %s", refusal.c_str());
        return {};
    }
    return picked;
}

} // namespace bro::platform
