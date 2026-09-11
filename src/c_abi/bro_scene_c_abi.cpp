// =============================================================================
// bro_scene_c_abi.cpp — C++ forwarding implementations for bro.scene
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_scene_c_abi.h"
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

struct BroSceneNodeImpl {
    static inline int32_t nextId = 1;
    int32_t id = nextId++;
    std::string name;
    bool visible = true;
    double posX = 0.0, posY = 0.0, posZ = 0.0;
    double rotX = 0.0, rotY = 0.0, rotZ = 0.0, rotW = 1.0;
    double scaleX = 1.0, scaleY = 1.0, scaleZ = 1.0;
    void* parent = nullptr;
    std::vector<void*> children;
};

struct BroSceneGraphImpl {
    BroSceneNodeImpl* root = nullptr;
    double cameraX = 0.0;
    double cameraY = 0.0;
    double cameraZoom = 1.0;
    bool showLightIcons = false;
    bool frustumCulling = true;
    bool shadowCache = false;
    double renderScale = 1.0;
    double msaa = 0.0;
    void* activeCamera = nullptr;
    std::vector<void*> nodes;

    BroSceneGraphImpl() {
        root = new BroSceneNodeImpl();
        root->name = "root";
    }

    ~BroSceneGraphImpl() {
        delete root;
        for (void* n : nodes) {
            delete static_cast<BroSceneNodeImpl*>(n);
        }
    }
};

extern "C" {

// --- Interface bro.scene.SceneNode ---
void* bro_SceneNode_create(void) {
    return new BroSceneNodeImpl();
}

void bro_SceneNode_destroy(void* self) {
    delete static_cast<BroSceneNodeImpl*>(self);
}

int32_t bro_SceneNode_get_id(void* self) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    return n ? n->id : 0;
}

const char* bro_SceneNode_get_name(void* self) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    return n ? n->name.c_str() : "";
}

void bro_SceneNode_set_name(void* self, const char* val) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    if (n && val) n->name = val;
}

bool bro_SceneNode_get_visible(void* self) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    return n ? n->visible : false;
}

void bro_SceneNode_set_visible(void* self, bool val) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    if (n) n->visible = val;
}

void* bro_SceneNode_get_position(void* /*self*/) { return nullptr; }
void bro_SceneNode_set_position(void* /*self*/, void* /*val*/) {}
void* bro_SceneNode_get_rotation(void* /*self*/) { return nullptr; }
void bro_SceneNode_set_rotation(void* /*self*/, void* /*val*/) {}
void* bro_SceneNode_get_scale(void* /*self*/) { return nullptr; }
void bro_SceneNode_set_scale(void* /*self*/, void* /*val*/) {}
void* bro_SceneNode_get_worldPosition(void* /*self*/) { return nullptr; }
void* bro_SceneNode_get_worldMatrix(void* /*self*/) { return nullptr; }

void* bro_SceneNode_get_parent(void* self) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    return n ? n->parent : nullptr;
}

void* bro_SceneNode_get_children(void* /*self*/) { return nullptr; }

void* bro_SceneNode_add(void* self, void* child) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    auto* c = static_cast<BroSceneNodeImpl*>(child);
    if (n && c) {
        c->parent = n;
        n->children.push_back(c);
    }
    return self;
}

void bro_SceneNode_remove(void* self, void* child) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    auto* c = static_cast<BroSceneNodeImpl*>(child);
    if (n && c) {
        auto it = std::find(n->children.begin(), n->children.end(), child);
        if (it != n->children.end()) {
            n->children.erase(it);
            c->parent = nullptr;
        }
    }
}

void* bro_SceneNode_addChild(void* self, void* child) {
    return bro_SceneNode_add(self, child);
}

void bro_SceneNode_removeChild(void* self, void* child) {
    bro_SceneNode_remove(self, child);
}

void* bro_SceneNode_setPosition(void* self, double x, double y, double z) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    if (n) { n->posX = x; n->posY = y; n->posZ = z; }
    return self;
}

void* bro_SceneNode_setRotation(void* self, double x, double y, double z, double w) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    if (n) { n->rotX = x; n->rotY = y; n->rotZ = z; n->rotW = w; }
    return self;
}

void* bro_SceneNode_setScale(void* self, double x, double y, double z) {
    auto* n = static_cast<BroSceneNodeImpl*>(self);
    if (n) { n->scaleX = x; n->scaleY = y; n->scaleZ = z; }
    return self;
}

void* bro_SceneNode_lookAt(void* self, void* /*target*/, void* /*up*/) { return self; }
void* bro_SceneNode_setMaterial(void* self, void* /*mat*/) { return self; }
void* bro_SceneNode_setSkeleton(void* self, void* /*skeleton*/) { return self; }
void* bro_SceneNode_addClip(void* self, const char* /*name*/, void* /*anim*/) { return self; }
void* bro_SceneNode_getBoneWorldMatrix(void* /*self*/, void* /*nameOrIndex*/) { return nullptr; }
void* bro_SceneNode_addBlendSpace1D(void* self, const char* /*name*/, void* /*clips*/) { return self; }
void* bro_SceneNode_addBlendSpace2D(void* self, const char* /*name*/, void* /*clips*/) { return self; }
void* bro_SceneNode_setBlendPos(void* self, const char* /*name*/, double /*x*/, double /*y*/) { return self; }
void* bro_SceneNode_blendState(void* /*self*/, const char* /*name*/) { return nullptr; }
void* bro_SceneNode_playLayer(void* self, int32_t /*layer*/, const char* /*clipName*/, double /*weight*/, double /*fadeTime*/) { return self; }
void* bro_SceneNode_stopLayer(void* self, int32_t /*layer*/, double /*fadeTime*/) { return self; }
void* bro_SceneNode_setLayerWeight(void* self, int32_t /*layer*/, double /*weight*/) { return self; }
void* bro_SceneNode_addStateMachine(void* self, const char* /*name*/, void* /*def*/) { return self; }
bool bro_SceneNode_travel(void* /*self*/, const char* /*name*/, const char* /*targetState*/) { return false; }
void* bro_SceneNode_setRootMotion(void* self, bool /*enabled*/) { return self; }
void* bro_SceneNode_consumeRootMotion(void* /*self*/) { return nullptr; }
void* bro_SceneNode_play(void* self, const char* /*clipName*/, void* /*opts*/) { return self; }
void* bro_SceneNode_stop(void* self) { return self; }
void* bro_SceneNode_pause(void* self) { return self; }
void* bro_SceneNode_resume(void* self) { return self; }
void bro_SceneNode_setInstanceTransform(void* /*self*/, int32_t /*index*/, void* /*matrix*/) {}
void bro_SceneNode_setInstanceColor(void* /*self*/, int32_t /*index*/, void* /*color*/) {}
void bro_SceneNode_setInstanceCount(void* /*self*/, int32_t /*count*/) {}
void* bro_SceneNode_setHtml(void* self, const char* /*html*/) { return self; }
void bro_SceneNode_markHtmlDirty(void* /*self*/) {}
void bro_SceneNode_burst(void* /*self*/, int32_t /*count*/) {}
void bro_SceneNode_clear(void* /*self*/) {}
void bro_SceneNode_probeCapture(void* /*self*/) {}
void bro_SceneNode_savePly(void* /*self*/, const char* /*path*/) {}

// --- Interface bro.scene.SceneGraph ---
void* bro_SceneGraph_create(void) {
    return new BroSceneGraphImpl();
}

void bro_SceneGraph_destroy(void* self) {
    delete static_cast<BroSceneGraphImpl*>(self);
}

void* bro_SceneGraph_get_root(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->root : nullptr;
}

double bro_SceneGraph_get_cameraX(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->cameraX : 0.0;
}
void bro_SceneGraph_set_cameraX(void* self, double val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->cameraX = val;
}

double bro_SceneGraph_get_cameraY(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->cameraY : 0.0;
}
void bro_SceneGraph_set_cameraY(void* self, double val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->cameraY = val;
}

double bro_SceneGraph_get_cameraZoom(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->cameraZoom : 1.0;
}
void bro_SceneGraph_set_cameraZoom(void* self, double val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->cameraZoom = val;
}

bool bro_SceneGraph_get_showLightIcons(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->showLightIcons : false;
}
void bro_SceneGraph_set_showLightIcons(void* self, bool val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->showLightIcons = val;
}

bool bro_SceneGraph_get_frustumCulling(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->frustumCulling : true;
}
void bro_SceneGraph_set_frustumCulling(void* self, bool val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->frustumCulling = val;
}

bool bro_SceneGraph_get_shadowCache(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->shadowCache : false;
}
void bro_SceneGraph_set_shadowCache(void* self, bool val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->shadowCache = val;
}

double bro_SceneGraph_get_renderScale(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->renderScale : 1.0;
}
void bro_SceneGraph_set_renderScale(void* self, double val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->renderScale = val;
}

double bro_SceneGraph_get_msaa(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->msaa : 0.0;
}
void bro_SceneGraph_set_msaa(void* self, double val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->msaa = val;
}

void* bro_SceneGraph_get_activeCamera(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    return g ? g->activeCamera : nullptr;
}
void bro_SceneGraph_set_activeCamera(void* self, void* val) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->activeCamera = val;
}

void* bro_SceneGraph_get_viewMatrix(void* /*self*/) { return nullptr; }
void* bro_SceneGraph_get_projectionMatrix(void* /*self*/) { return nullptr; }
void* bro_SceneGraph_get_cameraEye(void* /*self*/) { return nullptr; }

void* bro_SceneGraph_createNode(void* self, void* /*opts*/) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    auto* node = new BroSceneNodeImpl();
    if (g) {
        g->nodes.push_back(node);
        if (g->root) g->root->children.push_back(node);
    }
    return node;
}

void* bro_SceneGraph_createShape(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createSprite(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createPhysicsNode(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createMesh(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createSkinnedMesh(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createInstancedMesh(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createGaussianSplat(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createHtmlNode(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createLight(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createParticles(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createParticles3D(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createDecal(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createReflectionProbe(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void* bro_SceneGraph_createTween(void* /*self*/) { return nullptr; }
void* bro_SceneGraph_createAnimationPlayer(void* /*self*/) { return nullptr; }
void* bro_SceneGraph_createTerrain(void* /*self*/, void* /*opts*/) { return nullptr; }
void* bro_SceneGraph_createClipmapTerrain(void* /*self*/, void* /*opts*/) { return nullptr; }
void* bro_SceneGraph_createTileWorld(void* /*self*/, void* /*opts*/) { return nullptr; }

void* bro_SceneGraph_findById(void* self, int32_t id) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (!g) return nullptr;
    if (g->root && g->root->id == id) return g->root;
    for (void* p : g->nodes) {
        auto* n = static_cast<BroSceneNodeImpl*>(p);
        if (n && n->id == id) return n;
    }
    return nullptr;
}

void* bro_SceneGraph_findByName(void* self, const char* name) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (!g || !name) return nullptr;
    if (g->root && g->root->name == name) return g->root;
    for (void* p : g->nodes) {
        auto* n = static_cast<BroSceneNodeImpl*>(p);
        if (n && n->name == name) return n;
    }
    return nullptr;
}

void bro_SceneGraph_destroyNode(void* self, void* node) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g && node) {
        auto it = std::find(g->nodes.begin(), g->nodes.end(), node);
        if (it != g->nodes.end()) {
            g->nodes.erase(it);
            delete static_cast<BroSceneNodeImpl*>(node);
        }
    }
}

void bro_SceneGraph_setCamera(void* /*self*/, void* /*opts*/) {}
void* bro_SceneGraph_createCamera(void* self, void* opts) { return bro_SceneGraph_createNode(self, opts); }
void bro_SceneGraph_setActiveCamera(void* self, void* camera) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->activeCamera = camera;
}

void bro_SceneGraph_setToneMap(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setAmbient(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setWind(void* /*self*/, void* /*dir*/, double /*speed*/) {}
void bro_SceneGraph_setShadowQuality(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setShadowCache(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setFog(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setAtmosphere(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setStarfield(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setTiltShift(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setBloom(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setSSAO(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setSSR(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setDepthOfField(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setColorLUT(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setFXAA(void* /*self*/, bool /*enabled*/) {}
void bro_SceneGraph_setRenderScale(void* self, double scale) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->renderScale = scale;
}
void bro_SceneGraph_setMSAA(void* self, int32_t samples) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->msaa = static_cast<double>(samples);
}
void bro_SceneGraph_setEnvironment(void* /*self*/, void* /*opts*/) {}
void bro_SceneGraph_setFrustumCulling(void* self, bool enabled) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) g->frustumCulling = enabled;
}
void* bro_SceneGraph_cullStats(void* /*self*/) { return nullptr; }

void bro_SceneGraph_clear(void* self) {
    auto* g = static_cast<BroSceneGraphImpl*>(self);
    if (g) {
        for (void* n : g->nodes) {
            delete static_cast<BroSceneNodeImpl*>(n);
        }
        g->nodes.clear();
        if (g->root) g->root->children.clear();
    }
}

void bro_SceneGraph_syncPhysics(void* /*self*/) {}
void* bro_SceneGraph_raycast(void* /*self*/, void* /*origin*/, void* /*direction*/) { return nullptr; }
void* bro_SceneGraph_unprojectLocal(void* /*self*/, void* /*node*/, void* /*screenPoint*/) { return nullptr; }
void* bro_SceneGraph_toImageData(void* /*self*/) { return nullptr; }
void* bro_SceneGraph_captureFrame(void* /*self*/, const char* /*format*/, double /*quality*/) { return nullptr; }
void* bro_SceneGraph_asTexture(void* /*self*/) { return nullptr; }
void bro_SceneGraph_bindAudioListenerToCamera(void* /*self*/, bool /*bind*/) {}
void bro_SceneGraph_attachAIWorld(void* /*self*/, void* /*aiWorld*/, void* /*opts*/) {}
void bro_SceneGraph_detachAIWorld(void* /*self*/) {}

} // extern "C"