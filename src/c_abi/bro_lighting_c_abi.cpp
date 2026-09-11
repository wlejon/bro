// =============================================================================
// bro_lighting_c_abi.cpp — C++ forwarding implementations for bro.lighting
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_lighting_c_abi.h"
#include <cstdint>
#include <string>

struct BroLightNodeImpl {
    double intensity = 1.0;
    double range = 10.0;
    double innerCone = 0.0;
    double outerCone = 45.0;
    bool castShadow = false;
};

struct BroShapeNodeImpl {
    std::string shapeType = "box";
};

struct BroSpriteNodeImpl {
    std::string texture;
    bool isPlaying = false;
    std::string currentAnim;
};

struct BroHtmlNodeImpl {
    std::string html;
    bool dirty = false;
};

struct BroParticleNodeImpl {
    int32_t burstCount = 0;
};

struct BroParticles3DNodeImpl {
    int32_t burstCount = 0;
};

struct BroGaussianSplatNodeImpl {
    std::string plyPath;
};

struct BroDecalNodeImpl {
    std::string texture;
};

struct BroReflectionProbeNodeImpl {
    bool captured = false;
};

extern "C" {

// --- LightNode ---
void* bro_LightNode_create(void) {
    return new BroLightNodeImpl();
}

void bro_LightNode_destroy(void* self) {
    delete static_cast<BroLightNodeImpl*>(self);
}

void* bro_LightNode_get_color(void* /*self*/) {
    return nullptr;
}

void bro_LightNode_set_color(void* /*self*/, void* /*val*/) {
}

double bro_LightNode_get_intensity(void* self) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    return l ? l->intensity : 1.0;
}

void bro_LightNode_set_intensity(void* self, double val) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    if (l) l->intensity = val;
}

double bro_LightNode_get_range(void* self) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    return l ? l->range : 10.0;
}

void bro_LightNode_set_range(void* self, double val) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    if (l) l->range = val;
}

double bro_LightNode_get_innerCone(void* self) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    return l ? l->innerCone : 0.0;
}

void bro_LightNode_set_innerCone(void* self, double val) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    if (l) l->innerCone = val;
}

double bro_LightNode_get_outerCone(void* self) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    return l ? l->outerCone : 45.0;
}

void bro_LightNode_set_outerCone(void* self, double val) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    if (l) l->outerCone = val;
}

bool bro_LightNode_get_castShadow(void* self) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    return l ? l->castShadow : false;
}

void bro_LightNode_set_castShadow(void* self, bool val) {
    auto* l = static_cast<BroLightNodeImpl*>(self);
    if (l) l->castShadow = val;
}

// --- ShapeNode ---
void* bro_ShapeNode_create(void) {
    return new BroShapeNodeImpl();
}

void bro_ShapeNode_destroy(void* self) {
    delete static_cast<BroShapeNodeImpl*>(self);
}

const char* bro_ShapeNode_get_shapeType(void* self) {
    auto* s = static_cast<BroShapeNodeImpl*>(self);
    return s ? s->shapeType.c_str() : "box";
}

void bro_ShapeNode_set_shapeType(void* self, const char* val) {
    auto* s = static_cast<BroShapeNodeImpl*>(self);
    if (s && val) s->shapeType = val;
}

void* bro_ShapeNode_get_color(void* /*self*/) {
    return nullptr;
}

void bro_ShapeNode_set_color(void* /*self*/, void* /*val*/) {
}

void* bro_ShapeNode_get_size(void* /*self*/) {
    return nullptr;
}

void bro_ShapeNode_set_size(void* /*self*/, void* /*val*/) {
}

// --- SpriteNode ---
void* bro_SpriteNode_create(void) {
    return new BroSpriteNodeImpl();
}

void bro_SpriteNode_destroy(void* self) {
    delete static_cast<BroSpriteNodeImpl*>(self);
}

const char* bro_SpriteNode_get_texture(void* self) {
    auto* s = static_cast<BroSpriteNodeImpl*>(self);
    return s ? s->texture.c_str() : "";
}

void bro_SpriteNode_set_texture(void* self, const char* val) {
    auto* s = static_cast<BroSpriteNodeImpl*>(self);
    if (s && val) s->texture = val;
}

void* bro_SpriteNode_get_size(void* /*self*/) {
    return nullptr;
}

void bro_SpriteNode_set_size(void* /*self*/, void* /*val*/) {
}

void bro_SpriteNode_addAnimation(void* /*self*/, const char* /*name*/, void* /*animSpec*/) {
}

void bro_SpriteNode_play(void* self, const char* name) {
    auto* s = static_cast<BroSpriteNodeImpl*>(self);
    if (s) {
        s->isPlaying = true;
        s->currentAnim = name ? name : "";
    }
}

void bro_SpriteNode_stop(void* self) {
    auto* s = static_cast<BroSpriteNodeImpl*>(self);
    if (s) s->isPlaying = false;
}

// --- HtmlNode ---
void* bro_HtmlNode_create(void) {
    return new BroHtmlNodeImpl();
}

void bro_HtmlNode_destroy(void* self) {
    delete static_cast<BroHtmlNodeImpl*>(self);
}

void bro_HtmlNode_setHtml(void* self, const char* html) {
    auto* h = static_cast<BroHtmlNodeImpl*>(self);
    if (h && html) h->html = html;
}

void bro_HtmlNode_markHtmlDirty(void* self) {
    auto* h = static_cast<BroHtmlNodeImpl*>(self);
    if (h) h->dirty = true;
}

// --- ParticleNode ---
void* bro_ParticleNode_create(void) {
    return new BroParticleNodeImpl();
}

void bro_ParticleNode_destroy(void* self) {
    delete static_cast<BroParticleNodeImpl*>(self);
}

void bro_ParticleNode_burst(void* self, int32_t count) {
    auto* p = static_cast<BroParticleNodeImpl*>(self);
    if (p) p->burstCount += count;
}

void bro_ParticleNode_clear(void* self) {
    auto* p = static_cast<BroParticleNodeImpl*>(self);
    if (p) p->burstCount = 0;
}

// --- Particles3DNode ---
void* bro_Particles3DNode_create(void) {
    return new BroParticles3DNodeImpl();
}

void bro_Particles3DNode_destroy(void* self) {
    delete static_cast<BroParticles3DNodeImpl*>(self);
}

void bro_Particles3DNode_burst(void* self, int32_t count) {
    auto* p = static_cast<BroParticles3DNodeImpl*>(self);
    if (p) p->burstCount += count;
}

void bro_Particles3DNode_clear(void* self) {
    auto* p = static_cast<BroParticles3DNodeImpl*>(self);
    if (p) p->burstCount = 0;
}

// --- GaussianSplatNode ---
void* bro_GaussianSplatNode_create(void) {
    return new BroGaussianSplatNodeImpl();
}

void bro_GaussianSplatNode_destroy(void* self) {
    delete static_cast<BroGaussianSplatNodeImpl*>(self);
}

void bro_GaussianSplatNode_savePly(void* self, const char* path) {
    auto* g = static_cast<BroGaussianSplatNodeImpl*>(self);
    if (g && path) g->plyPath = path;
}

// --- DecalNode ---
void* bro_DecalNode_create(void) {
    return new BroDecalNodeImpl();
}

void bro_DecalNode_destroy(void* self) {
    delete static_cast<BroDecalNodeImpl*>(self);
}

const char* bro_DecalNode_get_texture(void* self) {
    auto* d = static_cast<BroDecalNodeImpl*>(self);
    return d ? d->texture.c_str() : "";
}

void bro_DecalNode_set_texture(void* self, const char* val) {
    auto* d = static_cast<BroDecalNodeImpl*>(self);
    if (d && val) d->texture = val;
}

void* bro_DecalNode_get_size(void* /*self*/) {
    return nullptr;
}

void bro_DecalNode_set_size(void* /*self*/, void* /*val*/) {
}

// --- ReflectionProbeNode ---
void* bro_ReflectionProbeNode_create(void) {
    return new BroReflectionProbeNodeImpl();
}

void bro_ReflectionProbeNode_destroy(void* self) {
    delete static_cast<BroReflectionProbeNodeImpl*>(self);
}

void bro_ReflectionProbeNode_probeCapture(void* self) {
    auto* r = static_cast<BroReflectionProbeNodeImpl*>(self);
    if (r) r->captured = true;
}

} // extern "C"