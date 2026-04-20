#pragma once

#include <atomic>

extern "C" {
#include "quickjs.h"
}

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

/// Install the platform signal handler (SIGINT on Unix,
/// SetConsoleCtrlHandler on Windows). Idempotent.
void installSignalHandler();

/// Wire a QuickJS interrupt handler on this runtime that aborts JS execution
/// when the global flag is set. Safe to call from any thread on its own
/// runtime (main thread, Worker thread, etc.). Idempotent per-runtime.
void installJsInterruptHandler(JSRuntime* rt);

} // namespace bro::util
