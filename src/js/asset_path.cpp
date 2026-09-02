#include "js/asset_path.h"
#include "util/asset_mounts.h"
#include "util/user_dirs.h"
#include <filesystem>
#include <system_error>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace {
std::string s_basePath;
const util::AssetMounts* s_mounts = nullptr;
}

void setAssetPathContext(const std::string& basePath, const util::AssetMounts* mounts) {
    s_basePath = basePath;
    s_mounts = mounts;
}

std::string resolveAssetPath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;           // C:\... drive
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) {
        if (s_mounts) {
            std::string m = s_mounts->resolve(src);
            if (!m.empty()) return m;
        }
        // An unmounted absolute path is already a filesystem path.
        return src;
    }
    if (s_basePath.empty()) return src;
    std::string path = s_basePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

namespace {

JSValue js_resolvePath(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "resolvePath: path required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string resolved = resolveAssetPath(s);
    JS_FreeCString(ctx, s);
    // Hand back a native-separator path: the caller's next move is usually to
    // spawn a process with it, and Windows tools are happier with backslashes.
    return JS_NewString(ctx, std::filesystem::path(resolved).make_preferred().string().c_str());
}

// bro.userDataDir — created on first read, so an app that never stores
// anything leaves nothing behind, and one that does can write to the path it
// was handed without a mkdir of its own. Native separators, like appDir.
JSValue js_get_userDataDir(JSContext* ctx, JSValueConst) {
    std::string dir = util::appUserDataDir(s_basePath);
    if (dir.empty()) return JS_NewString(ctx, "");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return JS_NewString(ctx, std::filesystem::path(dir).make_preferred().string().c_str());
}

} // namespace

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installAssetPathBindings(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JS_SetPropertyStr(ctx, broObj, "resolvePath",
                      JS_NewCFunction(ctx, js_resolvePath, "resolvePath", 1));

    // The app's own root. Empty when there is no app directory (a bare
    // `bro-headless -e` session), which callers can test for.
    std::string dir = s_basePath.empty()
                          ? std::string()
                          : std::filesystem::path(s_basePath).make_preferred().string();
    JS_SetPropertyStr(ctx, broObj, "appDir", JS_NewString(ctx, dir.c_str()));

    // Where the app's own persistent data belongs (util/user_dirs.h). A getter
    // rather than a value: the directory is created when it is first asked for.
    {
        JSAtom atom = JS_NewAtom(ctx, "userDataDir");
        JSValue getter = JS_NewCFunction2(ctx, reinterpret_cast<JSCFunction*>(js_get_userDataDir),
                                          "get userDataDir", 0, JS_CFUNC_getter, 0);
        JS_DefinePropertyGetSet(ctx, broObj, atom, getter, JS_UNDEFINED,
                                JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
    }

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

std::string resolveAssetWritePath(const std::string& src) {
    namespace fs = std::filesystem;
    fs::path p(src);
    // No parent to resolve (a bare filename), or already absolute: the read
    // rules are exactly right.
    if (p.is_absolute() || !p.has_parent_path()) return resolveAssetPath(src);
    // Resolve the DIRECTORY and rejoin the filename. Resolving the whole path
    // would ask a mount to resolve a file that does not exist yet, which is
    // the one thing the read path cannot answer.
    std::string dir = resolveAssetPath(p.parent_path().generic_string());
    return (fs::path(dir) / p.filename()).generic_string();
}


} // namespace bro::js
