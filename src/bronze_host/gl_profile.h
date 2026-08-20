#pragma once

// Env-gated wall-clock profiler for the host native call surface — the GL
// entry points first (that is what BRO_GL_PROFILE names), but by construction
// every function a bronze-compiled program reaches through ObjectBuilder::def
// or ::accessor, DOM included, because they all funnel through one place.
//
// MEASUREMENT, NOT POLICY. When BRO_GL_PROFILE is unset, hostProfileWrap()
// hands the callable straight back and the installed function is bit-for-bit
// the one the binding wrote: no branch, no counter, no indirection in the hot
// path. The cost of the seam existing is one bool test per *installed*
// function, at install time.
//
// When it is set, each distinct entry-point name gets one Slot, resolved once
// at install (so the 200-odd per-element functions a DOM-heavy app installs
// share slots rather than multiplying them) and captured by pointer, so a call
// costs two QueryPerformanceCounter reads and three adds — no hashing.
//
// Self vs inclusive: a host call can re-enter JS (event dispatch, a callback),
// which would double-count the callee's time into the caller. A thread-local
// child accumulator subtracts it, so `self` is the time actually spent inside
// the binding and the GL driver, and `incl` is the wall time the program saw.
// For GL, which never re-enters, the two are equal.

#include "embed/embed.h"

namespace bro::bronze_host {

// True when BRO_GL_PROFILE=1 was in the environment at first use. Read it
// through hostProfileEnabled(); the flag is initialised lazily on first wrap.
bool hostProfileEnabled();

// Identity when disabled. Otherwise returns a wrapper that times `fn` against
// the slot named by `name`. `name` must have static lifetime (every caller
// passes a string literal); the slot table copies it anyway, but the wrapper
// keeps no string.
bronze::embed::NativeFn hostProfileWrap(const char* name, bronze::embed::NativeFn fn);

// Sorted table on stderr. Registered with std::atexit on first wrap, so it
// fires on the normal teardown path bro-headless takes; calling it directly is
// harmless (it prints once).
void hostProfileDump();

}  // namespace bro::bronze_host
