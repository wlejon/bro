#include "bronze_host/eval_jit.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_callee_namer.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_pins.h"
#include "engine/engine.h"
#include "util/asset_mounts.h"
#include "util/log.h"

#include "eval/eval.h"
#include "embed/embed.h"
#include "modules/modules.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <vector>

namespace bro::bronze_host {

bool isJitDisabled() {
    static bool disabled = [] {
        const char* val = std::getenv("BRO_DISABLE_JIT");
        if (!val) val = std::getenv("BRO_EVAL_AOT");
        if (!val) val = std::getenv("BRO_NO_JIT");
        return val && (std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0);
    }();
    return disabled;
}

namespace {

// A throw out of a script's top level, reported with the script it came out
// of. The value's own text is `thrownValueText`'s: a `stack` when one was set,
// else `Name: message`. bronze records no source position on an Error — no
// line, no stack (runtime/exception.h) — so the script name is the location
// the report can give; `throw new Error(...)` sites in a test are what carry
// the rest.
bool reportCallResult(engine::Engine& engine, const bronze::embed::CallResult& res,
                      const char* context, const std::string& filename) {
    if (res.thrown) {
        std::string errMsg = thrownValueText(res.value);
        LOG_ERROR("%s: uncaught %s (in %s)", context, errMsg.c_str(),
                  filename.empty() ? "<eval>" : filename.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }
    return true;
}

bool hasAwaitStmt(const std::string& code) {
    size_t i = 0;
    while (i < code.size()) {
        while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) i++;
        if (i + 5 <= code.size() && code.compare(i, 5, "await") == 0) {
            char next = (i + 5 < code.size()) ? code[i + 5] : '\0';
            if (next == ' ' || next == '\t' || next == '(') {
                return true;
            }
        }
        while (i < code.size() && code[i] != '\n') i++;
        if (i < code.size() && code[i] == '\n') i++;
    }
    return false;
}

bool hasImportStmt(const std::string& code) {
    size_t i = 0;
    while (i < code.size()) {
        while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) i++;
        if (i + 6 <= code.size() && code.compare(i, 6, "import") == 0) {
            char next = (i + 6 < code.size()) ? code[i + 6] : '\0';
            if (next == ' ' || next == '\t' || next == '{' || next == '*' || next == '"' || next == '\'') {
                return true;
            }
        }
        while (i < code.size() && code[i] != '\n') i++;
        if (i < code.size() && code[i] == '\n') i++;
    }
    return false;
}

template <typename Fn>
auto compileWithPumping(engine::Engine& engine, Fn&& compileFn) {
    if (engine.displayMode() == engine::DisplayMode::Windowed && engine.window()) {
        engine.setAppCompiling(true);
        auto future = std::async(std::launch::async, std::forward<Fn>(compileFn));
        while (future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
            if (engine.splashVisible()) {
                engine.pumpSplashFrame(16.67);
            } else {
                engine.pumpEventsOnly();
            }
        }
        engine.setAppCompiling(false);
        return future.get();
    } else {
        return compileFn();
    }
}

} // namespace

std::string wrapAsyncIife(const std::string& code, const std::string& filename) {
    // The filename as a JS string literal: backslashes (Windows paths) and
    // quotes escaped, nothing else can appear in a path the OS opened.
    std::string where;
    for (char c : filename) {
        if (c == '\\' || c == '\'') where += '\\';
        where += c;
    }
    return "(async () => {\n" + code +
           "\n})().catch(err => {"
           " const text = err && typeof err.stack === 'string' && err.stack ? err.stack"
           " : err && typeof err.message === 'string' ? ((err.name || 'Error') + ': ' + err.message)"
           " : String(err);"
           " const where = '" + where + "';"
           " const report = 'Unhandled error: ' + text + (where ? ' (in ' + where + ')' : '');"
           " if (typeof assert === 'function') assert(false, report);"
           " else console.error(report); });\n";
}

bronze::embed::CallResult evalScriptJitResult(engine::Engine& engine, const std::string& code,
                                              const std::string& filename,
                                              bronze::embed::ModuleHandle* moduleHandleOut) {
    HostEvalScope evalScope;
    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    initHostCalleeNamer();

    bronze::eval::EvalOptions opts;
    opts.filename = filename.empty() ? "<eval>" : filename;
    // Read off the registry the install above filled, not from a list kept
    // beside it: what is registered is exactly what the compile admits.
    opts.hostGlobals = registeredHostGlobals();
    opts.moduleRoots = moduleRootsFor(engine);
    opts.entryResolvesAs = entryResolvesAsFor(engine, filename);
    opts.retainSource = true;
    opts.pinsPath = discoverPinsPath(engine, filename);
    opts.censusOutPath = discoverCensusOutPath(engine, filename);
    opts.moduleHandleOut = moduleHandleOut;
    opts.optimize = engine.jitOptimize();
    // One module map per realm. The page is one compilation unit and a headless
    // driver script is another, so without this a test's `import "/app/lib/x.js"`
    // compiles the app's module into its OWN unit and evaluates it a second
    // time — a second instance of state the page already holds. Publishing here
    // is what makes the page's instances the ones a later unit binds
    // (bronze: runtime/module_registry.h).
    opts.moduleRegistry = true;

    std::string execCode = code;
    if (hasAwaitStmt(execCode) && !hasImportStmt(execCode)) {
        execCode = wrapAsyncIife(execCode, filename);
    }

    auto compiled = compileWithPumping(engine, [&]() {
        return bronze::eval::compileScript(execCode, opts);
    });
    auto res = bronze::eval::runCompiledScript(std::move(compiled), opts);

    if (res.thrown && execCode == code && !hasImportStmt(code)) {
        std::string errStr = bronze::embed::toUtf8(res.value);
        if (errStr.find("await") != std::string::npos) {
            // The failed attempt is off the stack and superseded: its handle
            // is retired here so the one written below is the run's only one.
            if (moduleHandleOut && *moduleHandleOut) bronze::embed::unloadModule(*moduleHandleOut);
            auto retryCompiled = compileWithPumping(engine, [&]() {
                return bronze::eval::compileScript(wrapAsyncIife(code, filename), opts);
            });
            res = bronze::eval::runCompiledScript(std::move(retryCompiled), opts);
        }
    }
    return res;
}

bool evalScriptJit(engine::Engine& engine, const std::string& code, const std::string& filename,
                   bronze::embed::ModuleHandle* moduleHandleOut) {
    HostEvalScope evalScope;
    auto res = evalScriptJitResult(engine, code, filename, moduleHandleOut);
    if (!reportCallResult(engine, res, "evalScriptJit", filename)) {
        return false;
    }

    if (bronze::embed::microtasksPending()) {
        bronze::embed::drainMicrotasks();
    }
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

bool evalScriptFileJit(engine::Engine& engine, const std::string& filePath) {
    HostEvalScope evalScope;
    std::error_code ec;
    std::filesystem::path p(filePath);
    if (!std::filesystem::exists(p, ec)) {
        LOG_ERROR("evalScriptFileJit: file does not exist: %s", filePath.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    auto absPath = std::filesystem::absolute(p, ec);

    std::string content;
    {
        std::ifstream ifs(absPath, std::ios::binary);
        if (ifs.is_open()) {
            content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
    }

    initHostCalleeNamer();

    bronze::eval::EvalOptions opts;
    opts.filename = absPath.string();
    opts.hostGlobals = registeredHostGlobals();
    opts.moduleRoots = moduleRootsFor(engine);
    opts.entryResolvesAs = absPath;
    opts.retainSource = true;
    opts.pinsPath = discoverPinsPath(engine, absPath);
    opts.censusOutPath = discoverCensusOutPath(engine, absPath);
    opts.optimize = engine.jitOptimize();
    // The consuming half of the same seam: a specifier that resolves to a file
    // the page already evaluated binds that instance instead of compiling a
    // second copy of it into this unit. The driver's OWN file is the entry and
    // is never published, so running the same driver twice runs it twice.
    opts.moduleRegistry = true;

    bronze::embed::CallResult res;
    if (hasAwaitStmt(content) && !hasImportStmt(content)) {
        auto compiled = compileWithPumping(engine, [&]() {
            return bronze::eval::compileScript(wrapAsyncIife(content, absPath.string()), opts);
        });
        res = bronze::eval::runCompiledScript(std::move(compiled), opts);
    } else {
        auto compiled = compileWithPumping(engine, [&]() {
            return bronze::eval::compileFile(absPath.string(), opts);
        });
        res = bronze::eval::runCompiledScript(std::move(compiled), opts);
        if (res.thrown) {
            std::string errStr = bronze::embed::toUtf8(res.value);
            if (errStr.find("await") != std::string::npos && !hasImportStmt(content)) {
                auto retryCompiled = compileWithPumping(engine, [&]() {
                    return bronze::eval::compileScript(wrapAsyncIife(content, absPath.string()), opts);
                });
                res = bronze::eval::runCompiledScript(std::move(retryCompiled), opts);
            }
        }
    }

    if (!reportCallResult(engine, res, "evalScriptFileJit", absPath.string())) {
        return false;
    }

    if (bronze::embed::microtasksPending()) {
        bronze::embed::drainMicrotasks();
    }
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

} // namespace bro::bronze_host
