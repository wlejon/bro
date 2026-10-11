// native_models_decl.h — the C entry points native_models_register.cpp registers, one
// prototype per native. Hand-maintained: a native is added or changed here, in the
// register file, in its JS wrapper (js/models.js) and in its body (native_models.cpp)
// together. The type conventions are native_paths_decl.h's.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// bro.models.dirs
//   registered at __bro_native.models.dirs
//   the search list, one absolute directory per line (util/model_dirs.h)
const char* bro_models_dirs(void);

// bro.models.find
//   registered at __bro_native.models.find
//   the first <dir>/<name> that is a directory, absolute; "" for none
const char* bro_models_find(const char* name);

#ifdef __cplusplus
}  // extern "C"
#endif
