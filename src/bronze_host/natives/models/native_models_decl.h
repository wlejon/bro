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

// bro.models.sources
//   registered at __bro_native.models.sources
//   the `models.sources` entries in order, one per line (util/model_fetch.h)
const char* bro_models_sources(void);

// bro.models.fetch / bro.models.available
//   registered at __bro_native.models.fetchStart
//   starts a job on its own thread (listOnly: available); its id, 0 for a bad name
int32_t bro_models_fetchStart(const char* name, bool listOnly);

//   registered at __bro_native.models.fetchPoll
//   JSON {state: "running"|"done"|"failed"|"cancelled", done, total, files, file,
//   source, dir, error, code}; a settled job is forgotten once polled. An unknown
//   id answers {state: "failed", code: "io"}
const char* bro_models_fetchPoll(int32_t id);

//   registered at __bro_native.models.fetchCancel
//   asks the job to stop; it settles as "cancelled" (its partial files kept)
void bro_models_fetchCancel(int32_t id);

#ifdef __cplusplus
}  // extern "C"
#endif
