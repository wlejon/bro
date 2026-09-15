#include "bronze_host/eval_jit.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_callee_namer.h"
#include "bronze_host/host_pins.h"
#include "engine/engine.h"
#include "util/asset_mounts.h"
#include "util/log.h"

#include "eval/eval.h"
#include "embed/embed.h"
#include "modules/modules.h"

#include <cstring>
#include <filesystem>
#include <fstream>
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

bool reportCallResult(engine::Engine& engine, const bronze::embed::CallResult& res, const char* context) {
    if (res.thrown) {
        std::string errMsg;
        if (res.value.isObject()) {
            bronze::Value stack = bronze::embed::getProperty(res.value, "stack");
            if (!bronze::embed::isUndefined(stack) && !bronze::embed::isNull(stack)) {
                errMsg = bronze::embed::toUtf8(stack);
            } else {
                bronze::Value msg = bronze::embed::getProperty(res.value, "message");
                if (!bronze::embed::isUndefined(msg) && !bronze::embed::isNull(msg)) {
                    errMsg = bronze::embed::toUtf8(msg);
                }
            }
        }
        if (errMsg.empty()) {
            errMsg = bronze::embed::toUtf8(res.value);
        }
        LOG_ERROR("%s error: %s", context, errMsg.c_str());
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

std::string wrapAsyncIife(const std::string& code) {
    return "(async () => {\n" + code +
           "\n})().catch(err => { console.error(err && err.stack ? err.stack : err); if (typeof assert === 'function') assert(false, 'Unhandled error: ' + (err && err.message ? err.message : err)); });\n";
}

} // namespace

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

    std::string execCode = code;
    if (hasAwaitStmt(execCode) && !hasImportStmt(execCode)) {
        execCode = wrapAsyncIife(execCode);
    }

    auto res = bronze::eval::evalScript(execCode, opts);

    if (res.thrown && execCode == code && !hasImportStmt(code)) {
        std::string errStr = bronze::embed::toUtf8(res.value);
        if (errStr.find("await") != std::string::npos) {
            // The failed attempt is off the stack and superseded: its handle
            // is retired here so the one written below is the run's only one.
            if (moduleHandleOut && *moduleHandleOut) bronze::embed::unloadModule(*moduleHandleOut);
            res = bronze::eval::evalScript(wrapAsyncIife(code), opts);
        }
    }
    return res;
}

bool evalScriptJit(engine::Engine& engine, const std::string& code, const std::string& filename,
                   bronze::embed::ModuleHandle* moduleHandleOut) {
    HostEvalScope evalScope;
    auto res = evalScriptJitResult(engine, code, filename, moduleHandleOut);
    if (!reportCallResult(engine, res, "evalScriptJit")) {
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

    bronze::embed::CallResult res;
    if (hasAwaitStmt(content) && !hasImportStmt(content)) {
        res = bronze::eval::evalScript(wrapAsyncIife(content), opts);
    } else {
        res = bronze::eval::evalFile(absPath.string(), opts);
        if (res.thrown) {
            std::string errStr = bronze::embed::toUtf8(res.value);
            if (errStr.find("await") != std::string::npos && !hasImportStmt(content)) {
                res = bronze::eval::evalScript(wrapAsyncIife(content), opts);
            }
        }
    }

    if (!reportCallResult(engine, res, "evalScriptFileJit")) {
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
