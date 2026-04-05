#include "js/dialog_bindings.h"
#include "util/log.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <string>
#include <vector>
#include <mutex>

namespace bro::js {

// Store the SDL window so the dialog can be modal to it
static SDL_Window* s_window = nullptr;
static DialogBindings::TickCallback s_tickCb;

// ---------------------------------------------------------------------------
// Dialog result — written by callback thread, read by main thread
// ---------------------------------------------------------------------------

struct DialogResult {
    std::mutex mtx;
    std::atomic<bool> ready{false};
    std::vector<std::string> files;
};

static void dialogCallback(void* userdata, const char* const* filelist, int /*filter*/)
{
    auto* result = static_cast<DialogResult*>(userdata);
    std::lock_guard<std::mutex> lock(result->mtx);
    if (filelist) {
        for (const char* const* p = filelist; *p; ++p) {
            result->files.emplace_back(*p);
        }
    }
    result->ready.store(true, std::memory_order_release);
}

/// Spin-wait for the dialog result while keeping SDL's event loop alive
/// and ticking JS timers so audio sequencer / timers keep running.
static void waitForDialog(DialogResult& result)
{
    while (!result.ready.load(std::memory_order_acquire)) {
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
        if (s) { defaultLoc = s; JS_FreeCString(ctx, s); }
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

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "showOpenFileDialog",
        JS_NewCFunction(ctx, js_showOpenFileDialog, "showOpenFileDialog", 0));
    JS_SetPropertyStr(ctx, global, "showSaveFileDialog",
        JS_NewCFunction(ctx, js_showSaveFileDialog, "showSaveFileDialog", 0));
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
