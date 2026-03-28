#include "js/console.h"
#include "util/log.h"

#include <string>
#include <sstream>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string argsToString(JSContext* ctx, int argc, JSValueConst* argv)
{
    std::ostringstream oss;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) oss << ' ';
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            oss << str;
            JS_FreeCString(ctx, str);
        } else {
            oss << "[object]";
        }
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// console methods
// ---------------------------------------------------------------------------

static JSValue js_console_log(JSContext* ctx, JSValueConst /*this_val*/,
                              int argc, JSValueConst* argv)
{
    std::string msg = argsToString(ctx, argc, argv);
    LOG_INFO("[console.log] %s", msg.c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_info(JSContext* ctx, JSValueConst /*this_val*/,
                               int argc, JSValueConst* argv)
{
    std::string msg = argsToString(ctx, argc, argv);
    LOG_INFO("[console.info] %s", msg.c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_debug(JSContext* ctx, JSValueConst /*this_val*/,
                                int argc, JSValueConst* argv)
{
    std::string msg = argsToString(ctx, argc, argv);
    LOG_INFO("[console.debug] %s", msg.c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_warn(JSContext* ctx, JSValueConst /*this_val*/,
                               int argc, JSValueConst* argv)
{
    std::string msg = argsToString(ctx, argc, argv);
    LOG_WARN("[console.warn] %s", msg.c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_error(JSContext* ctx, JSValueConst /*this_val*/,
                                int argc, JSValueConst* argv)
{
    std::string msg = argsToString(ctx, argc, argv);
    LOG_ERROR("[console.error] %s", msg.c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_clear(JSContext* /*ctx*/, JSValueConst /*this_val*/,
                                int /*argc*/, JSValueConst* /*argv*/)
{
    LOG_INFO("[console.clear]");
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void Console::install(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 0));
    JS_SetPropertyStr(ctx, console, "info",
                      JS_NewCFunction(ctx, js_console_info, "info", 0));
    JS_SetPropertyStr(ctx, console, "debug",
                      JS_NewCFunction(ctx, js_console_debug, "debug", 0));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, js_console_warn, "warn", 0));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_error, "error", 0));
    JS_SetPropertyStr(ctx, console, "clear",
                      JS_NewCFunction(ctx, js_console_clear, "clear", 0));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
