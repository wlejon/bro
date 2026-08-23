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
};

static void dialogCallback(void* userdata, const char* const* filelist, int /*filter*/)
{
    auto* result = static_cast<DialogResult*>(userdata);
    if (filelist) {
        for (const char* const* p = filelist; *p; ++p) {
            result->files.emplace_back(*p);
        }
        result->haveResult.store(true, std::memory_order_release);
    }
    result->callbackCount.fetch_add(1, std::memory_order_release);
}

static void waitForDialog(DialogResult& result)
{
    while (!result.haveResult.load(std::memory_order_acquire) &&
           result.callbackCount.load(std::memory_order_acquire) < kMaxDialogCallbacks) {
        SDL_PumpEvents();
        if (s_tickCb) s_tickCb();
        SDL_Delay(8);
    }
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

static bool s_interactive = true;
static bool s_autoAccept = true;

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

static std::vector<std::string> s_queuedPicks;

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
