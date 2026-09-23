// native_time_decl.h — the C entry points native_time_register.cpp registers, one
// prototype per native. Hand-maintained: a native is added or changed here, in the
// register file, in its JS wrapper and in its body together. The bodies are
// hand-written in bro against the engine, and the compiler checks them here.
//
// How each type crosses (bronze/src/abi/bronze_native_type.h):
//   double / int32_t / bool      f64 / i32 / bool
//   const char*                  str — UTF-8, valid for the call only; a RESULT must
//                                outlive the return (a per-thread scratch), and one
//                                marked JSON carries JSON of the type named in the comment
//   uint64_t                     dynamic — a callback's raw Value bits; keep it in a
//                                Persistent, never across an allocating call raw
//   (const T*, uint32_t)         a typed array: elements and count, valid for the call
//   void*                        a class handle: what the class constructor returned
//   bronze_native_buffer* out    a typed-array RESULT: the function returns void and
//                                fills *out (data, length in elements); release == NULL
//                                and the runtime copies during the call; release set
//                                ([transfer]) and the array is a view over data, with
//                                release(ctx) run when it is collected
//   bool <x>_given, T <x>        an optional value with no declared default, and
//                                whether it was passed
//
// A dictionary or list RESULT crosses as reads: the operation runs and keeps its
// result in a per-thread slot, answering nothing (or the list's length), and the
// <op>_<member> / <op>_at natives read from that slot until the next call.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "abi/bronze_native_type.h"  // bronze_native_buffer, owned by bronze

#ifdef __cplusplus
extern "C" {
#endif

// bro.time.scale read
//   registered as a namespace property (getter) at __bro_native.time.scale
double bro_time_scale_get(void);

// bro.time.scale write
//   registered as a namespace property (setter) at __bro_native.time.scale
void bro_time_scale_set(double v);

// bro.time.paused read
//   registered as a namespace property (getter) at __bro_native.time.paused
bool bro_time_paused_get(void);

// bro.time.paused write
//   registered as a namespace property (setter) at __bro_native.time.paused
void bro_time_paused_set(bool v);

// bro.time.now read
//   registered as a namespace property (getter) at __bro_native.time.now
double bro_time_now_get(void);

#ifdef __cplusplus
}  // extern "C"
#endif
