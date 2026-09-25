#pragma once

// Crash diagnostics for a long headless run.
//
// The failure this exists for: a research script rendering for an hour exits
// 139 with NO output at all. Every diagnostic the run produced — the progress
// log, the last render's numbers, the line it was on — is sitting in a stdio
// buffer that a SIGSEGV never gets to flush, so the one run in four that
// crashes tells you nothing about why, and the only way to learn anything is
// to run it again and hope.
//
// install() fixes both halves of that:
//
//   * stdout and stderr go UNBUFFERED, so a line that was printed has been
//     written by the time the next statement runs — including the C-library
//     printf/fprintf a native module does, which is the output that was being
//     lost. Redirecting to a file is exactly the case where the CRT switches
//     to full buffering, and it is also exactly the case a long run uses.
//   * a crash prints the faulting address, the exception code, the script the
//     host last said it was running, and a symbolised stack before the process
//     goes, through three routes so that none of them has to be the one that
//     works: an unhandled-exception filter, a vectored handler (which runs
//     first-chance, so it reports even a fault some __except swallows), and
//     signal handlers for the abort()/SIGSEGV paths the CRT owns.
//
// The trace is symbolised through DbgHelp against the PDBs next to the
// executable, which is why the Release configuration ships them. Elsewhere
// (macOS, Linux) it is sigaction handlers for SIGSEGV/SIGBUS/SIGILL/SIGFPE/
// SIGABRT on an alternate stack (so a stack overflow still reports), the
// interrupted pc/lr/fault address from the ucontext, and execinfo's
// backtrace() named through the dynamic symbol table.

#include <string>

namespace bro::util {

// Unbuffer the standard streams and install the crash handlers. Idempotent;
// call it as the first thing in main().
void installCrashHandler();

// Breadcrumb for the crash report: what the host is running right now. Kept in
// a fixed buffer written with no allocation, so the handler can read it from a
// faulted thread. Empty clears it.
void setCrashContext(const std::string& what);

// Dump a symbolised backtrace of the CALLING thread to stderr, prefixed with
// `reason`. The crash handlers use it; it is exported because a hang or a
// "this should be impossible" branch wants the same output.
void dumpBacktrace(const char* reason);

} // namespace bro::util
