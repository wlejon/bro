#pragma once

// The bronze host layer's shared runtime: the frame seam every host-provided
// global hangs off, and the three things the files on it must share — the
// error funnel, the frame clock, and the main-thread task queue.
//
// host_builder.h is the other half: ObjectBuilder and the argument readers.
// A file that needs both includes both.
//
// THE GC RULE, restated because most of the host layer exists to obey it: a
// Value held across an allocating embed call is stale. Host state that must
// outlive such a call lives in an ev::Persistent — and a Persistent must never
// be owned by anything a HANDLE FINALIZER destroys. A finalizer runs
// mid-collection and may not call back into the embed API (embed.h says so),
// and ~Persistent IS the embed API. That single rule is why the payload
// structs in this layer are plain host memory and every callback lives as an
// ordinary property on the object instead.

#include "embed/embed.h"

#include <cstdint>
#include <functional>
#include <string>

namespace bro::engine {
class Engine;
}  // namespace bro::engine

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// ---------------------------------------------------------------------------
// The error funnel and the frame clock (dom_globals.cpp)
// ---------------------------------------------------------------------------

// Where an exception out of compiled code ends up: reported to the engine log
// and dropped, so one broken callback never silences its siblings or tears the
// loop down.
void reportBronzeError(const char* origin, Value thrown);

// The browser's "report the exception" step for `thrown`: window.onerror,
// then an ErrorEvent at the window's `error` listeners (host_error_events.cpp).
// True when the page cancelled it (onerror returned true, or a listener
// called preventDefault), which is the caller's cue to skip its log line.
// A throw from inside an error handler is never re-dispatched. ALLOCATES.
bool hostDispatchUncaughtError(Value thrown);

// The text `reportBronzeError` prints for a thrown value: its `stack` when a
// program set one, else `Name: message` for an Error, else its JSON for any
// other object, else ToString of the primitive. bronze itself records no
// stack and no source position on an Error (runtime/exception.h), so this
// is the whole of what a report can say about WHAT was thrown; the caller
// supplies WHERE (the script, the seam). ALLOCATES.
std::string thrownValueText(Value thrown);

// The Engine this layer was installed on, or nullptr before install. Every
// file here reaches the engine through it rather than through a second copy
// of the pointer.
engine::Engine* hostEngine();

// __host.memory (host_mem_probe.cpp): committed memory by holder, and with
// `sizes` every live heap block size with its count.
Value hostMemoryBreakdown(bool sizes);

// Milliseconds of SCALED engine time since installWebHostGlobals: the
// accumulated Engine::onFrame deltas. This is the clock rAF timestamps and
// performance.now() answer from, and the one timer deadlines are measured
// against — so it is frozen while bro.time is paused and virtual under
// headless advanceTime, exactly as the clock bro's own JS gets is. A compiled
// app and a JS app in the same engine therefore agree about what "now" is.
double hostClockMs();

// ---------------------------------------------------------------------------
// The main-thread task queue (host_timers.cpp)
// ---------------------------------------------------------------------------

// Where a host binding puts work that must not run inside the call that
// produced it: an image's load event. Drained once per
// frame at the top of the bronze frame seam, BEFORE requestAnimationFrame —
// which is where the web runs a load event relative to the rendering steps, and
// what lets a texture that finished decoding be uploaded by the same frame that
// learns about it.
//
// Main thread only, by construction rather than by locking: nothing in this
// layer runs off it (host_image.cpp says why its decode is synchronous), so a
// mutex here would be a claim about threads that is not true. A future producer
// that really is off-thread must add the lock AND state what it protects.
void postHostTask(std::function<void()> task);
void drainHostTasks();

// A DEADLINE for host work, on the same table and the same clock the app's own
// setTimeout uses — so a host-scheduled abort and an app-scheduled one are
// ordered against each other rather than against two different notions of now.
// The callback is host memory freed on the main thread as the entry is erased,
// never from a finalizer, so unlike a JS listener it may hold Persistents.
// Answers the id, which nothing needs yet; the symmetry with clearTimeout is
// the point of returning it rather than a promise of one.
int32_t hostSetTimeout(std::function<void()> task, double delayMs);

// ---------------------------------------------------------------------------
// Timers (host_timers.cpp)
// ---------------------------------------------------------------------------

// setTimeout / clearTimeout / setInterval / clearInterval, on hostClockMs().
void installTimerGlobals();

// Fire every timer whose deadline has passed, in (deadline, id) order — HTML's
// order for same-deadline timers is creation order. Called once per frame from
// the bronze frame seam, before requestAnimationFrame.
void fireHostTimers(double nowMs);

// The earliest timer deadline on hostClockMs() (+infinity with none; minus
// infinity while a host task waits to be drained).
double nextHostTimerDueMs();

}  // namespace bro::bronze_host
