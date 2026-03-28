#pragma once

#include <cstdint>
#include <map>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

class Timers {
public:
    Timers() = default;
    ~Timers();

    // Non-copyable
    Timers(const Timers&) = delete;
    Timers& operator=(const Timers&) = delete;

    /// Register setTimeout, setInterval, clearTimeout, clearInterval on the
    /// global object. `instance` must outlive the JSContext.
    static void install(JSContext* ctx, Timers* instance);

    /// Advance time – fires any expired timers, reschedules repeating ones.
    void tick(double currentTimeMs);

    /// Cancel all pending timers (called on teardown).
    void clearAll(JSContext* ctx);

private:
    struct TimerEntry {
        JSValue   callback;     // DupValue'd
        JSContext* ctx;
        double    intervalMs;
        double    nextFireTime;
        bool      repeating;
    };

    int32_t nextId_ = 1;
    std::map<int32_t, TimerEntry> timers_;

    int32_t addTimer(JSContext* ctx, JSValue callback, double delayMs,
                     bool repeating, double nowMs);
    void    removeTimer(int32_t id);

    // C callbacks
    static JSValue js_setTimeout(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv);
    static JSValue js_setInterval(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv);
    static JSValue js_clearTimeout(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv);
    static JSValue js_clearInterval(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv);
};

} // namespace bro::js
