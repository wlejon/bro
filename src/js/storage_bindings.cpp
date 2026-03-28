#include "js/storage_bindings.h"
#include "util/log.h"

#include <fstream>
#include <sstream>
#include <map>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Simple JSON-backed key-value store
// ---------------------------------------------------------------------------

static std::map<std::string, std::string> s_storage;
static std::string s_storagePath;

static void loadStorage()
{
    s_storage.clear();
    if (s_storagePath.empty()) return;

    std::ifstream file(s_storagePath);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // Minimal JSON object parser: {"key":"value","key2":"value2"}
    size_t pos = content.find('{');
    if (pos == std::string::npos) return;
    pos++;

    auto parseString = [&](size_t& p) -> std::string {
        if (p >= content.size() || content[p] != '"') return "";
        p++; // skip opening quote
        std::string result;
        while (p < content.size() && content[p] != '"') {
            if (content[p] == '\\' && p + 1 < content.size()) {
                p++;
                switch (content[p]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    default: result += content[p]; break;
                }
            } else {
                result += content[p];
            }
            p++;
        }
        if (p < content.size()) p++; // skip closing quote
        return result;
    };

    while (pos < content.size()) {
        // Skip whitespace and commas
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
            pos++;
        if (pos >= content.size() || content[pos] == '}') break;

        std::string key = parseString(pos);
        // Skip colon
        while (pos < content.size() && content[pos] != ':') pos++;
        if (pos < content.size()) pos++;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

        std::string value = parseString(pos);
        if (!key.empty()) {
            s_storage[key] = value;
        }
    }
}

static void saveStorage()
{
    if (s_storagePath.empty()) return;

    std::ofstream file(s_storagePath);
    if (!file.is_open()) return;

    auto escapeJson = [](const std::string& s) -> std::string {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    };

    file << "{\n";
    bool first = true;
    for (auto& [key, val] : s_storage) {
        if (!first) file << ",\n";
        file << "  \"" << escapeJson(key) << "\": \"" << escapeJson(val) << "\"";
        first = false;
    }
    file << "\n}\n";
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
    std::string key = jsStr(ctx, argv[0]);
    auto it = s_storage.find(key);
    if (it == s_storage.end()) return JS_NULL;
    return JS_NewString(ctx, it->second.c_str());
}

static JSValue js_setItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    std::string key = jsStr(ctx, argv[0]);
    std::string val = jsStr(ctx, argv[1]);
    s_storage[key] = val;
    saveStorage();
    return JS_UNDEFINED;
}

static JSValue js_removeItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    std::string key = jsStr(ctx, argv[0]);
    s_storage.erase(key);
    saveStorage();
    return JS_UNDEFINED;
}

static JSValue js_clear(JSContext*, JSValueConst, int, JSValueConst*) {
    s_storage.clear();
    saveStorage();
    return JS_UNDEFINED;
}

static JSValue js_key(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    if (index < 0 || static_cast<size_t>(index) >= s_storage.size()) return JS_NULL;
    auto it = s_storage.begin();
    std::advance(it, index);
    return JS_NewString(ctx, it->first.c_str());
}

static JSValue js_get_length(JSContext* ctx, JSValueConst) {
    return JS_NewInt32(ctx, static_cast<int32_t>(s_storage.size()));
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
    s_storagePath = storagePath;
    loadStorage();

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue storage = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, storage, js_storage_funcs,
                               sizeof(js_storage_funcs) / sizeof(js_storage_funcs[0]));
    JS_SetPropertyStr(ctx, global, "localStorage", storage);
    JS_FreeValue(ctx, global);
}

void StorageBindings::cleanup(JSContext*)
{
    s_storage.clear();
    s_storagePath.clear();
}

} // namespace bro::js
