// native_lighting_decl.h — the C entry points native_lighting_register.cpp registers, one
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

// destructor of __bro_native.lighting.LightNode: frees what the constructor (or a native returning a LightNode) allocated
void bro_lighting_LightNode_dtor(void* self);

// constructor of __bro_native.lighting.LightNode: registered so the class exists (instances come from natives returning a LightNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.LightNode
void* bro_lighting_LightNode_ctor(void);

// destructor of __bro_native.lighting.ShapeNode: frees what the constructor (or a native returning a ShapeNode) allocated
void bro_lighting_ShapeNode_dtor(void* self);

// constructor of __bro_native.lighting.ShapeNode: registered so the class exists (instances come from natives returning a ShapeNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.ShapeNode
void* bro_lighting_ShapeNode_ctor(void);

// destructor of __bro_native.lighting.SpriteNode: frees what the constructor (or a native returning a SpriteNode) allocated
void bro_lighting_SpriteNode_dtor(void* self);

// constructor of __bro_native.lighting.SpriteNode: registered so the class exists (instances come from natives returning a SpriteNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.SpriteNode
void* bro_lighting_SpriteNode_ctor(void);

// destructor of __bro_native.lighting.HtmlNode: frees what the constructor (or a native returning a HtmlNode) allocated
void bro_lighting_HtmlNode_dtor(void* self);

// constructor of __bro_native.lighting.HtmlNode: registered so the class exists (instances come from natives returning a HtmlNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.HtmlNode
void* bro_lighting_HtmlNode_ctor(void);

// destructor of __bro_native.lighting.ParticleNode: frees what the constructor (or a native returning a ParticleNode) allocated
void bro_lighting_ParticleNode_dtor(void* self);

// constructor of __bro_native.lighting.ParticleNode: registered so the class exists (instances come from natives returning a ParticleNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.ParticleNode
void* bro_lighting_ParticleNode_ctor(void);

// destructor of __bro_native.lighting.Particles3DNode: frees what the constructor (or a native returning a Particles3DNode) allocated
void bro_lighting_Particles3DNode_dtor(void* self);

// constructor of __bro_native.lighting.Particles3DNode: registered so the class exists (instances come from natives returning a Particles3DNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.Particles3DNode
void* bro_lighting_Particles3DNode_ctor(void);

// destructor of __bro_native.lighting.GaussianSplatNode: frees what the constructor (or a native returning a GaussianSplatNode) allocated
void bro_lighting_GaussianSplatNode_dtor(void* self);

// constructor of __bro_native.lighting.GaussianSplatNode: registered so the class exists (instances come from natives returning a GaussianSplatNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.GaussianSplatNode
void* bro_lighting_GaussianSplatNode_ctor(void);

// destructor of __bro_native.lighting.DecalNode: frees what the constructor (or a native returning a DecalNode) allocated
void bro_lighting_DecalNode_dtor(void* self);

// constructor of __bro_native.lighting.DecalNode: registered so the class exists (instances come from natives returning a DecalNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.DecalNode
void* bro_lighting_DecalNode_ctor(void);

// destructor of __bro_native.lighting.ReflectionProbeNode: frees what the constructor (or a native returning a ReflectionProbeNode) allocated
void bro_lighting_ReflectionProbeNode_dtor(void* self);

// constructor of __bro_native.lighting.ReflectionProbeNode: registered so the class exists (instances come from natives returning a ReflectionProbeNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.lighting.ReflectionProbeNode
void* bro_lighting_ReflectionProbeNode_ctor(void);

// bro.lighting.LightNode.prototype.color read
//   registered at __bro_native.lighting.LightNode_color_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_lighting_LightNode_color_get(void* self, bronze_native_buffer* out);

// bro.lighting.LightNode.prototype.color write
//   registered at __bro_native.lighting.LightNode_color_set
void bro_lighting_LightNode_color_set(void* self, const double* v, uint32_t v_len);

// bro.lighting.LightNode.prototype.intensity read
//   registered at __bro_native.lighting.LightNode_intensity_get
double bro_lighting_LightNode_intensity_get(void* self);

// bro.lighting.LightNode.prototype.intensity write
//   registered at __bro_native.lighting.LightNode_intensity_set
void bro_lighting_LightNode_intensity_set(void* self, double v);

// bro.lighting.LightNode.prototype.range read
//   registered at __bro_native.lighting.LightNode_range_get
double bro_lighting_LightNode_range_get(void* self);

// bro.lighting.LightNode.prototype.range write
//   registered at __bro_native.lighting.LightNode_range_set
void bro_lighting_LightNode_range_set(void* self, double v);

// bro.lighting.LightNode.prototype.innerCone read
//   registered at __bro_native.lighting.LightNode_innerCone_get
double bro_lighting_LightNode_innerCone_get(void* self);

// bro.lighting.LightNode.prototype.innerCone write
//   registered at __bro_native.lighting.LightNode_innerCone_set
void bro_lighting_LightNode_innerCone_set(void* self, double v);

// bro.lighting.LightNode.prototype.outerCone read
//   registered at __bro_native.lighting.LightNode_outerCone_get
double bro_lighting_LightNode_outerCone_get(void* self);

// bro.lighting.LightNode.prototype.outerCone write
//   registered at __bro_native.lighting.LightNode_outerCone_set
void bro_lighting_LightNode_outerCone_set(void* self, double v);

// bro.lighting.LightNode.prototype.castShadow read
//   registered at __bro_native.lighting.LightNode_castShadow_get
bool bro_lighting_LightNode_castShadow_get(void* self);

// bro.lighting.LightNode.prototype.castShadow write
//   registered at __bro_native.lighting.LightNode_castShadow_set
void bro_lighting_LightNode_castShadow_set(void* self, bool v);

// bro.lighting.ShapeNode.prototype.shapeType read
//   registered at __bro_native.lighting.ShapeNode_shapeType_get
const char* bro_lighting_ShapeNode_shapeType_get(void* self);

// bro.lighting.ShapeNode.prototype.shapeType write
//   registered at __bro_native.lighting.ShapeNode_shapeType_set
void bro_lighting_ShapeNode_shapeType_set(void* self, const char* v);

// bro.lighting.ShapeNode.prototype.color read
//   registered at __bro_native.lighting.ShapeNode_color_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_lighting_ShapeNode_color_get(void* self, bronze_native_buffer* out);

// bro.lighting.ShapeNode.prototype.color write
//   registered at __bro_native.lighting.ShapeNode_color_set
void bro_lighting_ShapeNode_color_set(void* self, const double* v, uint32_t v_len);

// bro.lighting.ShapeNode.prototype.size read
//   registered at __bro_native.lighting.ShapeNode_size_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_lighting_ShapeNode_size_get(void* self, bronze_native_buffer* out);

// bro.lighting.ShapeNode.prototype.size write
//   registered at __bro_native.lighting.ShapeNode_size_set
void bro_lighting_ShapeNode_size_set(void* self, const double* v, uint32_t v_len);

// bro.lighting.SpriteNode.prototype.texture read
//   registered at __bro_native.lighting.SpriteNode_texture_get
const char* bro_lighting_SpriteNode_texture_get(void* self);

// bro.lighting.SpriteNode.prototype.texture write
//   registered at __bro_native.lighting.SpriteNode_texture_set
void bro_lighting_SpriteNode_texture_set(void* self, const char* v);

// bro.lighting.SpriteNode.prototype.size read
//   registered at __bro_native.lighting.SpriteNode_size_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_lighting_SpriteNode_size_get(void* self, bronze_native_buffer* out);

// bro.lighting.SpriteNode.prototype.size write
//   registered at __bro_native.lighting.SpriteNode_size_set
void bro_lighting_SpriteNode_size_set(void* self, const double* v, uint32_t v_len);

// bro.lighting.SpriteNode.prototype.play
//   registered at __bro_native.lighting.SpriteNode_play
void bro_lighting_SpriteNode_play(void* self, const char* name);

// bro.lighting.SpriteNode.prototype.stop
//   registered at __bro_native.lighting.SpriteNode_stop
void bro_lighting_SpriteNode_stop(void* self);

// bro.lighting.HtmlNode.prototype.setHtml
//   registered at __bro_native.lighting.HtmlNode_setHtml
void bro_lighting_HtmlNode_setHtml(void* self, const char* html);

// bro.lighting.HtmlNode.prototype.markHtmlDirty
//   registered at __bro_native.lighting.HtmlNode_markHtmlDirty
void bro_lighting_HtmlNode_markHtmlDirty(void* self);

// bro.lighting.ParticleNode.prototype.burst
//   registered at __bro_native.lighting.ParticleNode_burst
void bro_lighting_ParticleNode_burst(void* self, int32_t count);

// bro.lighting.ParticleNode.prototype.clear
//   registered at __bro_native.lighting.ParticleNode_clear
void bro_lighting_ParticleNode_clear(void* self);

// bro.lighting.Particles3DNode.prototype.burst
//   registered at __bro_native.lighting.Particles3DNode_burst
void bro_lighting_Particles3DNode_burst(void* self, int32_t count);

// bro.lighting.Particles3DNode.prototype.clear
//   registered at __bro_native.lighting.Particles3DNode_clear
void bro_lighting_Particles3DNode_clear(void* self);

// bro.lighting.GaussianSplatNode.prototype.savePly
//   registered at __bro_native.lighting.GaussianSplatNode_savePly
void bro_lighting_GaussianSplatNode_savePly(void* self, const char* path);

// bro.lighting.DecalNode.prototype.texture read
//   registered at __bro_native.lighting.DecalNode_texture_get
const char* bro_lighting_DecalNode_texture_get(void* self);

// bro.lighting.DecalNode.prototype.texture write
//   registered at __bro_native.lighting.DecalNode_texture_set
void bro_lighting_DecalNode_texture_set(void* self, const char* v);

// bro.lighting.DecalNode.prototype.size read
//   registered at __bro_native.lighting.DecalNode_size_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_lighting_DecalNode_size_get(void* self, bronze_native_buffer* out);

// bro.lighting.DecalNode.prototype.size write
//   registered at __bro_native.lighting.DecalNode_size_set
void bro_lighting_DecalNode_size_set(void* self, const double* v, uint32_t v_len);

// bro.lighting.ReflectionProbeNode.prototype.probeCapture
//   registered at __bro_native.lighting.ReflectionProbeNode_probeCapture
void bro_lighting_ReflectionProbeNode_probeCapture(void* self);

#ifdef __cplusplus
}  // extern "C"
#endif
