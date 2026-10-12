// native_vision_decl.h — the C entry points native_vision_register.cpp registers, one
// prototype per native. Hand-maintained: a native is added or changed here, in the
// register file, in its JS caller (js/vision_wildlife.js) and in its body
// (native_vision.cpp) together.
//
// How each type crosses (bronze/src/abi/bronze_native_type.h):
//   int32_t                      i32
//   (const T*, uint32_t)         a typed array: elements and count, valid for the call
//   bronze_native_buffer* out    a typed-array RESULT: the function returns void and
//                                fills *out; release set ([transfer]) and the array is
//                                a view over data, with release(ctx) run when it is
//                                collected
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "abi/bronze_native_type.h"  // bronze_native_buffer, owned by bronze

#ifdef __cplusplus
extern "C" {
#endif

// bro.vision.wildlife's similarities: every query row against every row of
// `rows` (both row-major, `dim` floats a row). Result [q * nRows + r], each
// the dot product summed in float64 from i = 0 up, one product at a time,
// and stored as a float: the same number as the plain JavaScript loop
// `let d = 0; for (i) d += q[i] * row[i]` written into a Float32Array.
// Empty for dim < 1 or a length not a multiple of dim. [transfer]
//   registered at __bro_native.vision.dotRows
void bro_vision_dotRows(const float* queries, uint32_t queries_len,
                        const float* rows, uint32_t rows_len,
                        int32_t dim, bronze_native_buffer* out);

#ifdef __cplusplus
}  // extern "C"
#endif
