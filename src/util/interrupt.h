#pragma once

#include <atomic>

struct JSRuntime;

/// Process-wide interrupt flag + Ctrl+C wiring.
///
/// When the user hits Ctrl+C, a signal handler sets a global atomic flag.
/// Every QuickJS runtime (main thread + each Worker) installs an interrupt
/// handler that reads this flag and, when set, tells QuickJS to break out of
/// whatever JS is currently executing (raising an uncatchable
/// InternalError: "interrupted"). Event loops that aren't currently running
/// JS also poll the flag so tight C++ sleep/wait loops exit too.
///
/// A second Ctrl+C hard-exits the process, in case JS is misbehaving and
/// swallowing the interrupt somehow.
namespace bro::util {

/// Returns true once the user has requested interruption.
bool interrupted();

/// Request interruption. First call sets the flag (JS bails out at the next
/// interrupt poll, event loops drop out); a second call hard-exits the
/// process. Same path as Ctrl+C / SIGTERM / window close.
void requestInterrupt();

/// Mark the process as shutting down. Sets the same flag interrupted() reads,
/// but never escalates to a hard exit the way a repeated requestInterrupt()
/// does, so it is safe to call unconditionally (e.g. after Ctrl+C already set
/// the flag). Engine teardown calls this before joining worker threads.
///
/// Why it matters: long-running model inference is one synchronous native call
/// on its thread, so the QuickJS interrupt can't break it — the only way out
/// is the op's cooperative-cancel hook, and every such hook polls
/// interrupted(). Without this, teardown's join blocks until the op finishes;
/// the window sits unresponsive and a user force-close then kills threads
/// mid-CUDA-dispatch, which races the display driver's per-process cleanup
/// and has bugchecked the machine (0x139 CORRUPT_LIST_ENTRY in nvlddmkm).
void beginShutdown();

/// Install the platform signal handler (SIGINT on Unix,
/// SetConsoleCtrlHandler on Windows). Idempotent.
void installSignalHandler();

/// Wire a QuickJS interrupt handler on this runtime that aborts JS execution
/// when the global flag is set. Safe to call from any thread on its own
/// runtime (main thread, Worker thread, etc.). Idempotent per-runtime.
void installJsInterruptHandler(JSRuntime* rt);

} // namespace bro::util
