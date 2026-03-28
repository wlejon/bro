#include "js/timers.h"
#include "js/runtime.h"
#include "util/log.h"

#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Helpers – store / retrieve the Timers* via JS context opaque
// ---------------------------------------------------------------------------

// We stash the Timers pointer on a well-known property of globalThis so we
// don't collide with other opaque users.  A hidden Symbol would be nicer,
// but a simple string property with an unusual name is fine for now.

static const char* kTimersKey = "__bro_timers_ptr";

static Timers* getTimers(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kTimersKey);
    Timers* t = nullptr;
    if (JS_IsNumber(val)) {
        // Stored as a double-punned pointer – use intptr_t.
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        t = reinterpret_cast<Timers*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return t;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Timers::~Timers()
{
    // Caller should have called clearAll() before destroying the context.
    // We cannot free JSValues here without a context, so just warn.
    if (!timers_.empty()) {
        LOG_WARN("Timers destroyed with %zu pending timers", timers_.size());
    }
}

void Timers::clearAll(JSContext* ctx)
{
    for (auto& [id, entry] : timers_) {
        JS_FreeValue(ctx, entry.callback);
    }
    timers_.clear();
}

// ---------------------------------------------------------------------------
// Core logic
// ---------------------------------------------------------------------------

int32_t Timers::addTimer(JSContext* ctx, JSValue callback, double delayMs,
                         bool repeating, double nowMs)
{
    if (delayMs < 0) delayMs = 0;

    int32_t id = nextId_++;
    TimerEntry entry;
    entry.callback     = JS_DupValue(ctx, callback);
    entry.ctx          = ctx;
    entry.intervalMs   = delayMs;
    entry.nextFireTime = nowMs + delayMs;
    entry.repeating    = repeating;
    timers_[id] = entry;
    return id;
}

void Timers::removeTimer(int32_t id)
{
    auto it = timers_.find(id);
    if (it != timers_.end()) {
        JS_FreeValue(it->second.ctx, it->second.callback);
        timers_.erase(it);
    }
}

void Timers::tick(double currentTimeMs)
{
    // Collect expired timer ids first to avoid modifying the map while iterating.
    std::vector<int32_t> expired;
    for (auto& [id, entry] : timers_) {
        if (currentTimeMs >= entry.nextFireTime) {
            expired.push_back(id);
        }
    }

    for (int32_t id : expired) {
        auto it = timers_.find(id);
        if (it == timers_.end())
            continue; // may have been removed by a previous callback

        TimerEntry& entry = it->second;
        JSContext* ctx = entry.ctx;

        JSValue ret = JS_Call(ctx, entry.callback, JS_UNDEFINED, 0, nullptr);
        if (Runtime::checkException(ctx, ret)) {
            // Exception already logged
        } else {
            JS_FreeValue(ctx, ret);
        }

        // Re-lookup – callback may have cleared itself
        it = timers_.find(id);
        if (it == timers_.end())
            continue;

        if (it->second.repeating) {
            it->second.nextFireTime = currentTimeMs + it->second.intervalMs;
        } else {
            JS_FreeValue(ctx, it->second.callback);
            timers_.erase(it);
        }
    }
}

// ---------------------------------------------------------------------------
// JS C-function callbacks
// ---------------------------------------------------------------------------

JSValue Timers::js_setTimeout(JSContext* ctx, JSValueConst /*this_val*/,
                              int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "setTimeout: first argument must be a function");
    }

    double delay = 0;
    if (argc >= 2)
        JS_ToFloat64(ctx, &delay, argv[1]);

    Timers* t = getTimers(ctx);
    if (!t) return JS_UNDEFINED;

    // Use 0 as "now" – the caller's tick() supplies real time.
    int32_t id = t->addTimer(ctx, argv[0], delay, false, 0);
    return JS_NewInt32(ctx, id);
}

JSValue Timers::js_setInterval(JSContext* ctx, JSValueConst /*this_val*/,
                               int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "setInterval: first argument must be a function");
    }

    double delay = 0;
    if (argc >= 2)
        JS_ToFloat64(ctx, &delay, argv[1]);

    Timers* t = getTimers(ctx);
    if (!t) return JS_UNDEFINED;

    int32_t id = t->addTimer(ctx, argv[0], delay, true, 0);
    return JS_NewInt32(ctx, id);
}

JSValue Timers::js_clearTimeout(JSContext* ctx, JSValueConst /*this_val*/,
                                int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;

    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);

    Timers* t = getTimers(ctx);
    if (t) t->removeTimer(id);
    return JS_UNDEFINED;
}

JSValue Timers::js_clearInterval(JSContext* ctx, JSValueConst /*this_val*/,
                                 int argc, JSValueConst* argv)
{
    // clearInterval and clearTimeout are interchangeable per spec.
    return js_clearTimeout(ctx, JS_UNDEFINED, argc, argv);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void Timers::install(JSContext* ctx, Timers* instance)
{
    JSValue global = JS_GetGlobalObject(ctx);

    // Stash the pointer so the C callbacks can find it.
    JS_SetPropertyStr(ctx, global, kTimersKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(instance))));

    JS_SetPropertyStr(ctx, global, "setTimeout",
                      JS_NewCFunction(ctx, js_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
                      JS_NewCFunction(ctx, js_setInterval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                      JS_NewCFunction(ctx, js_clearTimeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
                      JS_NewCFunction(ctx, js_clearInterval, "clearInterval", 1));

    JS_FreeValue(ctx, global);
}

} // namespace bro::js
