#include "js/storage_bindings.h"
#include "util/log.h"
#include "util/storage_file.h"

#include <qjsbind/qjsbind.h>

#include <map>
#include <string>

namespace bro::js {

// ---------------------------------------------------------------------------
// Per-context storage state (heap-allocated, pointer stashed in JS global)
// ---------------------------------------------------------------------------

struct StorageState {
    std::map<std::string, std::string> storage;
    std::string storagePath;
};

static const char* kStorageKey = "__bro_storage_ptr";

static StorageState* getStorageState(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kStorageKey);
    StorageState* state = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        state = reinterpret_cast<StorageState*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return state;
}

// The file format — real JSON, written atomically — lives in
// util/storage_file.cpp, shared with the bronze host's Storage.
static void loadStorage(StorageState* state)
{
    state->storage.clear();
    if (state->storagePath.empty()) return;
    util::readStorageFile(state->storagePath, state->storage);
}

static void saveStorage(StorageState* state)
{
    if (!state || state->storagePath.empty()) return;
    if (!util::writeStorageFile(state->storagePath, state->storage))
        LOG_WARN("localStorage: could not write %s", state->storagePath.c_str());
}

// ---------------------------------------------------------------------------
// JS functions
// ---------------------------------------------------------------------------

static std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string r = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return r;
}

static JSValue js_getItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    auto* state = getStorageState(ctx);
    if (!state) return JS_NULL;
    std::string key = jsStr(ctx, argv[0]);
    auto it = state->storage.find(key);
    if (it == state->storage.end()) return JS_NULL;
    return JS_NewString(ctx, it->second.c_str());
}

static JSValue js_setItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getStorageState(ctx);
    if (!state) return JS_UNDEFINED;
    std::string key = jsStr(ctx, argv[0]);
    std::string val = jsStr(ctx, argv[1]);
    state->storage[key] = val;
    saveStorage(state);
    return JS_UNDEFINED;
}

static JSValue js_removeItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* state = getStorageState(ctx);
    if (!state) return JS_UNDEFINED;
    std::string key = jsStr(ctx, argv[0]);
    state->storage.erase(key);
    saveStorage(state);
    return JS_UNDEFINED;
}

static JSValue js_clear(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getStorageState(ctx);
    if (!state) return JS_UNDEFINED;
    state->storage.clear();
    saveStorage(state);
    return JS_UNDEFINED;
}

static JSValue js_key(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    auto* state = getStorageState(ctx);
    if (!state) return JS_NULL;
    int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    if (index < 0 || static_cast<size_t>(index) >= state->storage.size()) return JS_NULL;
    auto it = state->storage.begin();
    std::advance(it, index);
    return JS_NewString(ctx, it->first.c_str());
}

static JSValue js_get_length(JSContext* ctx, JSValueConst) {
    auto* state = getStorageState(ctx);
    if (!state) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, static_cast<int32_t>(state->storage.size()));
}

static const JSCFunctionListEntry js_storage_funcs[] = {
    JS_CFUNC_DEF("getItem", 1, js_getItem),
    JS_CFUNC_DEF("setItem", 2, js_setItem),
    JS_CFUNC_DEF("removeItem", 1, js_removeItem),
    JS_CFUNC_DEF("clear", 0, js_clear),
    JS_CFUNC_DEF("key", 1, js_key),
    JS_CGETSET_DEF("length", js_get_length, nullptr),
};

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void StorageBindings::install(JSContext* ctx, const std::string& storagePath)
{
    auto* state = new StorageState();
    state->storagePath = storagePath;
    loadStorage(state);

    qjsbind::Global(ctx)
        .value(kStorageKey, JS_NewInt64(ctx, static_cast<int64_t>(
                                reinterpret_cast<intptr_t>(state))));

    qjsbind::Namespace(ctx, "localStorage")
        .function_list(js_storage_funcs,
                       sizeof(js_storage_funcs) / sizeof(js_storage_funcs[0]));
}

// ---------------------------------------------------------------------------
// sessionStorage — same API, in-memory only (no persistence)
// ---------------------------------------------------------------------------

static const char* kSessionStorageKey = "__bro_session_storage_ptr";

static StorageState* getSessionStorageState(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kSessionStorageKey);
    StorageState* state = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        state = reinterpret_cast<StorageState*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return state;
}

static JSValue js_ss_getItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    auto* state = getSessionStorageState(ctx);
    if (!state) return JS_NULL;
    std::string key = jsStr(ctx, argv[0]);
    auto it = state->storage.find(key);
    if (it == state->storage.end()) return JS_NULL;
    return JS_NewString(ctx, it->second.c_str());
}

static JSValue js_ss_setItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getSessionStorageState(ctx);
    if (!state) return JS_UNDEFINED;
    state->storage[jsStr(ctx, argv[0])] = jsStr(ctx, argv[1]);
    return JS_UNDEFINED;
}

static JSValue js_ss_removeItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* state = getSessionStorageState(ctx);
    if (!state) return JS_UNDEFINED;
    state->storage.erase(jsStr(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_ss_clear(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getSessionStorageState(ctx);
    if (!state) return JS_UNDEFINED;
    state->storage.clear();
    return JS_UNDEFINED;
}

static JSValue js_ss_key(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    auto* state = getSessionStorageState(ctx);
    if (!state) return JS_NULL;
    int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    if (index < 0 || static_cast<size_t>(index) >= state->storage.size()) return JS_NULL;
    auto it = state->storage.begin();
    std::advance(it, index);
    return JS_NewString(ctx, it->first.c_str());
}

static JSValue js_ss_get_length(JSContext* ctx, JSValueConst) {
    auto* state = getSessionStorageState(ctx);
    if (!state) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, static_cast<int32_t>(state->storage.size()));
}

static const JSCFunctionListEntry js_session_storage_funcs[] = {
    JS_CFUNC_DEF("getItem", 1, js_ss_getItem),
    JS_CFUNC_DEF("setItem", 2, js_ss_setItem),
    JS_CFUNC_DEF("removeItem", 1, js_ss_removeItem),
    JS_CFUNC_DEF("clear", 0, js_ss_clear),
    JS_CFUNC_DEF("key", 1, js_ss_key),
    JS_CGETSET_DEF("length", js_ss_get_length, nullptr),
};

void StorageBindings::installSessionStorage(JSContext* ctx)
{
    auto* state = new StorageState(); // no storagePath → in-memory only

    qjsbind::Global(ctx)
        .value(kSessionStorageKey, JS_NewInt64(ctx, static_cast<int64_t>(
                                       reinterpret_cast<intptr_t>(state))));

    qjsbind::Namespace(ctx, "sessionStorage")
        .function_list(js_session_storage_funcs,
                       sizeof(js_session_storage_funcs) / sizeof(js_session_storage_funcs[0]));
}

void StorageBindings::cleanup(JSContext* ctx)
{
    auto* state = getStorageState(ctx);
    delete state;
    auto* ssState = getSessionStorageState(ctx);
    delete ssState;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kStorageKey, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, global, kSessionStorageKey, JS_UNDEFINED);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
