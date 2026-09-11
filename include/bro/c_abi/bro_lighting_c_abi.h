// =============================================================================
// bro_lighting_c_abi.h — Pure C-ABI declarations for bro.lighting
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_LIGHTING_C_ABI_H
#define BRO_LIGHTING_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.lighting.LightNode ---
void* bro_LightNode_create(void);
void  bro_LightNode_destroy(void* self);
void* bro_LightNode_get_color(void* self);
void bro_LightNode_set_color(void* self, void* val);
double bro_LightNode_get_intensity(void* self);
void bro_LightNode_set_intensity(void* self, double val);
double bro_LightNode_get_range(void* self);
void bro_LightNode_set_range(void* self, double val);
double bro_LightNode_get_innerCone(void* self);
void bro_LightNode_set_innerCone(void* self, double val);
double bro_LightNode_get_outerCone(void* self);
void bro_LightNode_set_outerCone(void* self, double val);
bool bro_LightNode_get_castShadow(void* self);
void bro_LightNode_set_castShadow(void* self, bool val);

// --- Interface bro.lighting.ShapeNode ---
void* bro_ShapeNode_create(void);
void  bro_ShapeNode_destroy(void* self);
const char* bro_ShapeNode_get_shapeType(void* self);
void bro_ShapeNode_set_shapeType(void* self, const char* val);
void* bro_ShapeNode_get_color(void* self);
void bro_ShapeNode_set_color(void* self, void* val);
void* bro_ShapeNode_get_size(void* self);
void bro_ShapeNode_set_size(void* self, void* val);

// --- Interface bro.lighting.SpriteNode ---
void* bro_SpriteNode_create(void);
void  bro_SpriteNode_destroy(void* self);
const char* bro_SpriteNode_get_texture(void* self);
void bro_SpriteNode_set_texture(void* self, const char* val);
void* bro_SpriteNode_get_size(void* self);
void bro_SpriteNode_set_size(void* self, void* val);
void bro_SpriteNode_addAnimation(void* self, const char* name, void* animSpec);
void bro_SpriteNode_play(void* self, const char* name);
void bro_SpriteNode_stop(void* self);

// --- Interface bro.lighting.HtmlNode ---
void* bro_HtmlNode_create(void);
void  bro_HtmlNode_destroy(void* self);
void bro_HtmlNode_setHtml(void* self, const char* html);
void bro_HtmlNode_markHtmlDirty(void* self);

// --- Interface bro.lighting.ParticleNode ---
void* bro_ParticleNode_create(void);
void  bro_ParticleNode_destroy(void* self);
void bro_ParticleNode_burst(void* self, int32_t count);
void bro_ParticleNode_clear(void* self);

// --- Interface bro.lighting.Particles3DNode ---
void* bro_Particles3DNode_create(void);
void  bro_Particles3DNode_destroy(void* self);
void bro_Particles3DNode_burst(void* self, int32_t count);
void bro_Particles3DNode_clear(void* self);

// --- Interface bro.lighting.GaussianSplatNode ---
void* bro_GaussianSplatNode_create(void);
void  bro_GaussianSplatNode_destroy(void* self);
void bro_GaussianSplatNode_savePly(void* self, const char* path);

// --- Interface bro.lighting.DecalNode ---
void* bro_DecalNode_create(void);
void  bro_DecalNode_destroy(void* self);
const char* bro_DecalNode_get_texture(void* self);
void bro_DecalNode_set_texture(void* self, const char* val);
void* bro_DecalNode_get_size(void* self);
void bro_DecalNode_set_size(void* self, void* val);

// --- Interface bro.lighting.ReflectionProbeNode ---
void* bro_ReflectionProbeNode_create(void);
void  bro_ReflectionProbeNode_destroy(void* self);
void bro_ReflectionProbeNode_probeCapture(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_LIGHTING_C_ABI_H
