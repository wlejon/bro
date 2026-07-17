#include "js/runtime.h"
#include "util/asset_mounts.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <api/api.h>  // brokit::api::uninstallFetch

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Module loader helpers (file-based)
// ---------------------------------------------------------------------------

static char* module_normalize(JSContext* ctx, const char* base_name,
                              const char* name, void* opaque)
{
    if (!name) return nullptr;

    const auto* mounts = static_cast<const util::AssetMounts*>(opaque);

    std::string result;
    if (name[0] == '.' && base_name) {
        // Relative specifier — resolve against the importing module's path.
        std::string base(base_name);
        auto slash = base.find_last_of("/\\");
        if (slash != std::string::npos) {
            result = base.substr(0, slash + 1) + name;
        } else {
            result = name;
        }
    } else if (name[0] == '/' && mounts) {
        // Engine-mounted specifier (e.g. "/lib/foo.js"). Rewrite through the
        // asset mounts so the shared stdlib is importable by virtual path.
        // Falls through to the literal path if no mount prefix matches.
        std::string mounted = mounts->resolve(name);
        result = mounted.empty() ? name : mounted;
    } else {
        result = name;
    }

    // Canonicalize lexically so different spellings of the same file
    // ("a/./b.js", "a/x/../b.js") collapse to one module name. QuickJS keys its
    // module cache on this string, so without this a module imported two ways
    // is instantiated twice — breaking any module-level singleton state.
    if (result.find('/') != std::string::npos ||
        result.find('\\') != std::string::npos) {
        std::error_code ec;
        std::string normalized =
            std::filesystem::path(result).lexically_normal().string();
        if (!normalized.empty()) result = normalized;
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
    if (Runtime::checkException(ctx, func, ErrorOrigin::module(module_name))) {
        return nullptr;
    }

    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func));
    JS_FreeValue(ctx, func);
    return m;
}

// ---------------------------------------------------------------------------
// Error funnel: suppressor + reporter + host hook dispatch
//
// All state is process-wide (function-local statics). Workers run on their
// own threads with their own JSRuntime; the suppressor's fingerprint includes
// file:line so cross-thread collisions are vanishingly rare and benign at
// worst (a few extra suppressed log lines).
//
// Recursion guard: if window.onerror itself throws, we must not re-enter the
// funnel for that secondary failure, otherwise a buggy onerror would loop
// forever. The thread_local flag short-circuits the host-hook step on reentry;
// the secondary error still gets logged through the normal path.
// ---------------------------------------------------------------------------

namespace {

constexpr double kSuppressionWindowMs = 5000.0;

struct SuppressEntry {
    std::string fingerprint;
    std::string headerLine;     // the original "[js error] ..." line, for the tail
    int repeatCount = 0;        // additional occurrences past the first
    double firstSeenMs = 0.0;
    double lastSeenMs = 0.0;
};

// Small bounded LRU. Bound is generous — fingerprint includes file:line so
// the working set is small in practice.
constexpr size_t kSuppressorMaxEntries = 128;

struct Suppressor {
    std::list<SuppressEntry> order;          // front = most recently touched
    std::unordered_map<std::string, std::list<SuppressEntry>::iterator> byKey;
};

Suppressor& suppressor() {
    static Suppressor s;
    return s;
}

thread_local int g_inHostHook = 0;

// RAII guard for the host-hook recursion counter. The counter stops
// window.onerror / window.onunhandledrejection from recursing if the handler
// itself throws. It must be decremented even when JS_Call unwinds via a C++
// exception (e.g. one thrown from a qjsbind native callback invoked by the
// handler) — a bare manual decrement would be skipped on that path and wedge
// the error hook permanently.
struct HostHookGuard {
    HostHookGuard()  { ++g_inHostHook; }
    ~HostHookGuard() { --g_inHostHook; }
    HostHookGuard(const HostHookGuard&) = delete;
    HostHookGuard& operator=(const HostHookGuard&) = delete;
};

// Emit the "still failing" tail for an entry being closed/aged out. Caller
// must have removed it from the map already; we just print.
void emitTail(const SuppressEntry& e, double nowMs) {
    if (e.repeatCount <= 0) return;
    double spanSec = (e.lastSeenMs - e.firstSeenMs) * 0.001;
    if (spanSec < 0.0) spanSec = 0.0;
    LOG_ERROR("[js error] still failing: %d more occurrences in %.1fs — %s",
              e.repeatCount, spanSec, e.headerLine.c_str());
    (void)nowMs;
}

void flushAged(double nowMs) {
    auto& s = suppressor();
    for (auto it = s.order.begin(); it != s.order.end(); ) {
        if (nowMs - it->lastSeenMs >= kSuppressionWindowMs) {
            emitTail(*it, nowMs);
            s.byKey.erase(it->fingerprint);
            it = s.order.erase(it);
        } else {
            ++it;
        }
    }
}

// Returns true if the caller should log the full entry (first occurrence or
// post-window). Returns false if the caller should swallow the log (within
// the suppression window).
bool noteOccurrence(const std::string& fingerprint, const std::string& header) {
    double now = bro::util::currentTimeMs();
    auto& s = suppressor();

    flushAged(now);

    auto found = s.byKey.find(fingerprint);
    if (found == s.byKey.end()) {
        // First sighting (or first since flush). Insert and tell caller to log.
        if (s.order.size() >= kSuppressorMaxEntries) {
            // Evict LRU (back of list). Emit its tail.
            auto victim = std::prev(s.order.end());
            emitTail(*victim, now);
            s.byKey.erase(victim->fingerprint);
            s.order.erase(victim);
        }
        SuppressEntry e;
        e.fingerprint = fingerprint;
        e.headerLine = header;
        e.firstSeenMs = now;
        e.lastSeenMs = now;
        s.order.push_front(std::move(e));
        s.byKey[fingerprint] = s.order.begin();
        return true;
    }

    // Repeat — bump count, move to front, swallow.
    auto it = found->second;
    it->repeatCount++;
    it->lastSeenMs = now;
    s.order.splice(s.order.begin(), s.order, it);
    return false;
}

const char* originName(ErrorOriginKind k) {
    switch (k) {
        case ErrorOriginKind::Eval:             return "eval";
        case ErrorOriginKind::Module:           return "module";
        case ErrorOriginKind::Timer:            return "timer";
        case ErrorOriginKind::Raf:              return "rAF";
        case ErrorOriginKind::Listener:         return "listener";
        case ErrorOriginKind::Microtask:        return "microtask";
        case ErrorOriginKind::PromiseRejection: return "unhandled rejection";
        case ErrorOriginKind::Binding:          return "binding";
    }
    return "?";
}

// Pull the first "  at fn (file:line:col)" line from a stack string. Used to
// fingerprint exceptions so two different throws with the same name+message
// from different code sites don't get suppressed as one.
std::string firstStackFrame(const std::string& stack) {
    if (stack.empty()) return {};
    size_t end = stack.find('\n');
    std::string line = (end == std::string::npos) ? stack : stack.substr(0, end);
    // Trim leading whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    return line.substr(start);
}

// True for "InternalError: interrupted" — our shutdown path, never a script bug.
bool isInterruptException(JSContext* ctx, JSValueConst exception) {
    if (!JS_IsObject(exception)) return false;
    JSValue nameVal = JS_GetPropertyStr(ctx, exception, "name");
    JSValue msgVal  = JS_GetPropertyStr(ctx, exception, "message");
    const char* name = JS_ToCString(ctx, nameVal);
    const char* msg  = JS_ToCString(ctx, msgVal);
    bool match = name && msg
        && std::strcmp(name, "InternalError") == 0
        && std::strcmp(msg, "interrupted") == 0;
    if (name) JS_FreeCString(ctx, name);
    if (msg)  JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, nameVal);
    JS_FreeValue(ctx, msgVal);
    return match;
}

// Try window.onerror / window.onunhandledrejection. Returns true if the
// host suppressed the engine log.
bool dispatchHostHook(JSContext* ctx, const ErrorOrigin& origin,
                      const std::string& message, JSValueConst exception)
{
    if (g_inHostHook > 0) return false;  // recursion guard

    JSValue global = JS_GetGlobalObject(ctx);
    bool suppress = false;

    if (origin.kind == ErrorOriginKind::PromiseRejection) {
        JSValue handler = JS_GetPropertyStr(ctx, global, "onunhandledrejection");
        if (JS_IsFunction(ctx, handler)) {
            JSValue evt = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, evt, "reason", JS_DupValue(ctx, exception));
            JS_SetPropertyStr(ctx, evt, "type", JS_NewString(ctx, "unhandledrejection"));
            JS_SetPropertyStr(ctx, evt, "_prevented", JS_FALSE);
            // Minimal preventDefault — sets _prevented to true.
            JS_SetPropertyStr(ctx, evt, "preventDefault",
                JS_NewCFunction(ctx,
                    [](JSContext* c, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                        JS_SetPropertyStr(c, this_val, "_prevented", JS_TRUE);
                        return JS_UNDEFINED;
                    }, "preventDefault", 0));

            JSValue ret;
            {
                HostHookGuard guard;
                ret = JS_Call(ctx, handler, global, 1, &evt);
            }

            if (JS_IsException(ret)) {
                // Host hook itself threw — report it under a synthetic origin
                // so it's distinguishable but doesn't recurse (guard is set
                // back to 0 here, but our re-entry into reportException sees
                // the guard at 0; the secondary call won't dispatch a hook
                // again because we explicitly skip when origin is the host
                // hook itself. Simplest: just log it directly below.)
                JSValue secondary = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, secondary);
                LOG_ERROR("[js error] window.onunhandledrejection itself threw: %s",
                          s ? s : "(no message)");
                if (s) JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, secondary);
            } else {
                JSValue prev = JS_GetPropertyStr(ctx, evt, "_prevented");
                if (JS_ToBool(ctx, prev)) suppress = true;
                JS_FreeValue(ctx, prev);
            }
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, evt);
        }
        JS_FreeValue(ctx, handler);
    } else {
        JSValue handler = JS_GetPropertyStr(ctx, global, "onerror");
        if (JS_IsFunction(ctx, handler)) {
            // Pull source/line/col from the exception's first stack frame if any
            std::string source, line, col;
            if (JS_IsObject(exception)) {
                JSValue stackVal = JS_GetPropertyStr(ctx, exception, "stack");
                const char* stackStr = JS_ToCString(ctx, stackVal);
                if (stackStr) {
                    std::string frame = firstStackFrame(stackStr);
                    // Parse "at fn (file:line:col)" — best-effort, never throws.
                    auto lp = frame.rfind('(');
                    auto rp = frame.rfind(')');
                    std::string inside = (lp != std::string::npos && rp != std::string::npos && rp > lp)
                        ? frame.substr(lp + 1, rp - lp - 1) : frame;
                    auto c2 = inside.rfind(':');
                    auto c1 = (c2 != std::string::npos) ? inside.rfind(':', c2 - 1) : std::string::npos;
                    if (c1 != std::string::npos && c2 != std::string::npos) {
                        source = inside.substr(0, c1);
                        line   = inside.substr(c1 + 1, c2 - c1 - 1);
                        col    = inside.substr(c2 + 1);
                    } else {
                        source = inside;
                    }
                    JS_FreeCString(ctx, stackStr);
                }
                JS_FreeValue(ctx, stackVal);
            }

            JSValue args[5] = {
                JS_NewString(ctx, message.c_str()),
                JS_NewString(ctx, source.c_str()),
                JS_NewInt32(ctx, line.empty() ? 0 : std::atoi(line.c_str())),
                JS_NewInt32(ctx, col.empty()  ? 0 : std::atoi(col.c_str())),
                JS_DupValue(ctx, exception),
            };

            JSValue ret;
            {
                HostHookGuard guard;
                ret = JS_Call(ctx, handler, global, 5, args);
            }

            if (JS_IsException(ret)) {
                JSValue secondary = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, secondary);
                LOG_ERROR("[js error] window.onerror itself threw: %s",
                          s ? s : "(no message)");
                if (s) JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, secondary);
            } else if (JS_ToBool(ctx, ret)) {
                suppress = true;
            }
            JS_FreeValue(ctx, ret);
            for (auto& a : args) JS_FreeValue(ctx, a);
        }
        JS_FreeValue(ctx, handler);
    }

    JS_FreeValue(ctx, global);
    return suppress;
}

// Format and emit one full-detail log entry. Suppression is consulted here.
void emitErrorLog(JSContext* ctx, const ErrorOrigin& origin, JSValueConst exception)
{
    // Extract name + message.
    std::string name = "Error", message;
    if (JS_IsObject(exception)) {
        JSValue n = JS_GetPropertyStr(ctx, exception, "name");
        JSValue m = JS_GetPropertyStr(ctx, exception, "message");
        const char* ns = JS_ToCString(ctx, n);
        const char* ms = JS_ToCString(ctx, m);
        if (ns) { name = ns; JS_FreeCString(ctx, ns); }
        if (ms) { message = ms; JS_FreeCString(ctx, ms); }
        JS_FreeValue(ctx, n);
        JS_FreeValue(ctx, m);
    }
    if (message.empty()) {
        const char* s = JS_ToCString(ctx, exception);
        if (s) { message = s; JS_FreeCString(ctx, s); }
    }

    // Extract stack.
    std::string stack;
    if (JS_IsObject(exception)) {
        JSValue stackVal = JS_GetPropertyStr(ctx, exception, "stack");
        const char* s = JS_ToCString(ctx, stackVal);
        if (s) { stack = s; JS_FreeCString(ctx, s); }
        JS_FreeValue(ctx, stackVal);
    }
    std::string topFrame = firstStackFrame(stack);

    // Host hook first — apps can fully suppress.
    if (dispatchHostHook(ctx, origin, message, exception)) return;

    // Build header string. This is also what the suppressor stores so the
    // "still failing" tail can echo it without re-extracting.
    std::string header = std::string("[js error] ") + name + ": " + message;
    if (!origin.label.empty()) {
        header += "  (";
        header += originName(origin.kind);
        header += ": ";
        header += origin.label;
        header += ")";
    } else {
        header += "  (";
        header += originName(origin.kind);
        header += ")";
    }

    // Suppressor key: fingerprint by (name, message, top-frame). Origin is
    // intentionally excluded so the same TypeError from a timer and a
    // listener still suppress as one entry.
    std::string fingerprint = name + "|" + message + "|" + topFrame;
    if (!noteOccurrence(fingerprint, header)) return;

    // First occurrence — full multi-line log.
    if (!stack.empty()) {
        // Indent the stack so it visually nests under the header.
        std::string indented;
        indented.reserve(stack.size() + 16);
        size_t pos = 0;
        while (pos < stack.size()) {
            size_t nl = stack.find('\n', pos);
            std::string line = stack.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            // Skia stack lines often already start with "    at "; just prefix
            // a couple of spaces for clarity.
            indented += "  ";
            indented += line;
            if (nl == std::string::npos) break;
            indented += "\n";
            pos = nl + 1;
        }
        LOG_ERROR("%s\n%s", header.c_str(), indented.c_str());
    } else {
        LOG_ERROR("%s", header.c_str());
    }
}

// QuickJS host promise rejection tracker. QuickJS calls this the moment a
// promise rejects with no reactions attached (is_handled = false) and again
// if a handler shows up later (is_handled = true). Judgment is deferred to
// the end of the microtask burst — see Runtime::flushPendingRejections().
void hostPromiseRejectionTracker(JSContext* ctx, JSValueConst promise,
                                 JSValueConst reason, bool is_handled,
                                 void* opaque)
{
    auto* self = static_cast<Runtime*>(opaque);
    if (!self) return;
    if (is_handled) {
        self->removePendingRejection(promise);
    } else {
        self->addPendingRejection(ctx, promise, reason);
    }
}

} // anonymous namespace

ErrorOrigin ErrorOrigin::timer(int32_t id, bool repeating) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s#%d", repeating ? "setInterval" : "setTimeout", id);
    return {ErrorOriginKind::Timer, buf};
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

Runtime::Runtime()
{
    rt_ = JS_NewRuntime();
    if (!rt_) {
        LOG_ERROR("Failed to create QuickJS runtime");
        return;
    }

    // 4 GB: high enough that apps shuttling ML tensors over the JS boundary
    // (a single Krea 2 raw-taps buffer is a 63 MB Float32Array; workers cache
    // several) never hit it, low enough to still catch a runaway allocation
    // loop. The old 256 MB cap OOM'd the krea2-lab worker on its second
    // expression field.
    JS_SetMemoryLimit(rt_, 4ull * 1024 * 1024 * 1024);
    JS_SetMaxStackSize(rt_, 8 * 1024 * 1024);

    bro::util::installJsInterruptHandler(rt_);

    // Route unhandled promise rejections through the funnel (deferred to
    // end-of-burst — the opaque is this Runtime's pending-rejection list).
    JS_SetHostPromiseRejectionTracker(rt_, hostPromiseRejectionTracker, this);

#ifndef NDEBUG
    JS_SetDumpFlags(rt_, JS_DUMP_LEAKS | JS_DUMP_ATOM_LEAKS);
    // The leak dumps print to stdout with plain printf inside JS_FreeRuntime,
    // immediately before its `assert(list_empty(&rt->gc_obj_list))`. When
    // stdout is a pipe it is fully buffered, and the assert's abort() exits
    // without flushing — the dump that names the leaked objects is exactly
    // the output that gets lost. Unbuffered stdout in Debug keeps it.
    setvbuf(stdout, nullptr, _IONBF, 0);
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
    // Parked rejections hold context refs — release them before the runtime
    // dies. No reporting: teardown is not the place to run JS error hooks.
    discardPendingRejections();
    if (ctx_) {
        brokit::api::uninstallFetch(ctx_);
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
    if (checkException(ctx_, result, ErrorOrigin::eval(filename))) {
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
    if (checkException(ctx_, func, ErrorOrigin::module(filename))) {
        return false;
    }

    JSValue result = JS_EvalFunction(ctx_, func);
    if (checkException(ctx_, result, ErrorOrigin::module(filename))) {
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

void Runtime::addPendingRejection(JSContext* ctx, JSValueConst promise,
                                  JSValueConst reason)
{
    PendingRejection p;
    p.ctx = JS_DupContext(ctx);
    p.key = JS_VALUE_GET_PTR(promise);
    // Retaining the promise keeps its heap pointer stable (no GC + reuse
    // between the reject and a later is_handled notification).
    p.promise = JS_DupValue(ctx, promise);
    p.reason = JS_DupValue(ctx, reason);
    pendingRejections_.push_back(p);
}

void Runtime::removePendingRejection(JSValueConst promise)
{
    void* key = JS_VALUE_GET_PTR(promise);
    for (auto it = pendingRejections_.begin(); it != pendingRejections_.end(); ++it) {
        if (it->key == key) {
            JS_FreeValue(it->ctx, it->promise);
            JS_FreeValue(it->ctx, it->reason);
            JS_FreeContext(it->ctx);
            pendingRejections_.erase(it);
            return;
        }
    }
}

void Runtime::flushPendingRejections()
{
    // Swap-and-iterate: the funnel may run window.onunhandledrejection,
    // which can park fresh rejections; those belong to the next flush.
    std::vector<PendingRejection> batch;
    batch.swap(pendingRejections_);
    for (auto& p : batch) {
        if (!isInterruptException(p.ctx, p.reason)) {
            emitErrorLog(p.ctx, ErrorOrigin::promiseRejection(), p.reason);
        }
        JS_FreeValue(p.ctx, p.promise);
        JS_FreeValue(p.ctx, p.reason);
        JS_FreeContext(p.ctx);
    }
}

void Runtime::discardPendingRejections()
{
    for (auto& p : pendingRejections_) {
        JS_FreeValue(p.ctx, p.promise);
        JS_FreeValue(p.ctx, p.reason);
        JS_FreeContext(p.ctx);
    }
    pendingRejections_.clear();
}

void Runtime::executePendingJobs()
{
    JSContext* pctx = nullptr;
    for (;;) {
        int r = JS_ExecutePendingJob(rt_, &pctx);
        if (r == 0) {
            // Queue empty — the burst is over. Anything still parked was
            // rejected and never picked up a handler: report it now. The
            // funnel may queue new jobs (onunhandledrejection), so loop.
            if (pendingRejections_.empty()) break;
            flushPendingRejections();
            continue;
        }
        if (r < 0) {
            // A microtask threw. Pull the exception off `pctx` and route it.
            // QuickJS leaves the exception on the context that ran the job.
            JSValue exception = JS_GetException(pctx);
            if (!isInterruptException(pctx, exception)) {
                emitErrorLog(pctx, ErrorOrigin::microtask(), exception);
            }
            JS_FreeValue(pctx, exception);
            // Continue draining — other microtasks should still run.
        }
    }
}

JSContext* Runtime::createContext()
{
    if (!rt_) return nullptr;
    return JS_NewContext(rt_);
}

JSContext* Runtime::renewContext()
{
    if (!rt_) return nullptr;
    // Report anything the dying realm left unhandled while it can still be
    // formatted, and drop the context refs so JS_RunGC below can collect it.
    flushPendingRejections();
    if (ctx_) {
        // Same order ~Runtime uses: brokit's fetch state is the one per-context
        // resource the runtime owns the teardown of.
        brokit::api::uninstallFetch(ctx_);
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    // Collect cycles the freed realm left behind before the new one allocates.
    JS_RunGC(rt_);
    ctx_ = JS_NewContext(rt_);
    if (!ctx_) LOG_ERROR("renewContext: failed to create QuickJS context");
    return ctx_;
}

void Runtime::setModuleLoader(const util::AssetMounts* mounts)
{
    // The mounts pointer is handed to QuickJS as the loader opaque and reaches
    // both module_normalize and module_loader. It must outlive module eval.
    JS_SetModuleLoaderFunc(rt_, module_normalize, module_loader,
                           const_cast<util::AssetMounts*>(mounts));
}

JSValue Runtime::callJs(JSContext* ctx, JSValueConst fn, JSValueConst thisVal,
                        int argc, JSValueConst* argv, const ErrorOrigin& origin)
{
    JSValue ret = JS_Call(ctx, fn, thisVal, argc, argv);
    if (JS_IsException(ret)) {
        JSValue exception = JS_GetException(ctx);
        if (!isInterruptException(ctx, exception)) {
            emitErrorLog(ctx, origin, exception);
        }
        JS_FreeValue(ctx, exception);
        return JS_UNDEFINED;
    }
    return ret;
}

bool Runtime::checkException(JSContext* ctx, JSValue val, const ErrorOrigin& origin)
{
    if (!JS_IsException(val)) return false;

    JSValue exception = JS_GetException(ctx);
    if (!isInterruptException(ctx, exception)) {
        emitErrorLog(ctx, origin, exception);
    }
    JS_FreeValue(ctx, exception);
    return true;
}

void Runtime::flushSuppressor()
{
    flushAged(bro::util::currentTimeMs());
}

} // namespace bro::js
