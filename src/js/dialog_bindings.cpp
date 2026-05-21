#include "js/dialog_bindings.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <SDL3/SDL_dialog.h>
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
// Install
// ---------------------------------------------------------------------------

void DialogBindings::install(JSContext* ctx, SDL_Window* window,
                             TickCallback tickCb)
{
    s_window = window;
    s_tickCb = std::move(tickCb);

    qjsbind::Global(ctx)
        .function("showOpenFileDialog",   js_showOpenFileDialog,   0)
        .function("showOpenFolderDialog", js_showOpenFolderDialog, 0)
        .function("showSaveFileDialog",   js_showSaveFileDialog,   0);
}

} // namespace bro::js
