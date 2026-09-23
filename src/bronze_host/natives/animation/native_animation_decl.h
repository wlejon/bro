// native_animation_decl.h — the C entry points native_animation_register.cpp registers, one
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

// destructor of __bro_native.animation.Tween: frees what the constructor (or a native returning a Tween) allocated
void bro_animation_Tween_dtor(void* self);

// constructor of __bro_native.animation.Tween: registered so the class exists (instances come from natives returning a Tween); never called by the wrapper, the body may return NULL
//   registered at __bro_native.animation.Tween
void* bro_animation_Tween_ctor(void);

// destructor of __bro_native.animation.AnimationPlayer: frees what the constructor (or a native returning a AnimationPlayer) allocated
void bro_animation_AnimationPlayer_dtor(void* self);

// constructor of __bro_native.animation.AnimationPlayer: registered so the class exists (instances come from natives returning a AnimationPlayer); never called by the wrapper, the body may return NULL
//   registered at __bro_native.animation.AnimationPlayer
void* bro_animation_AnimationPlayer_ctor(void);

// bro.animation.Tween.prototype.call
//   registered at __bro_native.animation.Tween_call
void* bro_animation_Tween_call(void* self, uint64_t callback);

// bro.animation.Tween.prototype.loop
//   registered at __bro_native.animation.Tween_loop
//   count_given: false when the value was not passed (no declared default)
void* bro_animation_Tween_loop(void* self, bool count_given, int32_t count);

// bro.animation.Tween.prototype.start
//   registered at __bro_native.animation.Tween_start
void* bro_animation_Tween_start(void* self);

// bro.animation.Tween.prototype.stop
//   registered at __bro_native.animation.Tween_stop
void* bro_animation_Tween_stop(void* self);

// bro.animation.Tween.prototype.pause
//   registered at __bro_native.animation.Tween_pause
void* bro_animation_Tween_pause(void* self);

// bro.animation.Tween.prototype.resume
//   registered at __bro_native.animation.Tween_resume
void* bro_animation_Tween_resume(void* self);

// bro.animation.Tween.prototype.destroy
//   registered at __bro_native.animation.Tween_destroy
void bro_animation_Tween_destroy(void* self);

// bro.animation.AnimationPlayer.prototype.pause
//   registered at __bro_native.animation.AnimationPlayer_pause
void bro_animation_AnimationPlayer_pause(void* self);

// bro.animation.AnimationPlayer.prototype.resume
//   registered at __bro_native.animation.AnimationPlayer_resume
void bro_animation_AnimationPlayer_resume(void* self);

// bro.animation.AnimationPlayer.prototype.stop
//   registered at __bro_native.animation.AnimationPlayer_stop
void bro_animation_AnimationPlayer_stop(void* self);

// bro.animation.AnimationPlayer.prototype.seek
//   registered at __bro_native.animation.AnimationPlayer_seek
void bro_animation_AnimationPlayer_seek(void* self, double time);

// bro.animation.AnimationPlayer.prototype.destroy
//   registered at __bro_native.animation.AnimationPlayer_destroy
void bro_animation_AnimationPlayer_destroy(void* self);

#ifdef __cplusplus
}  // extern "C"
#endif
