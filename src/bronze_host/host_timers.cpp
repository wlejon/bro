// setTimeout / clearTimeout / setInterval / clearInterval for a bronze-compiled
// app, plus the main-thread task queue the other host bindings deliver
// completions through.
//
// WHY NOT js::Timers. bro's own timer table (src/js/timers.h) stores JSValue
// callbacks and calls them through QuickJS — there is no non-QuickJS entry
// point on it, and adding one would put a bronze Value in a JSValue field.
// What matters for parity is not the table but the CLOCK, and this one runs on
// exactly the clock bro's JS gets: hostClockMs(), the accumulated
// Engine::onFrame deltas, which is engineNowMs_ scaled by bro.time. A compiled
// app and a JS app in the same engine therefore see time stop together when the
// app is paused, stretch together under a timescale, and step together under
// headless advanceTime.
//
// WHERE THEY FIRE. fireHostTimers runs once per frame from the bronze frame
// seam, before requestAnimationFrame — the same order bro's own loop uses
// (engine_frame.cpp: timers_->tick at step 2, fireAnimationFrames at step 3a).
// Resolution is therefore one frame: a 1 ms timeout and a 5 ms timeout set in
// the same turn both fire on the next frame, in creation order. That is the
// honest consequence of having exactly one host seam per frame, and it is
// stated here rather than papered over with a sub-frame poll that the engine
// would never call.
//
// LIFETIME. A timer's callback lives in an ev::Persistent, which is a GC root.
// A one-shot's entry is erased as it fires, so its root goes with it. An
// interval's does NOT: it is rooted until clearInterval(id) removes it, or the
// process exits. That is the web's behaviour too — an uncleared interval keeps
// its whole closure alive forever — but on the web the tab eventually closes,
// and here the only bound is the process. Nothing reaps them, deliberately:
// a host that dropped a timer the app never cleared would be a silent
// behaviour change, not a leak fix. The warning below is the diagnostic.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // argAt / numAt / i32At

#include "util/log.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace bro::bronze_host {

namespace {

struct TimerEntry {
    int32_t id = 0;
    ev::Persistent fn;
    // setTimeout(fn, delay, a, b) hands a and b to fn. three.js never uses
    // them, but a callback silently called with none is the kind of divergence
    // that surfaces as a wrong render rather than an error.
    std::vector<ev::Persistent> args;
    double dueMs = 0.0;
    double intervalMs = 0.0;
    bool repeating = false;
};

// Process-lived and never freed, the same convention HostState follows
// (dom_globals.cpp): these hold ev::Persistents, and a static destructor
// running at process exit would release root slots against a runtime whose own
// statics may already be gone.
std::vector<TimerEntry>* g_timers = nullptr;
std::deque<std::function<void()>>* g_tasks = nullptr;

int32_t g_nextTimerId = 1;
bool g_timerCountWarned = false;

// High enough that no honest app reaches it and low enough to catch a runaway
// while the log is still readable. Warned once: the point is to name the leak,
// not to narrate it.
constexpr size_t kTimerCountWarnAt = 1024;

std::vector<TimerEntry>& timers() {
    if (!g_timers) g_timers = new std::vector<TimerEntry>();
    return *g_timers;
}

std::deque<std::function<void()>>& tasks() {
    if (!g_tasks) g_tasks = new std::deque<std::function<void()>>();
    return *g_tasks;
}

Value addTimer(std::span<const Value> a, bool repeating) {
    Value fn = argAt(a, 0);
    if (!ev::isFunction(fn)) {
        // The web coerces a string first argument to code. bronze has no eval
        // and never will, so this is a named refusal rather than a timer that
        // silently never fires.
        return ev::throwTypeError(
            "setTimeout/setInterval: the first argument must be a function");
    }

    TimerEntry entry;
    entry.id = g_nextTimerId++;
    entry.fn = ev::Persistent(fn);
    for (size_t i = 2; i < a.size(); ++i) entry.args.emplace_back(a[i]);

    // HTML clamps a negative or non-finite delay to 0; NaN takes the same road
    // because every comparison against it is false.
    double delay = numAt(a, 1);
    if (!(delay > 0.0)) delay = 0.0;
    entry.dueMs = hostClockMs() + delay;
    entry.intervalMs = delay;
    entry.repeating = repeating;

    auto& list = timers();
    list.push_back(std::move(entry));
    if (!g_timerCountWarned && list.size() >= kTimerCountWarnAt) {
        g_timerCountWarned = true;
        LOG_WARN("bronze_host: %zu live timers. Every one roots its callback and "
                 "everything the callback closes over until clearTimeout/"
                 "clearInterval removes it; nothing else reaps them.",
                 list.size());
    }
    return ev::fromDouble(static_cast<double>(list.back().id));
}

Value clearTimer(std::span<const Value> a) {
    const int32_t id = i32At(a, 0);
    auto& list = timers();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->id == id) {
            list.erase(it);
            break;
        }
    }
    return ev::undefined();
}

}  // namespace

// ---------------------------------------------------------------------------
// The main-thread task queue
// ---------------------------------------------------------------------------

void postHostTask(std::function<void()> task) {
    tasks().push_back(std::move(task));
}

void drainHostTasks() {
    auto& queue = tasks();
    if (queue.empty()) return;
    // Move the batch out before running any of it, so a task that posts another
    // task — an onload handler that starts the next load — queues it for the
    // NEXT frame and one frame cannot starve on a self-feeding chain. Same rule
    // the rAF queue follows (dom_globals.cpp), for the same reason.
    std::deque<std::function<void()>> batch;
    batch.swap(queue);
    for (auto& task : batch) task();
}

// ---------------------------------------------------------------------------
// Firing
// ---------------------------------------------------------------------------

void fireHostTimers(double nowMs) {
    auto& list = timers();
    if (list.empty()) return;

    // Decide the batch BEFORE running any of it. A callback that adds a timer
    // must not have it fire in the same tick (that is how a setTimeout(0) loop
    // becomes an infinite one), and a callback that clears a sibling must be
    // obeyed — which the re-find by id below is what honours.
    struct Due {
        double dueMs;
        int32_t id;
    };
    std::vector<Due> due;
    for (const TimerEntry& entry : list) {
        if (entry.dueMs <= nowMs) due.push_back({entry.dueMs, entry.id});
    }
    if (due.empty()) return;
    // HTML fires same-deadline timers in creation order, and ids are handed out
    // in creation order.
    std::sort(due.begin(), due.end(), [](const Due& x, const Due& y) {
        return x.dueMs != y.dueMs ? x.dueMs < y.dueMs : x.id < y.id;
    });

    for (const Due& d : due) {
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const TimerEntry& e) { return e.id == d.id; });
        if (it == list.end()) continue;  // cleared by an earlier callback

        // Take what the call needs, then settle the table BEFORE calling. A
        // one-shot is already gone, so clearTimeout from inside its own
        // callback is the no-op it is on the web; a repeat already carries its
        // next deadline, so clearInterval from inside removes a live entry and
        // actually stops it. Settling afterwards would resurrect a timer the
        // callback had just cleared.
        ev::Persistent fn = it->fn;
        std::vector<ev::Persistent> args = it->args;
        if (it->repeating) {
            // Advance from the DEADLINE, so a long frame does not stretch the
            // interval — but skip whole missed periods instead of firing a
            // catch-up burst: a 16 ms interval across a 500 ms stall is thirty
            // calls nobody asked for, and the engine's own clock already
            // freezes rather than accumulating while paused.
            double next = it->dueMs + it->intervalMs;
            if (next <= nowMs) next = nowMs + it->intervalMs;
            it->dueMs = next;
        } else {
            list.erase(it);
        }
        // `it` is dead from here on — the call below can push onto `list`
        // (another setTimeout) and reallocate it. Nothing after this point
        // touches the iterator; the next round re-finds by id.

        std::vector<Value> argv;
        argv.reserve(args.size());
        for (ev::Persistent& arg : args) argv.push_back(arg.get());
        // Nothing has allocated since the argv reads, so every Value in it is
        // current when `call` roots them.
        ev::CallResult r = ev::call(fn.get(), ev::undefined(),
                                    std::span<const Value>(argv.data(), argv.size()));
        if (r.thrown) reportBronzeError("timer", r.value);
    }
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installTimerGlobals() {
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return addTimer(a, /*repeating=*/false); },
            2);
        ev::registerGlobal("setTimeout", fn);
    }
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return clearTimer(a); }, 1);
        ev::registerGlobal("clearTimeout", fn);
    }
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return addTimer(a, /*repeating=*/true); },
            2);
        ev::registerGlobal("setInterval", fn);
    }
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return clearTimer(a); }, 1);
        ev::registerGlobal("clearInterval", fn);
    }
}

}  // namespace bro::bronze_host
