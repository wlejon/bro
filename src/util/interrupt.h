#pragma once

#include <atomic>

/// Process-wide interrupt flag + Ctrl+C wiring.
///
/// When the user hits Ctrl+C, a signal handler sets a global atomic flag.
/// Event loops poll the flag so tight C++ sleep/wait loops exit.
///
/// A second Ctrl+C hard-exits the process.
namespace bro::util {

/// Returns true once the user has requested interruption.
bool interrupted();

/// Request interruption. First call sets the flag (interrupt poll, event loops
/// drop out); a second call hard-exits the process. Same path as Ctrl+C / SIGTERM / window close.
void requestInterrupt();

/// Mark the process as shutting down. Sets the same flag interrupted() reads,
/// but never escalates to a hard exit the way a repeated requestInterrupt()
/// does, so it is safe to call unconditionally (e.g. after Ctrl+C already set
/// the flag). Engine teardown calls this before joining worker threads.
void beginShutdown();

/// Install the platform signal handler (SIGINT on Unix,
/// SetConsoleCtrlHandler on Windows). Idempotent.
void installSignalHandler();

} // namespace bro::util
