#include "js/timers.h"
#include "js/runtime.h"
#include "util/log.h"
#include "util/time.h"

#include <qjsbind/qjsbind.h>

#include <vector>
#include <algorithm>

namespace bro::js {

// ---------------------------------------------------------------------------
// Helpers – store / retrieve the Timers* via JS context opaque.
//
// Per-context (not per-thread) because system panels run multiple JS
// contexts on the engine main thread. Any other consumer on the same
// context must cooperate (e.g. Worker stores its own state elsewhere).
// ---------------------------------------------------------------------------

static Timers* getTimers(JSContext* ctx)
{
    return static_cast<Timers*>(JS_GetContextOpaque(ctx));
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

void Timers::clearForContext(JSContext* ctx)
{
    for (auto it = timers_.begin(); it != timers_.end(); ) {
        if (it->second.ctx == ctx) {
            JS_FreeValue(ctx, it->second.callback);
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = rafPending_.begin(); it != rafPending_.end(); ) {
        if (it->ctx == ctx) {
            JS_FreeValue(ctx, it->callback);
            it = rafPending_.erase(it);
        } else {
            ++it;
        }
    }
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

        // Copy everything we need out of the map before calling JS.
        // The callback may clear itself or add/remove other timers,
        // which could invalidate the map iterators or the entry reference.
        JSContext* ctx = it->second.ctx;
        JSValue callback = JS_DupValue(ctx, it->second.callback);
        bool repeating = it->second.repeating;
        double intervalMs = it->second.intervalMs;

        JSValue ret = Runtime::callJs(ctx, callback, JS_UNDEFINED, 0, nullptr,
                                      ErrorOrigin::timer(id, repeating));
        JS_FreeValue(ctx, ret);

        JS_FreeValue(ctx, callback);

        // Re-lookup – callback may have cleared itself or the context might have been shut down.
        it = timers_.find(id);
        if (it == timers_.end())
            continue;

        if (repeating) {
            it->second.nextFireTime = currentTimeMs + intervalMs;
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
        JSValue ret = Runtime::callJs(entry.ctx, entry.callback, JS_UNDEFINED, 1, &ts,
                                      ErrorOrigin::raf());
        JS_FreeValue(entry.ctx, ts);
        JS_FreeValue(entry.ctx, ret);
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

    // Use lastTickMs_ so timers work correctly with both real and virtual time.
    double now = (t->lastTickMs_ > 0.0) ? t->lastTickMs_ : bro::util::currentTimeMs();
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

    double now = (t->lastTickMs_ > 0.0) ? t->lastTickMs_ : bro::util::currentTimeMs();
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
    JS_SetContextOpaque(ctx, instance);

    qjsbind::Global(ctx)
        .function("setTimeout", js_setTimeout, 2)
        .function("setInterval", js_setInterval, 2)
        .function("clearTimeout", js_clearTimeout, 1)
        .function("clearInterval", js_clearInterval, 1)
        .function("requestAnimationFrame", js_requestAnimationFrame, 1)
        .function("cancelAnimationFrame", js_cancelAnimationFrame, 1);

    // Install performance.now() so it returns engine-tracked time (real in
    // windowed, virtual in headless). QuickJS's own built-in `performance`
    // (JS_AddPerformance in quickjs.c) defines `.now` with JS_PROP_ENUMERABLE
    // only — no JS_PROP_WRITABLE/CONFIGURABLE — so it can never be overwritten
    // in place; a plain JS_SetPropertyStr against it silently no-ops (sloppy-
    // mode [[Set]] against a non-writable property just returns false). The
    // outer `performance` property on globalThis IS writable+configurable
    // (brokit/QuickJS both define it that way), so instead of mutating the
    // existing object we build a fresh one — copying over any extra
    // properties brokit's polyfill already added (mark/measure/getEntries*/
    // timeOrigin) — and replace globalThis.performance wholesale.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue oldPerf = JS_GetPropertyStr(ctx, global, "performance");
    JSValue newPerf = JS_NewObject(ctx);

    if (!JS_IsUndefined(oldPerf) && !JS_IsNull(oldPerf)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, oldPerf,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                JSAtom atom = props[i].atom;
                const char* name = JS_AtomToCString(ctx, atom);
                if (name && strcmp(name, "now") != 0) {
                    JSValue v = JS_GetProperty(ctx, oldPerf, atom);
                    JS_SetPropertyStr(ctx, newPerf, name, v);
                }
                if (name) JS_FreeCString(ctx, name);
                JS_FreeAtom(ctx, atom);
            }
            js_free(ctx, props);
        }
    }
    JS_FreeValue(ctx, oldPerf);

    JS_SetPropertyStr(ctx, newPerf, "now",
        JS_NewCFunction(ctx, js_performanceNow, "now", 0));
    JS_SetPropertyStr(ctx, global, "performance", newPerf);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
