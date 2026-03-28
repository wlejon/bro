#include "js/runtime.h"
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

static char* module_normalize(JSContext* /*ctx*/, const char* base_name,
                              const char* name, void* /*opaque*/)
{
    // Very simple path resolution – just return a copy of the name.
    // A real implementation would resolve relative to base_name.
    (void)base_name;
    char* buf = static_cast<char*>(js_malloc_rt(JS_GetRuntime(JS_GetContextOpaque(nullptr) ? nullptr : nullptr), std::strlen(name) + 1));
    // Fallback: use C malloc since we don't have a clean rt handle here.
    buf = static_cast<char*>(malloc(std::strlen(name) + 1));
    if (buf)
        std::strcpy(buf, name);
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
    JS_SetMaxStackSize(rt_, 1024 * 1024);       // 1 MB stack

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

void Runtime::setModuleLoader()
{
    JS_SetModuleLoaderFunc(rt_, module_normalize, module_loader, nullptr);
}

bool Runtime::checkException(JSContext* ctx, JSValue val)
{
    if (!JS_IsException(val))
        return false;

    JSValue exception = JS_GetException(ctx);
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
