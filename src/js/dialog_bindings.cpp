#include "js/dialog_bindings.h"
#include "util/log.h"
#include <qjsbind/qjsbind.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <string>
#include <vector>

namespace bro::js {

static SDL_Window* s_window = nullptr;
static DialogBindings::TickCallback s_tickCb;
static bool s_interactive = true;
static bool s_autoAccept = true;
static std::vector<std::string> s_queuedPicks;

static std::string normalizeSeparators(std::string s)
{
#ifdef _WIN32
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return s;
}

#ifdef _WIN32
static constexpr int kMaxDialogCallbacks = 2;
#else
static constexpr int kMaxDialogCallbacks = 1;
#endif

struct DialogResult {
    std::atomic<bool> haveResult{false};
    std::atomic<int>  callbackCount{0};
    std::vector<std::string> files;
    /// Why the dialog was refused, read out of SDL on the thread that set it.
    ///
    /// SDL's error is thread-local and the refusal that matters here — a filter
    /// SDL will not accept — is delivered synchronously from
    /// `SDL_ShowOpenFileDialog` itself, but a backend may refuse from a thread
    /// of its own. Taking it here rather than after the wait is the only
    /// spelling that is right in both cases.
    std::string error;
};

static void dialogCallback(void* userdata, const char* const* filelist, int /*filter*/)
{
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

/// Read a filter string into SDL's filter array.
///
/// `"Images|png;jpg"` is one filter and `"Documents|json|Media|mp4;mkv|All
/// files|*"` is three: names and patterns alternating, which is the spelling
/// every native file dialog has taken since the eighties and the one people
/// write without being told to. This used to split at the **first** `|` and
/// keep the rest as one pattern, which put the second filter's name inside the
/// first filter's extensions — and SDL validates a pattern *before* it opens
/// anything (`[a-zA-Z0-9_.-]`, `;` between extensions, or a bare `*`, and
/// nothing else). So a caller offering more than one filter was refused, and a
/// refused dialog is one that never appears: the press did nothing at all.
///
/// A pattern with no `|` in it at all keeps the old reading — it is the
/// pattern, and the filter is called "Files". A trailing name with no pattern
/// is dropped rather than guessed at.
struct FileFilters {
    std::vector<std::string> parts;          // name, pattern, name, pattern, …
    std::vector<SDL_DialogFileFilter> list;  // built once `parts` has stopped growing

    const SDL_DialogFileFilter* data() const { return list.empty() ? nullptr : list.data(); }
    int count() const { return static_cast<int>(list.size()); }
};

static FileFilters filtersFrom(const std::string& filterStr)
{
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

/// How long a dialog that has already answered once is given to answer again.
///
/// See `waitForDialog`. Any second callback is delivered from the same place as
/// the first and arrives immediately; this is a bound, not a poll interval.
static constexpr Uint64 kExtraCallbackGraceMs = 500;

/// Block until the dialog has answered, keeping timers running while it is up.
///
/// **An error answers once, and waiting for a second answer that is not coming
/// is a window that never comes back.** SDL calls back with a null `filelist`
/// when it refuses the request — most easily by handing it a filter pattern
/// with anything but `[a-zA-Z0-9_.-]` in it, which it validates *before* it
/// opens anything — and on Windows this loop wanted two callbacks before it
/// would return. One bad filter string therefore hung the application with its
/// last frame on the screen and no dialog to close: it went on pumping events
/// and ticking timers for ever, which is why it looked like a freeze rather
/// than a crash and why nothing anywhere said what had happened.
///
/// The second callback is still waited for, because the thing it was guarding
/// is real: `DialogResult` lives on the caller's stack, and a callback arriving
/// after this returns would write into a frame that is gone. So the wait is
/// *bounded* instead of abandoned, and the reason is logged — an application
/// that has been refused should be able to find out why.
///
/// Answers false when it was refused, which is **not** the same as cancelled:
/// SDL hands over an empty list for a cancel and a null one for a refusal, and
/// reporting both as "no files" is what let a broken filter look to the
/// application exactly like a person pressing Escape.
static bool waitForDialog(DialogResult& result)
{
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

/// The refusal as a JS exception.
///
/// A file dialog that will not open is the one thing a caller cannot see for
/// itself — the return is an empty list either way — so it is thrown rather
/// than returned, and it carries SDL's own sentence because the caller wrote
/// the filter SDL is objecting to.
static JSValue throwRefusal(JSContext* ctx, const DialogResult& result)
{
    return JS_ThrowTypeError(ctx, "file dialog refused: %s",
                             result.error.empty() ? "unknown reason"
                                                  : result.error.c_str());
}

static JSValue js_showOpenFileDialog(JSContext* ctx, JSValueConst /*this_val*/,
                                     int argc, JSValueConst* argv)
{
    std::string filterStr;
    bool allowMany = false;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { filterStr = s; JS_FreeCString(ctx, s); }
    }
    if (argc >= 2) {
        allowMany = JS_ToBool(ctx, argv[1]);
    }

    if (!s_interactive) {
        std::vector<std::string> picked;
        picked.swap(s_queuedPicks);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < picked.size(); i++) {
            JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                                 JS_NewString(ctx, picked[i].c_str()));
        }
        return arr;
    }

    const FileFilters filters = filtersFrom(filterStr);

    DialogResult result;
    SDL_ShowOpenFileDialog(dialogCallback, &result, s_window,
                           filters.data(), filters.count(),
                           nullptr, allowMany);

    if (!waitForDialog(result)) return throwRefusal(ctx, result);

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < result.files.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, result.files[i].c_str()));
    }
    return arr;
}

static JSValue js_showOpenFolderDialog(JSContext* ctx, JSValueConst /*this_val*/,
                                       int argc, JSValueConst* argv)
{
    std::string defaultLoc;
    bool allowMany = false;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { defaultLoc = normalizeSeparators(s); JS_FreeCString(ctx, s); }
    }
    if (argc >= 2) {
        allowMany = JS_ToBool(ctx, argv[1]);
    }

    if (!s_interactive) {
        std::vector<std::string> picked;
        picked.swap(s_queuedPicks);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < picked.size(); i++) {
            JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                                 JS_NewString(ctx, picked[i].c_str()));
        }
        return arr;
    }

    DialogResult result;
    SDL_ShowOpenFolderDialog(dialogCallback, &result, s_window,
                             defaultLoc.empty() ? nullptr : defaultLoc.c_str(),
                             allowMany);

    if (!waitForDialog(result)) return throwRefusal(ctx, result);

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < result.files.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, result.files[i].c_str()));
    }
    return arr;
}

static JSValue js_showSaveFileDialog(JSContext* ctx, JSValueConst /*this_val*/,
                                     int argc, JSValueConst* argv)
{
    std::string filterStr;
    std::string defaultLoc;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { filterStr = s; JS_FreeCString(ctx, s); }
    }
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) { defaultLoc = normalizeSeparators(s); JS_FreeCString(ctx, s); }
    }

    if (!s_interactive) {
        if (!s_autoAccept) return JS_NULL;
        if (!s_queuedPicks.empty()) {
            std::string p = s_queuedPicks.front();
            s_queuedPicks.erase(s_queuedPicks.begin());
            return JS_NewString(ctx, p.c_str());
        }
        std::string p = defaultLoc.empty() ? "untitled" : defaultLoc;
        return JS_NewString(ctx, p.c_str());
    }

    const FileFilters filters = filtersFrom(filterStr);

    DialogResult result;
    SDL_ShowSaveFileDialog(dialogCallback, &result, s_window,
                            filters.data(), filters.count(),
                            defaultLoc.empty() ? nullptr : defaultLoc.c_str());

    if (!waitForDialog(result)) return throwRefusal(ctx, result);

    if (result.files.empty()) return JS_NULL;
    return JS_NewString(ctx, result.files[0].c_str());
}

enum : int { kBtnCancel = 0, kBtnOk = 1 };

static std::string argToString(JSContext* ctx, JSValueConst v)
{
    if (JS_IsUndefined(v)) return "";
    const char* s = JS_ToCString(ctx, v);
    if (!s) return "";
    std::string out(s);
    JS_FreeCString(ctx, s);
    return out;
}

static bool showMessageBox(const std::string& message, bool withCancel)
{
    SDL_MessageBoxButtonData buttons[2];
    int nButtons = 0;
    if (withCancel) {
        buttons[nButtons++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
                               kBtnCancel, "Cancel"};
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

void DialogBindings::showAlert(const std::string& message)
{
    if (!s_interactive) {
        LOG_INFO("[alert] %s", message.c_str());
        return;
    }
    showMessageBox(message, false);
}

bool DialogBindings::showConfirm(const std::string& message)
{
    if (!s_interactive) {
        LOG_INFO("[confirm] %s -> %s", message.c_str(),
                 s_autoAccept ? "OK" : "Cancel");
        return s_autoAccept;
    }
    return showMessageBox(message, true);
}

std::optional<std::string> DialogBindings::showPrompt(const std::string& message,
                                                      const std::string& defaultText)
{
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

static JSValue js_alert(JSContext* ctx, JSValueConst /*this_val*/,
                        int argc, JSValueConst* argv)
{
    DialogBindings::showAlert(argc >= 1 ? argToString(ctx, argv[0]) : "");
    return JS_UNDEFINED;
}

static JSValue js_confirm(JSContext* ctx, JSValueConst /*this_val*/,
                          int argc, JSValueConst* argv)
{
    return JS_NewBool(ctx, DialogBindings::showConfirm(
        argc >= 1 ? argToString(ctx, argv[0]) : ""));
}

static JSValue js_prompt(JSContext* ctx, JSValueConst /*this_val*/,
                         int argc, JSValueConst* argv)
{
    std::optional<std::string> answer = DialogBindings::showPrompt(
        argc >= 1 ? argToString(ctx, argv[0]) : "",
        argc >= 2 ? argToString(ctx, argv[1]) : "");
    if (!answer) return JS_NULL;
    return JS_NewString(ctx, answer->c_str());
}

void DialogBindings::setPickedFiles(std::vector<std::string> paths)
{
    s_queuedPicks = std::move(paths);
}

std::vector<std::string> DialogBindings::pickFiles(const std::string& accept,
                                                   bool allowMultiple)
{
    if (!s_interactive) {
        std::vector<std::string> picked;
        picked.swap(s_queuedPicks);
        return picked;
    }

    std::string pattern;
    auto add = [&pattern](const std::string& ext) {
        if (ext.empty()) return;
        if (!pattern.empty()) pattern += ';';
        pattern += ext;
    };
    size_t start = 0;
    while (start <= accept.size() && !accept.empty()) {
        size_t comma = accept.find(',', start);
        std::string tok = accept.substr(start, comma == std::string::npos
                                                   ? std::string::npos
                                                   : comma - start);
        while (!tok.empty() && isspace((unsigned char)tok.front())) tok.erase(tok.begin());
        while (!tok.empty() && isspace((unsigned char)tok.back())) tok.pop_back();
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

    SDL_DialogFileFilter sdlFilter;
    bool hasFilter = !pattern.empty();
    if (hasFilter) {
        sdlFilter.name = "Accepted files";
        sdlFilter.pattern = pattern.c_str();
    }

    DialogResult result;
    SDL_ShowOpenFileDialog(dialogCallback, &result, s_window,
                           hasFilter ? &sdlFilter : nullptr,
                           hasFilter ? 1 : 0,
                           nullptr, allowMultiple);
    waitForDialog(result);
    return std::move(result.files);
}

void DialogBindings::setAutoDialogAnswer(bool accept)
{
    s_autoAccept = accept;
}

void DialogBindings::install(JSContext* ctx, SDL_Window* window, TickCallback tickCb, bool interactive)
{
    s_window = window;
        s_tickCb = std::move(tickCb);
        s_interactive = interactive;

        qjsbind::Global(ctx)
            .function("showOpenFileDialog",   js_showOpenFileDialog,   0)
            .function("showOpenFolderDialog", js_showOpenFolderDialog, 0)
            .function("showSaveFileDialog",   js_showSaveFileDialog,   0)
            .function("alert",                js_alert,                1)
            .function("confirm",              js_confirm,              1)
            .function("prompt",               js_prompt,               2);
}


} // namespace bro::js
