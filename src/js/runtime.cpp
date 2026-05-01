#include "js/runtime.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <fstream>
#include <sstream>
#include <cstring>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Module loader helpers (file-based)
// ---------------------------------------------------------------------------

static char* module_normalize(JSContext* ctx, const char* base_name,
                              const char* name, void* /*opaque*/)
{
    // If the name is already absolute or doesn't start with '.', return as-is.
    if (!name) return nullptr;

    std::string result;
    if (name[0] == '.' && base_name) {
        // Resolve relative to the directory of the base module.
        std::string base(base_name);
        auto slash = base.find_last_of("/\\");
        if (slash != std::string::npos) {
            result = base.substr(0, slash + 1) + name;
        } else {
            result = name;
        }
    } else {
        result = name;
    }

    char* buf = static_cast<char*>(js_malloc(ctx, result.size() + 1));
    if (buf) {
        std::memcpy(buf, result.c_str(), result.size() + 1);
    }
    return buf;
}

static JSModuleDef* module_loader(JSContext* ctx, const char* module_name,
                                  void* /*opaque*/)
{
    std::ifstream file(module_name, std::ios::in | std::ios::binary);
    if (!file) {
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        return nullptr;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    JSValue func = JS_Eval(ctx, source.c_str(), source.size(), module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) {
        Runtime::checkException(ctx, func);
        return nullptr;
    }

    // js_module_set_import_meta – not needed for basic loading.
    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func));
    JS_FreeValue(ctx, func);
    return m;
}

// ---------------------------------------------------------------------------
// Runtime implementation
// ---------------------------------------------------------------------------

Runtime::Runtime()
{
    rt_ = JS_NewRuntime();
    if (!rt_) {
        LOG_ERROR("Failed to create QuickJS runtime");
        return;
    }

    // Set reasonable limits
    JS_SetMemoryLimit(rt_, 256 * 1024 * 1024); // 256 MB
    JS_SetMaxStackSize(rt_, 8 * 1024 * 1024);    // 8 MB stack (Vue template compiler recurses deeply)

    // Break out of long-running JS on Ctrl+C (covers main thread + each Worker,
    // since workers construct their own Runtime on their thread).
    bro::util::installJsInterruptHandler(rt_);

#ifndef NDEBUG
    // Print leaked GC objects + atoms before JS_FreeRuntime asserts on shutdown.
    // Requires ENABLE_DUMPS in the qjs build (set in third_party/CMakeLists.txt
    // for Debug). No-op in Release.
    JS_SetDumpFlags(rt_, JS_DUMP_LEAKS | JS_DUMP_ATOM_LEAKS);
#endif

    ctx_ = JS_NewContext(rt_);
    if (!ctx_) {
        LOG_ERROR("Failed to create QuickJS context");
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
        return;
    }
}

Runtime::~Runtime()
{
    if (ctx_) {
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }
}

bool Runtime::eval(const std::string& code, const std::string& filename)
{
    JSValue result = JS_Eval(ctx_, code.c_str(), code.size(),
                             filename.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (checkException(ctx_, result)) {
        return false;
    }
    JS_FreeValue(ctx_, result);
    return true;
}

bool Runtime::evalModule(const std::string& code, const std::string& filename)
{
    JSValue func = JS_Eval(ctx_, code.c_str(), code.size(),
                           filename.c_str(),
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (checkException(ctx_, func)) {
        return false;
    }

    JSValue result = JS_EvalFunction(ctx_, func);
    if (checkException(ctx_, result)) {
        return false;
    }
    JS_FreeValue(ctx_, result);
    return true;
}

bool Runtime::loadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        LOG_ERROR("Failed to open file: %s", path.c_str());
        return false;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return eval(ss.str(), path);
}

JSValue Runtime::getGlobalObject() const
{
    return JS_GetGlobalObject(ctx_);
}

void Runtime::executePendingJobs()
{
    JSContext* pctx = nullptr;
    while (JS_ExecutePendingJob(rt_, &pctx) > 0) {
        // keep draining
    }
}

JSContext* Runtime::createContext()
{
    if (!rt_) return nullptr;
    return JS_NewContext(rt_);
}

void Runtime::setModuleLoader()
{
    JS_SetModuleLoaderFunc(rt_, module_normalize, module_loader, nullptr);
}

bool Runtime::checkException(JSContext* ctx, JSValue val)
{
    if (!JS_IsException(val))
        return false;

    JSValue exception = JS_GetException(ctx);

    // Filter out our own interrupt-handler-induced exceptions. When the
    // QuickJS interrupt callback returns non-zero (Ctrl+C, worker shutdown,
    // engine teardown), the in-flight JS_Call is aborted with an
    // InternalError whose message is exactly "interrupted". That's our
    // intended termination path, not a script bug — logging it floods the
    // console on every clean shutdown. No realistic user code throws an
    // InternalError with that exact message, so identifying it by name +
    // message is reliable in practice.
    if (JS_IsObject(exception)) {
        JSValue nameVal = JS_GetPropertyStr(ctx, exception, "name");
        JSValue msgVal  = JS_GetPropertyStr(ctx, exception, "message");
        const char* name = JS_ToCString(ctx, nameVal);
        const char* msg  = JS_ToCString(ctx, msgVal);
        bool isInterrupt = name && msg
            && std::strcmp(name, "InternalError") == 0
            && std::strcmp(msg, "interrupted") == 0;
        if (name) JS_FreeCString(ctx, name);
        if (msg)  JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, nameVal);
        JS_FreeValue(ctx, msgVal);
        if (isInterrupt) {
            JS_FreeValue(ctx, exception);
            return true;
        }
    }

    const char* str = JS_ToCString(ctx, exception);
    if (str) {
        LOG_ERROR("JS Exception: %s", str);
        JS_FreeCString(ctx, str);
    }

    // Try to get a stack trace
    if (JS_IsObject(exception)) {
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            const char* stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                LOG_ERROR("Stack:\n%s", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
    }

    JS_FreeValue(ctx, exception);
    return true;
}

} // namespace bro::js
