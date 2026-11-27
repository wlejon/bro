#include "bronze_host/eval.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_compartment.h"

#include "api/api.h"  // brokit::api::RequireDirGuard
#include "embed/embed.h"
#include "modules/modules.h"
#include "engine/engine.h"
#include "util/asset_mounts.h"
#include "util/crash_handler.h"
#include "util/storage_file.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

namespace {

static std::unordered_map<std::string, ev::Persistent> s_dynFnCache;

void cleanGlobalProp(const std::string& name) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        // Rooted: getProperty and fromUtf8 allocate, and the argument array
        // must not pair a pre-allocation globalThis with the new string.
        ev::Persistent global(gt.value);
        ev::GlobalValue reflectG = ev::globalValue("Reflect");
        if (reflectG.found && ev::isObject(reflectG.value)) {
            ev::Persistent reflect(reflectG.value);
            ev::Persistent delFn(ev::getProperty(reflect.get(), "deleteProperty"));
            if (ev::isFunction(delFn.get())) {
                ev::Persistent key(ev::fromUtf8(name));
                const Value dArgs[2] = { global.get(), key.get() };
                ev::call(delFn.get(), reflect.get(), std::span<const Value>(dArgs, 2));
                return;
            }
        }
        ev::setProperty(global.get(), name, ev::undefined());
    }
}

} // namespace

// Module roots for a compile that runs against `engine`'s app. Every engine
// mount (`/app`, `/lib`, `/system`, `/std`) becomes a root, so the compiler
// resolves `import "/lib/x.js"` exactly as the asset loader resolves
// `<script src="/lib/x.js">`. The compiling document's import map adds its
// entries ("three" -> a file, "three/addons/" -> a folder), for static and
// dynamic imports alike.
std::vector<bronze::modules::ModuleRoot> moduleRootsFor(const engine::Engine& engine) {
    std::vector<bronze::modules::ModuleRoot> roots;
    const Compartment* comp = currentCompartment();
    const bool appViewOverride = comp && comp->kind == CompartmentKind::AppView && !comp->appDir.empty();
    for (const auto& [prefix, target] : engine.assetMounts().mounts()) {
        if (appViewOverride && prefix == "/app") {
            roots.push_back({prefix, std::filesystem::path(comp->appDir)});
        } else {
            roots.push_back({prefix, std::filesystem::path(target)});
        }
    }
    const dom::Document* doc = currentHostDocument();
    const auto& importMap = doc && doc != engine.document() ? doc->importMap() : engine.pageImportMap();
    for (const auto& [specifier, target] : importMap) {
        roots.push_back({specifier, std::filesystem::path(target)});
    }
    return roots;
}

// Where script text handed to evalScript lives, as far as its own `./x.js`
// imports are concerned: the document it came from, or for `-e` text with no
// document, a file in the app dir.
std::string entryResolvesAsFor(const engine::Engine& engine, const std::string& filename) {
    std::error_code ec;
    if (!filename.empty()) return std::filesystem::absolute(filename, ec).string();
    if (!engine.appDir().empty()) {
        return (std::filesystem::absolute(engine.appDir(), ec) / "eval.js").string();
    }
    return {};
}

bool evalScript(engine::Engine& engine, const std::string& code, const std::string& filename, ScriptGoal goal) {
    HostEvalScope evalScope;
    return evalScriptJit(engine, code, filename, nullptr, false, goal);
}

bool evalAppScript(engine::Engine& engine, const std::string& code, const std::string& filename,
                   bool moduleFile, ScriptGoal goal) {
    ev::ModuleHandle handle = 0;
    // A module script file's require('./x') resolves against its own folder
    // while its top level runs (docs/brokit-api.js: the calling file's).
    // Classic scripts are one program named for the page, and use the app
    // directory brokit falls back to (host_brokit.cpp).
    std::optional<brokit::api::RequireDirGuard> requireDir;
    if (moduleFile) {
        const std::filesystem::path file(std::u8string(filename.begin(), filename.end()));
        requireDir.emplace(file.parent_path());
    }
    bool ok;
    {
        HostEvalScope evalScope;
        ok = evalScriptJit(engine, code, filename, &handle, moduleFile, goal);
    }
    // Recorded on failure too: a top level that threw halfway still
    // registered its spans, and they are still the reload's to retire.
    engine.addAppModuleHandle(handle);
    return ok;
}

bool evalSubDocScript(engine::Engine& engine, Compartment& comp, const std::string& code,
                      const std::string& filename, bool moduleFile, ScriptGoal goal) {
    ev::ModuleHandle handle = 0;
    std::optional<brokit::api::RequireDirGuard> requireDir;
    if (moduleFile) {
        const std::filesystem::path file(std::u8string(filename.begin(), filename.end()));
        requireDir.emplace(file.parent_path());
    }
    bool ok;
    {
        HostEvalScope evalScope;
        ok = evalScriptJit(engine, code, filename, &handle, moduleFile, goal);
    }
    if (handle != 0) comp.moduleHandles.push_back(handle);
    return ok;
}

namespace {
constexpr const char* kClassicScriptMarker = "__broClassicScript";
}  // namespace

const char* classicScriptMarkerName() { return kClassicScriptMarker; }

std::string classicScriptMarker(size_t index) {
    return std::string("globalThis.") + kClassicScriptMarker + "=" + std::to_string(index) + ";";
}

std::string classicScriptEventCall(const char* type) {
    return std::string("__broClassicScriptEvent(\"") + type + "\");";
}

void clearClassicScriptMarker() {
    cleanGlobalProp(kClassicScriptMarker);
}

int takeClassicScriptMarker() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return -1;
    Value v = ev::getProperty(gt.value, kClassicScriptMarker);
    const int reached = ev::isUndefined(v) || ev::isObject(v) ? -1 : static_cast<int>(ev::toDouble(v));
    cleanGlobalProp(kClassicScriptMarker);
    return reached;
}

int currentClassicScript() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return -1;
    Value v = ev::getProperty(gt.value, kClassicScriptMarker);
    return ev::isNumber(v) ? static_cast<int>(ev::toDouble(v)) : -1;
}

bool evalScriptFile(engine::Engine& engine, const std::string& filePath) {
    HostEvalScope evalScope;
    // The breadcrumb a crash report prints. The host cannot name the LINE a
    // faulted native call came from — the script is compiled and running as
    // machine code by then — but it can always name the file, and on a run
    // that drives five model surfaces in a loop that is most of the answer.
    util::setCrashContext("script " + filePath);
    return evalScriptFileJit(engine, filePath);
}

namespace {

static engine::Engine* s_activeEngine = nullptr;

const char* sourcePrefixFor(ev::DynamicFunctionKind kind) {
    switch (kind) {
        case ev::DynamicFunctionKind::Generator: return "(function* anonymous(";
        case ev::DynamicFunctionKind::Async: return "(async function anonymous(";
        case ev::DynamicFunctionKind::AsyncGenerator: return "(async function* anonymous(";
        default: return "(function anonymous(";
    }
}

bronze::Value dynamicFunction(ev::DynamicFunctionKind kind, std::span<const bronze::Value> args) {
    if (!s_activeEngine) {
        return ev::throwError("new Function: no active engine available");
    }

    std::string params;
    std::string body;
    for (size_t i = 0; i < args.size(); ++i) {
        const bool last = (i + 1 == args.size());
        std::string text =
            ev::isUndefined(args[i]) ? std::string("undefined") : ev::toUtf8(args[i]);
        if (last) {
            body = std::move(text);
        } else {
            if (!params.empty()) params += ",";
            params += text;
        }
    }

    // Per compartment: a function is its realm's (it runs there whoever calls
    // it), so one compartment's compiled `new Function` is never another's.
    const std::string cacheKey = std::to_string(currentCompartmentId()) + "\n" +
                                 std::to_string(static_cast<int>(kind)) + "\n" + params + "\n" + body;
    auto it = s_dynFnCache.find(cacheKey);
    if (it != s_dynFnCache.end() && !ev::isUndefined(it->second.get())) {
        return it->second.get();
    }

    // One compile whose completion value is the function. What the compile
    // or the run threw (a SyntaxError for a bad body) is thrown to the caller
    // as that value, so it is catchable and leaves the page unharmed: through
    // evalScript, one bad body latched the page's failure flag, and every
    // later `new Function` in the page then failed as well.
    const std::string expr = std::string(sourcePrefixFor(kind)) + params + ") {\n" + body + "\n})";
    auto res = evalScriptJitResult(*s_activeEngine, expr, "<new Function>", nullptr, false, ScriptGoal::Module);
    if (res.thrown) return ev::throwValue(res.value);
    if (!ev::isFunction(res.value)) return ev::throwError("new Function: created function was not found");
    ev::Persistent& cached = s_dynFnCache[cacheKey];
    cached.set(res.value);
    return cached.get();
}

bronze::Value dynamicEval(bronze::Value source) {
    if (!ev::isString(source)) return source;
    if (!s_activeEngine) {
        return ev::throwError("eval: no active engine available");
    }
    const std::string code = ev::toUtf8(source);

    // One compile of the text as written. bronze's evaluator answers the
    // completion value (the last statement that gave one), so there is no
    // expression-first probe to make, and none is wanted: a probe that fails
    // to parse `var x = 1` as an expression used to go through the reporting
    // path, whose failure latch then failed the statement retry that had in
    // fact succeeded. What the script threw is rethrown as the value it was,
    // for the caller's catch.
    auto res = evalScriptJitResult(*s_activeEngine, code, "<eval>");
    if (res.thrown) return ev::throwValue(res.value);
    return res.value;
}

// The importer's `import.meta.url` as a file path: a file: URL decoded
// (`file:///C:/x%20y/a.js` is C:/x y/a.js, `file:///home/a.js` /home/a.js),
// anything else as it is.
std::string referrerPath(const std::string& url) {
    std::string rest = url;
    if (rest.rfind("file://", 0) == 0) {
        rest = rest.substr(7);
        if (rest.size() >= 3 && rest[0] == '/' && std::isalpha(static_cast<unsigned char>(rest[1])) &&
            rest[2] == ':') {
            rest = rest.substr(1);
        }
    }
    std::string out;
    out.reserve(rest.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(rest[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(rest.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(rest[i]);
        }
    }
    return out;
}

Value errorValue(const std::string& message) {
    ev::GlobalValue ctor = ev::globalValue("Error");
    if (!ctor.found) return ev::fromUtf8(message);
    ev::Persistent fn(ctor.value);
    ev::Persistent msg(ev::fromUtf8(message));
    const Value args[1] = {msg.get()};
    ev::CallResult made = ev::construct(fn.get(), std::span<const Value>(args, 1));
    return made.value;
}

// An `import()` whose specifier the compiler could not read (built at run
// time). It is answered as a static import of the same specifier written in
// the importer would be: a unit that imports the module's namespace,
// compiled to resolve as the importer does (its file, the app's mounts and
// import map), so a relative specifier resolves against the importer, and
// a module already loaded (the page's, an earlier import()) is the same
// instance and namespace rather than a second one. A file that is not there
// rejects with an Error; what the module's evaluation threw rejects with
// that value. The module is evaluated before import() returns; its promise
// settles as a job, as always.
Value dynamicImport(Value specifierValue, const std::string& referrerUrl) {
    ev::Persistent promise(ev::createPromise());
    if (!s_activeEngine) {
        ev::Persistent err(errorValue("import(): no active engine available"));
        ev::rejectPromise(promise.get(), err.get());
        return promise.get();
    }
    const std::string specifier = ev::toUtf8(specifierValue);
    const std::string referrer = referrerPath(referrerUrl);

    // A path the compiler would look for beside the importer: a missing file
    // is a load failure, named as one.
    const bool relative = specifier.rfind("./", 0) == 0 || specifier.rfind("../", 0) == 0;
    if (relative && !referrer.empty()) {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::path(std::u8string(referrer.begin(), referrer.end()))
                                               .parent_path();
        const std::filesystem::path target =
            (base / std::filesystem::path(std::u8string(specifier.begin(), specifier.end()))).lexically_normal();
        if (!std::filesystem::is_regular_file(target, ec)) {
            ev::Persistent err(errorValue("import(): could not load module '" + specifier + "' from '" +
                                          referrer + "' (no file " + target.generic_string() + ")"));
            ev::rejectPromise(promise.get(), err.get());
            return promise.get();
        }
    }

    const std::string code =
        "import * as __broDynamicImport from " + util::jsonQuote(specifier) + ";\n__broDynamicImport\n";
    auto res = evalScriptJitResult(*s_activeEngine, code, referrer, nullptr, false, ScriptGoal::Module);
    ev::Persistent value(res.value);
    if (res.thrown) {
        ev::rejectPromise(promise.get(), value.get());
    } else {
        ev::resolvePromise(promise.get(), value.get());
    }
    return promise.get();
}

} // namespace

void installDynamicHooks(engine::Engine& engine) {
    s_activeEngine = &engine;
    ev::setDynamicFunctionHook(dynamicFunction);
    ev::setDynamicEvalHook(dynamicEval);
    ev::setDynamicImportHook(dynamicImport);
}

void clearDynamicModules() {
    s_dynFnCache.clear();
}

} // namespace bro::bronze_host
