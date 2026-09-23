#include "bronze_host/eval_jit.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_callee_namer.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_pins.h"
#include "bronze_host/host_rejection_events.h"
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
#include <cstdlib>
#include <future>
#include <thread>
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
        // The page's `error` handlers see a script's uncaught throw, as they
        // do on the web. The run still counts as failed and is still logged:
        // a script that died at its top level did not finish, whatever a
        // handler made of it.
        bronze::embed::Persistent thrownRoot(res.value);
        hostDispatchUncaughtError(thrownRoot.get());
        std::string errMsg = thrownValueText(thrownRoot.get());
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

// Appended to a driver script so its END is observable (evalScriptFileJit).
// A property write on globalThis, not a declaration: the name is nothing a
// test could collide with, and it needs no host-globals entry.
constexpr const char* kScriptDoneMarker = "\n;globalThis.__broScriptDone = true;\n";

void resetScriptDone() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "__broScriptDone", ev::fromBool(false));
    }
}

bool scriptDone() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return true;
    Value v = ev::getProperty(gt.value, "__broScriptDone");
    return ev::isBool(v) && ev::toBool(v);
}

// Fire the rejection events the last drains queued NOW rather than at the
// next frame's task drain, which a run that is about to end never reaches:
// an unhandled rejection left by a script's final turn is still reported
// (and, uncancelled, fails the run). Bounded, since a handler may reject
// again.
void settleRejections() {
    for (int i = 0; i < 8 && rejectionEventsPending(); ++i) {
        flushRejectionEvents();
        if (ev::microtasksPending()) ev::drainMicrotasks();
    }
}

// A headless driver script suspended at a top-level `await` is still
// running: frames are pumped — timers, rAF, host tasks, worker messages, the
// microtask checkpoint, each advancing the virtual clock one frame and kept
// no faster than the wall clock, so a worker or a decode on another thread
// gets the time its reply takes — until the script reaches its end or the
// run has already failed. One that never gets there fails the run: a test
// whose remainder never executed has not passed.
void awaitScriptCompletion(engine::Engine& engine, const std::string& filename) {
    if (engine.displayMode() != engine::DisplayMode::Headless) return;
    if (scriptDone() || hasTestFailure() || engine.hasTestFailure()) return;

    double limitMs = 30000.0;
    if (const char* env = std::getenv("BRO_SCRIPT_SETTLE_MS")) {
        const double v = std::atof(env);
        if (v > 0.0) limitMs = v;
    }
    constexpr double kFrameMs = 1000.0 / 60.0;
    const auto start = std::chrono::steady_clock::now();
    double virtualMs = 0.0;
    while (!scriptDone() && !hasTestFailure() && !engine.hasTestFailure()) {
        const double wallMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start).count();
        if (wallMs >= limitMs) break;
        if (virtualMs > wallMs) {
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(virtualMs - wallMs));
        }
        engine.advanceTime(kFrameMs);
        virtualMs += kFrameMs;
        pumpBrokitTicks();
        if (ev::microtasksPending()) ev::drainMicrotasks();
        settleRejections();
    }
    if (!scriptDone() && !hasTestFailure() && !engine.hasTestFailure()) {
        LOG_ERROR("evalScriptFileJit: the script's top-level await never settled "
                  "(%.0f ms, %.0f ms of frames) — the rest of %s did not run",
                  limitMs, virtualMs, filename.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
    }
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
    settleRejections();
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

    // The script's last statement records that it RAN to its end. A script
    // suspended at a top-level `await` (the async-IIFE form, or a module's
    // native TLA) returns from its top level long before that, and without
    // the marker a run would stop there: the rest of the test never executed
    // and the run still read as a pass.
    const std::string source = content + kScriptDoneMarker;
    resetScriptDone();

    bronze::embed::CallResult res;
    if (hasAwaitStmt(content) && !hasImportStmt(content)) {
        auto compiled = compileWithPumping(engine, [&]() {
            return bronze::eval::compileScript(wrapAsyncIife(source, absPath.string()), opts);
        });
        res = bronze::eval::runCompiledScript(std::move(compiled), opts);
    } else {
        auto compiled = compileWithPumping(engine, [&]() {
            return bronze::eval::compileScript(source, opts);
        });
        res = bronze::eval::runCompiledScript(std::move(compiled), opts);
        if (res.thrown) {
            std::string errStr = bronze::embed::toUtf8(res.value);
            if (errStr.find("await") != std::string::npos && !hasImportStmt(content)) {
                auto retryCompiled = compileWithPumping(engine, [&]() {
                    return bronze::eval::compileScript(wrapAsyncIife(source, absPath.string()), opts);
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
    awaitScriptCompletion(engine, absPath.string());
    settleRejections();
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

} // namespace bro::bronze_host
