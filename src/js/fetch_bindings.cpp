#include "js/fetch_bindings.h"
#include "util/log.h"

#include "fetch_polyfill.js.h"

#include <quickjs.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>

namespace bro::js {

static const char* kFetchBaseKey = "__bro_fetch_base";

static std::string getFetchBasePath(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kFetchBaseKey);
    std::string result;
    if (!JS_IsUndefined(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { result = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return result;
}

static std::string resolveUrl(JSContext* ctx, const std::string& url) {
    // Strip leading ./
    std::string clean = url;
    if (clean.size() >= 2 && clean[0] == '.' && clean[1] == '/') {
        clean = clean.substr(2);
    }
    // Already absolute?
    if (clean.size() >= 2 && clean[1] == ':') return clean;
    if (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) return clean;
    // Relative — join with base
    std::string basePath = getFetchBasePath(ctx);
    if (basePath.empty()) return clean;
    if (basePath.back() != '/' && basePath.back() != '\\') basePath += '/';
    return basePath + clean;
}

// __bro_readFile(path) -> ArrayBuffer | null
static JSValue js_bro_readFile(JSContext* ctx, JSValueConst /*this_val*/,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_NULL;

    std::string urlStr(url);
    std::string path = resolveUrl(ctx, urlStr);
    JS_FreeCString(ctx, url);

    std::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_WARN("fetch: file not found: '%s' (from URL '%s')", path.c_str(), urlStr.c_str());
        return JS_NULL;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data((size_t)size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    return JS_NewArrayBufferCopy(ctx, data.data(), data.size());
}

// The JS polyfill that builds fetch/Response/Headers/TextDecoder on top of __bro_readFile

void FetchBindings::install(JSContext* ctx, const std::string& basePath) {
    // Store base path per-context in the JS global object.
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kFetchBaseKey,
                      JS_NewString(ctx, basePath.c_str()));

    // Register native __bro_readFile
    JS_SetPropertyStr(ctx, global, "__bro_readFile",
        JS_NewCFunction(ctx, js_bro_readFile, "__bro_readFile", 1));
    JS_FreeValue(ctx, global);

    // Evaluate the JS polyfill
    JSValue result = JS_Eval(ctx, js_fetch_polyfill, strlen(js_fetch_polyfill),
                             "<fetch-polyfill>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            LOG_ERROR("fetch polyfill failed: %s", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);

    LOG_INFO("Fetch API installed (local file backend)");
}

} // namespace bro::js
