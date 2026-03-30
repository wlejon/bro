#include "js/timers.h"
#include "js/runtime.h"
#include "util/log.h"
#include "util/time.h"

#include <vector>
#include <algorithm>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Helpers – store / retrieve the Timers* via JS context opaque
// ---------------------------------------------------------------------------

static const char* kTimersKey = "__bro_timers_ptr";

static Timers* getTimers(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kTimersKey);
    Timers* t = nullptr;
    if (JS_IsNumber(val)) {
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
    if (!timers_.empty()) {
        LOG_WARN("Timers destroyed with %zu pending timers", timers_.size());
    }
    if (!rafPending_.empty()) {
        LOG_WARN("Timers destroyed with %zu pending rAF callbacks", rafPending_.size());
    }
}

void Timers::clearAll(JSContext* ctx)
{
    for (auto& [id, entry] : timers_) {
        JS_FreeValue(ctx, entry.callback);
    }
    timers_.clear();

    for (auto& entry : rafPending_) {
        JS_FreeValue(ctx, entry.callback);
    }
    rafPending_.clear();
}

// ---------------------------------------------------------------------------
// Timer core logic
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
    lastTickMs_ = currentTimeMs;
    std::vector<int32_t> expired;
    for (auto& [id, entry] : timers_) {
        if (currentTimeMs >= entry.nextFireTime) {
            expired.push_back(id);
        }
    }

    for (int32_t id : expired) {
        auto it = timers_.find(id);
        if (it == timers_.end())
            continue;

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
// requestAnimationFrame core logic
// ---------------------------------------------------------------------------

int32_t Timers::addAnimationFrame(JSContext* ctx, JSValue callback)
{
    int32_t id = nextRafId_++;
    RafEntry entry;
    entry.id       = id;
    entry.callback = JS_DupValue(ctx, callback);
    entry.ctx      = ctx;
    rafPending_.push_back(entry);
    return id;
}

void Timers::removeAnimationFrame(int32_t id)
{
    auto it = std::find_if(rafPending_.begin(), rafPending_.end(),
                           [id](const RafEntry& e) { return e.id == id; });
    if (it != rafPending_.end()) {
        JS_FreeValue(it->ctx, it->callback);
        rafPending_.erase(it);
    }
}

void Timers::fireAnimationFrames(double timestampMs)
{
    if (rafPending_.empty()) return;

    // Move current callbacks out so new rAF calls during firing go to next frame
    std::vector<RafEntry> current = std::move(rafPending_);
    rafPending_.clear();

    for (auto& entry : current) {
        JSValue ts = JS_NewFloat64(entry.ctx, timestampMs);
        JSValue ret = JS_Call(entry.ctx, entry.callback, JS_UNDEFINED, 1, &ts);
        JS_FreeValue(entry.ctx, ts);
        if (Runtime::checkException(entry.ctx, ret)) {
            // Exception already logged
        } else {
            JS_FreeValue(entry.ctx, ret);
        }
        JS_FreeValue(entry.ctx, entry.callback);
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

    double now = bro::util::currentTimeMs();
    int32_t id = t->addTimer(ctx, argv[0], delay, false, now);
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

    double now = bro::util::currentTimeMs();
    int32_t id = t->addTimer(ctx, argv[0], delay, true, now);
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
    return js_clearTimeout(ctx, JS_UNDEFINED, argc, argv);
}

JSValue Timers::js_requestAnimationFrame(JSContext* ctx, JSValueConst /*this_val*/,
                                         int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "requestAnimationFrame: first argument must be a function");
    }

    Timers* t = getTimers(ctx);
    if (!t) return JS_UNDEFINED;

    int32_t id = t->addAnimationFrame(ctx, argv[0]);
    return JS_NewInt32(ctx, id);
}

JSValue Timers::js_cancelAnimationFrame(JSContext* ctx, JSValueConst /*this_val*/,
                                        int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;

    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);

    Timers* t = getTimers(ctx);
    if (t) t->removeAnimationFrame(id);
    return JS_UNDEFINED;
}

JSValue Timers::js_performanceNow(JSContext* ctx, JSValueConst /*this_val*/,
                                  int /*argc*/, JSValueConst* /*argv*/)
{
    // Return the last tick time so headless virtual time works correctly.
    // In windowed mode, tick() is called with currentTimeMs() so this
    // returns real wall-clock time. In headless, it returns virtual time.
    Timers* t = getTimers(ctx);
    if (t && t->lastTickMs_ > 0.0)
        return JS_NewFloat64(ctx, t->lastTickMs_);
    return JS_NewFloat64(ctx, bro::util::currentTimeMs());
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
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
                      JS_NewCFunction(ctx, js_requestAnimationFrame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
                      JS_NewCFunction(ctx, js_cancelAnimationFrame, "cancelAnimationFrame", 1));

    // performance.now()
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now",
                      JS_NewCFunction(ctx, js_performanceNow, "now", 0));
    JS_SetPropertyStr(ctx, global, "performance", perf);

    JS_FreeValue(ctx, global);
}

} // namespace bro::js
