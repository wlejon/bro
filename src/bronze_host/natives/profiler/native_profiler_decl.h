// native_profiler_decl.h — the C entry points native_profiler_register.cpp registers, one
// prototype per native. Hand-maintained: a native is added or changed here, in the
// register file, in its JS wrapper (js/profiler.js) and in its body
// (native_profiler.cpp) together.
//
// How each type crosses (bronze/src/abi/bronze_native_type.h):
//   double / int32_t / bool      f64 / i32 / bool
//   const char*                  str — UTF-8, valid for the call only; a RESULT must
//                                outlive the return (a per-thread scratch), and one
//                                marked JSON carries JSON of the type named in the comment
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// bro.profiler.start: `threads` is "main", "workers", "js" or "all". Answers "" when
// the profile started, else why it did not.
//   registered at __bro_native.profiler.start
const char* bro_profiler_start(double hz, const char* threads);

// bro.profiler.running
//   registered at __bro_native.profiler.running
bool bro_profiler_running(void);

// bro.profiler.stop: JSON of a ProfileResult (docs/profiler-api.js), or "" when no
// profile was running.
//   registered at __bro_native.profiler.stop
const char* bro_profiler_stop(bool callers, bool report, int32_t top);

#ifdef __cplusplus
}  // extern "C"
#endif
