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

// Store the SDL window so the dialog can be modal to it
static SDL_Window* s_window = nullptr;
static DialogBindings::TickCallback s_tickCb;

// ---------------------------------------------------------------------------
// Path separator normalization
// ---------------------------------------------------------------------------

/// Windows' shell path parser (SHCreateItemFromParsingName — used by SDL's
/// modern IFileDialog backend to honour a default location) only accepts
/// backslash separators. A forward slash makes it fail, which silently drops
/// SDL to its legacy SHBrowseForFolder/GetOpenFileName dialog. Normalize any
/// path handed to SDL so the modern themed dialog is actually used.
static std::string normalizeSeparators(std::string s)
{
#ifdef _WIN32
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return s;
}

// ---------------------------------------------------------------------------
// Dialog result — written by the SDL dialog thread, read by the main thread
// ---------------------------------------------------------------------------

// SDL's Windows dialog backend can invoke the callback TWICE for one request:
// when the modern IFileDialog path fails it reports the error through the
// callback (filelist == NULL) and then falls back to the legacy dialog, which
// invokes the callback a second time with the real result. The waiter must not
// return — and destroy this stack object — while another callback is still
// pending against it, or the second callback is a use-after-free.
//
// SDL issues at most two callbacks per request: exactly one for the modern
// attempt, plus one for the legacy fallback if the modern attempt failed.
// Every non-Windows backend invokes the callback exactly once.
#ifdef _WIN32
static constexpr int kMaxDialogCallbacks = 2;
#else
static constexpr int kMaxDialogCallbacks = 1;
#endif

struct DialogResult {
    std::atomic<bool> haveResult{false};  // a non-error callback delivered a path list
    std::atomic<int>  callbackCount{0};   // number of times SDL invoked the callback
    std::vector<std::string> files;       // written before haveResult release-store
};

static void dialogCallback(void* userdata, const char* const* filelist, int /*filter*/)
{
    auto* result = static_cast<DialogResult*>(userdata);
    // filelist == NULL is SDL signalling an error for *this* attempt. When the
    // modern dialog errors this way SDL still falls back to the legacy dialog,
    // so a NULL callback is not the final word — only a non-NULL filelist
    // carries the actual result (an empty {NULL} list means the user cancelled).
    if (filelist) {
        for (const char* const* p = filelist; *p; ++p) {
            result->files.emplace_back(*p);
        }
        result->haveResult.store(true, std::memory_order_release);
    }
    // Published last: a waiter observing this increment is guaranteed to see
    // every preceding write from this invocation.
    result->callbackCount.fetch_add(1, std::memory_order_release);
}

/// Spin-wait for the dialog result while keeping SDL's event loop alive
/// and ticking JS timers so audio sequencer / timers keep running.
///
/// Returns once a real result has landed, or once every callback SDL can
/// issue has landed (so nothing more can touch `result` after we return).
static void waitForDialog(DialogResult& result)
{
    while (!result.haveResult.load(std::memory_order_acquire) &&
           result.callbackCount.load(std::memory_order_acquire) < kMaxDialogCallbacks) {
        SDL_PumpEvents();
        if (s_tickCb) s_tickCb();
        SDL_Delay(8);
    }
}

// ---------------------------------------------------------------------------
// showOpenFileDialog(filters, allowMultiple)
//   filters: optional string like "Audio Files|wav;mp3;ogg" or null
//   allowMultiple: optional boolean
//   Returns: array of file paths, or empty array if cancelled
// ---------------------------------------------------------------------------

static JSValue js_showOpenFileDialog(JSContext* ctx, JSValueConst /*this_val*/,
                                     int argc, JSValueConst* argv)
{
    // Parse optional filter string: "Label|ext1;ext2"
    std::string filterStr;
    bool allowMany = false;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { filterStr = s; JS_FreeCString(ctx, s); }
    }
    if (argc >= 2) {
        allowMany = JS_ToBool(ctx, argv[1]);
    }

    // Build SDL filter
    SDL_DialogFileFilter sdlFilter;
    bool hasFilter = false;
    std::string filterName, filterPattern;
    if (!filterStr.empty()) {
        auto pos = filterStr.find('|');
        if (pos != std::string::npos) {
            filterName = filterStr.substr(0, pos);
            filterPattern = filterStr.substr(pos + 1);
        } else {
            filterName = "Files";
            filterPattern = filterStr;
        }
        sdlFilter.name = filterName.c_str();
        sdlFilter.pattern = filterPattern.c_str();
        hasFilter = true;
    }

    DialogResult result;
    SDL_ShowOpenFileDialog(dialogCallback, &result, s_window,
                           hasFilter ? &sdlFilter : nullptr,
                           hasFilter ? 1 : 0,
                           nullptr, allowMany);

    waitForDialog(result);

    // Return JS array of paths
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < result.files.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, result.files[i].c_str()));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// showOpenFolderDialog(defaultLocation, allowMultiple)
//   defaultLocation: optional starting folder path, or null
//   allowMultiple: optional boolean
//   Returns: array of folder paths, or empty array if cancelled
// ---------------------------------------------------------------------------

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

    DialogResult result;
    SDL_ShowOpenFolderDialog(dialogCallback, &result, s_window,
                             defaultLoc.empty() ? nullptr : defaultLoc.c_str(),
                             allowMany);

    waitForDialog(result);

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < result.files.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, result.files[i].c_str()));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// showSaveFileDialog(filters, defaultName)
//   Returns: file path string, or null if cancelled
// ---------------------------------------------------------------------------

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

    SDL_DialogFileFilter sdlFilter;
    bool hasFilter = false;
    std::string filterName, filterPattern;
    if (!filterStr.empty()) {
        auto pos = filterStr.find('|');
        if (pos != std::string::npos) {
            filterName = filterStr.substr(0, pos);
            filterPattern = filterStr.substr(pos + 1);
        } else {
            filterName = "Files";
            filterPattern = filterStr;
        }
        sdlFilter.name = filterName.c_str();
        sdlFilter.pattern = filterPattern.c_str();
        hasFilter = true;
    }

    DialogResult result;
    SDL_ShowSaveFileDialog(dialogCallback, &result, s_window,
                            hasFilter ? &sdlFilter : nullptr,
                            hasFilter ? 1 : 0,
                            defaultLoc.empty() ? nullptr : defaultLoc.c_str());

    waitForDialog(result);

    if (result.files.empty()) return JS_NULL;
    return JS_NewString(ctx, result.files[0].c_str());
}

// ---------------------------------------------------------------------------
// alert / confirm / prompt
//
// The three modal dialogs every page assumes exist. Without them a plain
// `confirm('Are you sure?')` is a ReferenceError, which takes out the whole
// handler around it — "New document", "Clear history" and every "load this
// example, you'll lose your work" path in a typical editor.
//
// Windowed: SDL's native message box, which is modal and blocking, exactly
// like the browser's. Nothing else can run meanwhile — a page that alerts in
// a loop freezes itself, as it does in a browser.
//
// Headless/server: there is no one to answer, and blocking would hang a test
// run forever. The message is logged and the dialog answers itself; see
// setAutoDialogAnswer().
// ---------------------------------------------------------------------------

static bool s_interactive = true;
static bool s_autoAccept = true;

// Message-box button IDs.
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

/// Show a native modal box. Returns true for OK/accept.
/// `withCancel` false makes it a one-button acknowledgement (alert).
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
        // No message box available (no video backend, or the platform
        // refused): treat it as a dismissal rather than blocking.
        LOG_WARN("[dialog] message box unavailable: %s", SDL_GetError());
        return !withCancel;
    }
    return pressed == kBtnOk;
}

// The engine-level trio. The JS bindings below are wrappers; so is the bronze
// host layer's (src/bronze_host/host_platform.cpp). One implementation, so
// `s_interactive` and `s_autoAccept` mean the same thing to both.
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
    // SDL has no native text-entry dialog, so the box states the value that OK
    // will return and the user chooses between it and cancelling. An app that
    // needs real text entry should draw its own field — everything it takes
    // (input, focus, overlay) is already in the engine.
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

// ---------------------------------------------------------------------------
// pickFiles — the picker behind <input type=file>
// ---------------------------------------------------------------------------

static std::vector<std::string> s_queuedPicks;

void DialogBindings::setPickedFiles(std::vector<std::string> paths)
{
    s_queuedPicks = std::move(paths);
}

std::vector<std::string> DialogBindings::pickFiles(const std::string& accept,
                                                   bool allowMultiple)
{
    if (!s_interactive) {
        // Nobody to ask: hand back whatever the script queued, once, so a
        // headless run can drive a file input the way a user would.
        std::vector<std::string> picked;
        picked.swap(s_queuedPicks);
        return picked;
    }

    // The HTML `accept` attribute is a comma-separated list of extensions
    // (".obj"), MIME types ("image/png") and wildcards ("image/*"). SDL wants
    // a semicolon-separated extension pattern, so keep the extensions, expand
    // the handful of wildcards worth expanding, and drop the rest — a filter
    // that cannot be expressed is better shown as "all files" than as one
    // that hides the file the user came for.
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
        // trim
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
                add(tok.substr(slash + 1));   // "image/png" -> "png"
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

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void DialogBindings::setAutoDialogAnswer(bool accept)
{
    s_autoAccept = accept;
}

void DialogBindings::install(JSContext* ctx, SDL_Window* window,
                             TickCallback tickCb, bool interactive)
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
