// native_scene_lighting.cpp — LightNode, ShapeNode, SpriteNode, etc. native implementations.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/lighting/native_lighting_decl.h"
#include "scene/light_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/html_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/gaussian_splat_node.h"
#include "scene/decal_node.h"
#include "scene/reflection_probe_node.h"
#include "util/asset_path.h"
#include <broimage/decode.h>
#include <bromesh/io/splat_ply.h>
#include <unordered_map>

namespace bro::bronze_host {

bool registerNatives_lighting(std::string* error);

namespace {

static thread_local double tl_buf4[4];
static thread_local double tl_buf3[3];
static thread_local double tl_buf2[2];
static thread_local std::string tl_str;
static std::unordered_map<scene::DecalNode*, std::string> s_decalTextures;

}  // namespace

bool registerLightingNatives(std::string* error) {
    return registerNatives_lighting(error);
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

// =============================================================================
// LightNode
// =============================================================================

void bro_lighting_LightNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_LightNode_ctor(void) {
    return new HostSceneNodeCell();
}

void bro_lighting_LightNode_color_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        auto* l = static_cast<scene::LightNode*>(n);
        tl_buf3[0] = l->color().x;
        tl_buf3[1] = l->color().y;
        tl_buf3[2] = l->color().z;
    } else {
        tl_buf3[0] = 1; tl_buf3[1] = 1; tl_buf3[2] = 1;
    }
    copyBuffer(tl_buf3, 3, out);
}

void bro_lighting_LightNode_color_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light && v && v_len >= 3) {
        static_cast<scene::LightNode*>(n)->setColor(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
    }
}

double bro_lighting_LightNode_intensity_get(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Light) ? static_cast<double>(static_cast<scene::LightNode*>(n)->intensity()) : 1.0;
}

void bro_lighting_LightNode_intensity_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setIntensity(static_cast<float>(v));
    }
}

double bro_lighting_LightNode_range_get(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Light) ? static_cast<double>(static_cast<scene::LightNode*>(n)->range()) : 10.0;
}

void bro_lighting_LightNode_range_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setRange(static_cast<float>(v));
    }
}

double bro_lighting_LightNode_innerCone_get(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Light) ? static_cast<double>(static_cast<scene::LightNode*>(n)->innerAngle()) : 0.0;
}

void bro_lighting_LightNode_innerCone_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setInnerAngle(static_cast<float>(v));
    }
}

double bro_lighting_LightNode_outerCone_get(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Light) ? static_cast<double>(static_cast<scene::LightNode*>(n)->outerAngle()) : 0.0;
}

void bro_lighting_LightNode_outerCone_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setOuterAngle(static_cast<float>(v));
    }
}

bool bro_lighting_LightNode_castShadow_get(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Light) ? static_cast<scene::LightNode*>(n)->castsShadow() : false;
}

void bro_lighting_LightNode_castShadow_set(void* self, bool v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setCastsShadow(v);
    }
}

// =============================================================================
// ShapeNode
// =============================================================================

void bro_lighting_ShapeNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_ShapeNode_ctor(void) {
    return new HostSceneNodeCell();
}

const char* bro_lighting_ShapeNode_shapeType_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Shape) return "rect";
    switch (static_cast<scene::ShapeNode*>(n)->shape()) {
        case scene::ShapeNode::Shape::Rect: return "rect";
        case scene::ShapeNode::Shape::RoundRect: return "roundRect";
        case scene::ShapeNode::Shape::Circle: return "circle";
        case scene::ShapeNode::Shape::Ellipse: return "ellipse";
        case scene::ShapeNode::Shape::Polygon: return "polygon";
        case scene::ShapeNode::Shape::Line: return "line";
    }
    return "rect";
}

void bro_lighting_ShapeNode_shapeType_set(void* self, const char* v) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Shape || !v) return;
    std::string s = v;
    auto* sn = static_cast<scene::ShapeNode*>(n);
    if (s == "rect") sn->setShape(scene::ShapeNode::Shape::Rect);
    else if (s == "roundRect") sn->setShape(scene::ShapeNode::Shape::RoundRect);
    else if (s == "circle") sn->setShape(scene::ShapeNode::Shape::Circle);
    else if (s == "ellipse") sn->setShape(scene::ShapeNode::Shape::Ellipse);
    else if (s == "polygon") sn->setShape(scene::ShapeNode::Shape::Polygon);
    else if (s == "line") sn->setShape(scene::ShapeNode::Shape::Line);
}

void bro_lighting_ShapeNode_color_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Shape) {
        auto c = static_cast<scene::ShapeNode*>(n)->fillColor();
        tl_buf4[0] = c.r; tl_buf4[1] = c.g; tl_buf4[2] = c.b; tl_buf4[3] = c.a;
    } else {
        tl_buf4[0] = 1; tl_buf4[1] = 1; tl_buf4[2] = 1; tl_buf4[3] = 1;
    }
    copyBuffer(tl_buf4, 4, out);
}

void bro_lighting_ShapeNode_color_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Shape && v && v_len >= 3) {
        float a = (v_len >= 4) ? static_cast<float>(v[3]) : 1.0f;
        static_cast<scene::ShapeNode*>(n)->setFillColor(bromath::Color{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), a});
    }
}

void bro_lighting_ShapeNode_size_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Shape) {
        auto* sn = static_cast<scene::ShapeNode*>(n);
        tl_buf2[0] = sn->width();
        tl_buf2[1] = sn->height();
    } else {
        tl_buf2[0] = 0; tl_buf2[1] = 0;
    }
    copyBuffer(tl_buf2, 2, out);
}

void bro_lighting_ShapeNode_size_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Shape && v && v_len >= 2) {
        static_cast<scene::ShapeNode*>(n)->setSize(static_cast<float>(v[0]), static_cast<float>(v[1]));
    }
}

// =============================================================================
// SpriteNode
// =============================================================================

void bro_lighting_SpriteNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_SpriteNode_ctor(void) {
    return new HostSceneNodeCell();
}

const char* bro_lighting_SpriteNode_texture_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Sprite) return "";
    tl_str = static_cast<scene::SpriteNode*>(n)->imagePath();
    return tl_str.c_str();
}

void bro_lighting_SpriteNode_texture_set(void* self, const char* v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Sprite && v) {
        static_cast<scene::SpriteNode*>(n)->setImagePath(bro::util::resolveAssetPath(v));
    }
}

void bro_lighting_SpriteNode_size_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Sprite) {
        auto* sn = static_cast<scene::SpriteNode*>(n);
        tl_buf2[0] = sn->width();
        tl_buf2[1] = sn->height();
    } else {
        tl_buf2[0] = 0; tl_buf2[1] = 0;
    }
    copyBuffer(tl_buf2, 2, out);
}

void bro_lighting_SpriteNode_size_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Sprite && v && v_len >= 2) {
        static_cast<scene::SpriteNode*>(n)->setSize(static_cast<float>(v[0]), static_cast<float>(v[1]));
    }
}

void bro_lighting_SpriteNode_play(void* self, const char* name) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Sprite) {
        if (name && name[0] != '\0') static_cast<scene::SpriteNode*>(n)->play(name);
        else static_cast<scene::SpriteNode*>(n)->resume();
    }
}

void bro_lighting_SpriteNode_stop(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Sprite) {
        static_cast<scene::SpriteNode*>(n)->stop();
    }
}

// =============================================================================
// HtmlNode
// =============================================================================

void bro_lighting_HtmlNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_HtmlNode_ctor(void) {
    return new HostSceneNodeCell();
}

void bro_lighting_HtmlNode_setHtml(void* self, const char* html) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Html && html) {
        static_cast<scene::HtmlNode*>(n)->setHtml(html);
    }
}

void bro_lighting_HtmlNode_markHtmlDirty(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Html) {
        static_cast<scene::HtmlNode*>(n)->markDirty();
    }
}

// =============================================================================
// ParticleNode & Particles3DNode
// =============================================================================

void bro_lighting_ParticleNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_ParticleNode_ctor(void) {
    return new HostSceneNodeCell();
}

void bro_lighting_ParticleNode_burst(void* self, int32_t count) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->burst(count);
    }
}

void bro_lighting_ParticleNode_clear(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->clear();
    }
}

void bro_lighting_Particles3DNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_Particles3DNode_ctor(void) {
    return new HostSceneNodeCell();
}

void bro_lighting_Particles3DNode_burst(void* self, int32_t count) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->burst(count);
    }
}

void bro_lighting_Particles3DNode_clear(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->clear();
    }
}

// =============================================================================
// GaussianSplatNode
// =============================================================================

void bro_lighting_GaussianSplatNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_GaussianSplatNode_ctor(void) {
    return new HostSceneNodeCell();
}

void bro_lighting_GaussianSplatNode_savePly(void* self, const char* path) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::GaussianSplat && path) {
        auto* sn = static_cast<scene::GaussianSplatNode*>(n);
        bromesh::saveSplatPLY(sn->cloud(), bro::util::resolveAssetPath(path));
    }
}

// =============================================================================
// DecalNode
// =============================================================================

void bro_lighting_DecalNode_dtor(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        s_decalTextures.erase(static_cast<scene::DecalNode*>(n));
    }
    delete nodeCellOf(self);
}

void* bro_lighting_DecalNode_ctor(void) {
    return new HostSceneNodeCell();
}

const char* bro_lighting_DecalNode_texture_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Decal) return "";
    auto it = s_decalTextures.find(static_cast<scene::DecalNode*>(n));
    return (it != s_decalTextures.end()) ? it->second.c_str() : "";
}

void bro_lighting_DecalNode_texture_set(void* self, const char* v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal && v) {
        auto* dn = static_cast<scene::DecalNode*>(n);
        s_decalTextures[dn] = v;
        broimage::Image img;
        if (broimage::decode_file(bro::util::resolveAssetPath(v), img) && img.width > 0 && img.height > 0) {
            dn->setAlbedoTexture(img.width, img.height, img.pixels.data());
        }
    }
}

void bro_lighting_DecalNode_size_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        auto* dn = static_cast<scene::DecalNode*>(n);
        tl_buf3[0] = dn->scale().x;
        tl_buf3[1] = dn->scale().y;
        tl_buf3[2] = dn->scale().z;
    } else {
        tl_buf3[0] = 1; tl_buf3[1] = 1; tl_buf3[2] = 1;
    }
    copyBuffer(tl_buf3, 3, out);
}

void bro_lighting_DecalNode_size_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal && v && v_len >= 3) {
        static_cast<scene::DecalNode*>(n)->setScale(static_cast<float>(v[0]),
                                                   static_cast<float>(v[1]),
                                                   static_cast<float>(v[2]));
    }
}

// =============================================================================
// ReflectionProbeNode
// =============================================================================

void bro_lighting_ReflectionProbeNode_dtor(void* self) {
    delete nodeCellOf(self);
}

void* bro_lighting_ReflectionProbeNode_ctor(void) {
    return new HostSceneNodeCell();
}

void bro_lighting_ReflectionProbeNode_probeCapture(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        static_cast<scene::ReflectionProbeNode*>(n)->requestCapture();
    }
}

}  // extern "C"
