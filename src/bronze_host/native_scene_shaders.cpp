#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include <bromesh/mesh_data.h>
#include "scene/particles3d_node.h"
#include "scene/decal_node.h"
#include "scene/sprite_node.h"
#include <json.hpp>

namespace bro::bronze_host {

using json = nlohmann::json;

static thread_local std::string tl_shaderErr;
static thread_local double tl_visRangeBuf[3];

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

bool bro_scene_SceneNode_hasShader(void* self) {
    auto* n = nodeOf(self);
    if (!n) return false;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        return static_cast<scene::MeshNode*>(n)->hasCustomShader();
    }
    if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        return static_cast<scene::InstancedMeshNode*>(n)->hasCustomShader();
    }
    return false;
}

const char* bro_scene_SceneNode_setShader(void* self, const char* vertex, const char* fragment,
                                          const char* uniformsJson) {
    auto* n = nodeOf(self);
    auto* cell = nodeCellOf(self);
    auto* g = cell ? cell->graph() : nullptr;
    if (!n || !g) {
        tl_shaderErr = "setShader: node or scene destroyed";
        return tl_shaderErr.c_str();
    }

    std::string vChunk = vertex ? vertex : "";
    std::string fChunk = fragment ? fragment : "";
    if (vChunk.empty() && fChunk.empty()) {
        tl_shaderErr = "setShader: neither vertex nor fragment provided";
        return tl_shaderErr.c_str();
    }

    scene::SceneRenderer::CustomShaderTarget target = scene::SceneRenderer::CustomShaderTarget::Static;
    bool isSkinned = false;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (mn->asSkinnedMesh()) {
            target = scene::SceneRenderer::CustomShaderTarget::Skinned;
            isSkinned = true;
        }
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        target = scene::SceneRenderer::CustomShaderTarget::Instanced;
    } else {
        tl_shaderErr = "setShader: node is not a MeshNode or InstancedMeshNode";
        return tl_shaderErr.c_str();
    }

    std::string key = vChunk + '\x1f' + fChunk;
    std::string errStr;
    bool ok = g->compileCustomShader(target, key, vChunk, fChunk, errStr);
    if (ok && isSkinned) {
        // Skinned meshes degrade to the static path if unposed/no skeleton; pre-compile static variant too
        std::string errStatic;
        g->compileCustomShader(scene::SceneRenderer::CustomShaderTarget::Static, key, vChunk, fChunk, errStatic);
    }

    if (!ok) {
        tl_shaderErr = errStr.empty() ? "shader compilation failed" : errStr;
        return tl_shaderErr.c_str();
    }

    // Install shader
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setCustomShader(vChunk, fChunk);
    } else {
        static_cast<scene::InstancedMeshNode*>(n)->setCustomShader(vChunk, fChunk);
    }

    // Apply uniforms if present
    if (uniformsJson && *uniformsJson) {
        auto j = json::parse(uniformsJson, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string& uname = it.key();
                if (uname.rfind("u_", 0) != 0) continue; // must start with u_
                float vals[4] = {0, 0, 0, 0};
                int comps = 1;
                if (it.value().is_number()) {
                    vals[0] = it.value().get<float>();
                    comps = 1;
                } else if (it.value().is_array()) {
                    comps = std::clamp(static_cast<int>(it.value().size()), 1, 4);
                    for (int c = 0; c < comps; ++c) {
                        vals[c] = it.value()[c].get<float>();
                    }
                }
                if (n->type() == scene::SceneNode::Type::Mesh) {
                    static_cast<scene::MeshNode*>(n)->setCustomShaderUniform(uname, comps, vals);
                } else {
                    static_cast<scene::InstancedMeshNode*>(n)->setCustomShaderUniform(uname, comps, vals);
                }
            }
        }
    }

    return "";
}


void bro_scene_SceneNode_clearShader(void* self) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->clearCustomShader();
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->clearCustomShader();
    }
}

void bro_scene_SceneNode_setShaderUniform(void* self, const char* name, const double* vals, uint32_t count) {
    auto* n = nodeOf(self);
    if (!n || !name || !vals || count == 0) return;
    float fvals[4] = {0, 0, 0, 0};
    int comps = std::clamp(static_cast<int>(count), 1, 4);
    for (int i = 0; i < comps; ++i) fvals[i] = static_cast<float>(vals[i]);

    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setCustomShaderUniform(name, comps, fvals);
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setCustomShaderUniform(name, comps, fvals);
    }
}

void bro_scene_SceneNode_setLodMeshes(void* self, const char* jsonLods) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh || !jsonLods) return;
    auto* mn = static_cast<scene::MeshNode*>(n);
    std::vector<scene::MeshNode::LodLevel> levels;
    auto j = json::parse(jsonLods, nullptr, false);
    if (j.is_array()) {
        for (const auto& item : j) {
            if (!item.is_object()) continue;
            scene::MeshNode::LodLevel lvl;
            lvl.maxDist = item.value("maxDist", 0.0f);
            if (item.contains("meshPositions") && item["meshPositions"].is_array()) {
                for (auto v : item["meshPositions"]) lvl.mesh.positions.push_back(v.get<float>());
            }
            if (item.contains("meshNormals") && item["meshNormals"].is_array()) {
                for (auto v : item["meshNormals"]) lvl.mesh.normals.push_back(v.get<float>());
            }
            if (item.contains("meshIndices") && item["meshIndices"].is_array()) {
                for (auto v : item["meshIndices"]) lvl.mesh.indices.push_back(v.get<uint32_t>());
            }
            levels.push_back(std::move(lvl));
        }
    }
    mn->setLodMeshes(std::move(levels));
}

int32_t bro_scene_SceneNode_lodCount(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Mesh) {
        return static_cast<scene::MeshNode*>(n)->lodCount();
    }
    return 0;
}

int32_t bro_scene_SceneNode_lodLevel(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Mesh) {
        return static_cast<scene::MeshNode*>(n)->selectedLod();
    }
    return 0;
}

void bro_scene_SceneNode_visibilityRange_set(void* self, double begin, double end, double margin) {
    auto* n = nodeOf(self);
    if (n) {
        n->setVisibilityRange(static_cast<float>(begin), static_cast<float>(end), static_cast<float>(margin));
    }
}

void bro_scene_SceneNode_visibilityRange_clear(void* self) {
    auto* n = nodeOf(self);
    if (n) {
        n->clearVisibilityRange();
    }
}

void bro_scene_SceneNode_visibilityRange_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->hasVisibilityRange()) {
        tl_visRangeBuf[0] = n->visibilityRangeBegin();
        tl_visRangeBuf[1] = n->visibilityRangeEnd();
        tl_visRangeBuf[2] = n->visibilityRangeMargin();
        copyBuffer(tl_visRangeBuf, 3, out);
    } else {
        copyBuffer<double>(nullptr, 0, out);
    }
}

bool bro_scene_SceneGraph_isValid(void* self) {
    return graphOf(self) != nullptr;
}

void bro_scene_SceneNode_setBaseColorTextureFromScene(void* self, void* sourceScene) {
    auto* n = nodeOf(self);
    auto* srcGraph = graphOf(sourceScene);
    if (!n || n->type() != scene::SceneNode::Type::Mesh || !srcGraph) return;
    auto* mn = static_cast<scene::MeshNode*>(n);
    auto srcToken = srcGraph->outputTextureSource();
    std::weak_ptr<scene::SceneGraph::OutputTextureSource> weak = srcToken;
    mn->setExternalBaseColorTexture([weak]() -> unsigned {
        auto locked = weak.lock();
        if (!locked || !locked->graph) return 0;
        return locked->graph->outputColorTexture();
    });
}

bool bro_scene_SceneNode_setShaderTexture(void* self, const char* name, int32_t x, int32_t y,
                                          int32_t width, int32_t height,
                                          const float* data, uint32_t dataCount,
                                          bool mipmap, bool isSub) {
    auto* n = nodeOf(self);
    if (!n || !name) return false;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (isSub) {
            return mn->updateCustomShaderTexture(name, x, y, width, height, data);
        } else {
            return mn->setCustomShaderTexture(name, width, height, data, mipmap);
        }
    }
    return false;
}

void bro_scene_SceneNode_setBaseColorTextureData(void* self, int32_t width, int32_t height,
                                                 const uint8_t* data, uint32_t len) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (data && width > 0 && height > 0) mn->setBaseColorTexture(width, height, data);
        else mn->clearBaseColorTexture();
    } else if (n->type() == scene::SceneNode::Type::Decal) {
        auto* dn = static_cast<scene::DecalNode*>(n);
        if (data && width > 0 && height > 0) dn->setAlbedoTexture(width, height, data);
        else dn->clearAlbedoTexture();
    }
}

void bro_scene_SceneNode_clearBaseColorTexture(void* self) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->clearBaseColorTexture();
    } else if (n->type() == scene::SceneNode::Type::Decal) {
        static_cast<scene::DecalNode*>(n)->clearAlbedoTexture();
    }
}

void bro_scene_SceneNode_setEmissionTextureData(void* self, int32_t width, int32_t height,
                                                const uint8_t* data, uint32_t len) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Decal) {
        auto* dn = static_cast<scene::DecalNode*>(n);
        if (data && width > 0 && height > 0) dn->setEmissionTexture(width, height, data);
        else dn->clearEmissionTexture();
    } else if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (data && width > 0 && height > 0) mn->setEmissiveTexture(width, height, data);
        else mn->clearEmissiveTexture();
    }
}

void bro_scene_SceneNode_onAnimationEnd_set(void* self, uint64_t cbBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Sprite) return;
    auto* sn = static_cast<scene::SpriteNode*>(n);
    Value cb = ev::fromBits(cbBits);
    if (ev::isFunction(cb)) {
        auto p = std::make_shared<ev::Persistent>(cb);
        sn->setOnAnimationEnd([p](const std::string& clipName) {
            ev::Persistent arg{ev::fromUtf8(clipName)};
            Value args[1] = {arg.get()};
            ev::call(p->get(), ev::undefined(), args);
        });
    } else {
        sn->setOnAnimationEnd(nullptr);
    }
}

int32_t bro_scene_SceneNode_renderPriority_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        return static_cast<scene::DecalNode*>(n)->renderPriority();
    }
    return 0;
}
void bro_scene_SceneNode_renderPriority_set(void* self, int32_t p) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        static_cast<scene::DecalNode*>(n)->setRenderPriority(p);
    }
}

double bro_scene_SceneNode_emissionStrength_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        return static_cast<scene::DecalNode*>(n)->emissionStrength();
    }
    return 1.0;
}
void bro_scene_SceneNode_emissionStrength_set(void* self, double s) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        static_cast<scene::DecalNode*>(n)->setEmissionStrength(static_cast<float>(s));
    }
}

double bro_scene_SceneNode_upperFade_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        return static_cast<scene::DecalNode*>(n)->upperFade();
    }
    return 0.0;
}
void bro_scene_SceneNode_upperFade_set(void* self, double f) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        static_cast<scene::DecalNode*>(n)->setUpperFade(static_cast<float>(f));
    }
}

double bro_scene_SceneNode_lowerFade_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        return static_cast<scene::DecalNode*>(n)->lowerFade();
    }
    return 0.0;
}
void bro_scene_SceneNode_lowerFade_set(void* self, double f) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        static_cast<scene::DecalNode*>(n)->setLowerFade(static_cast<float>(f));
    }
}

double bro_scene_SceneNode_normalFade_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        return static_cast<scene::DecalNode*>(n)->normalFade();
    }
    return 0.0;
}
void bro_scene_SceneNode_normalFade_set(void* self, double f) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        static_cast<scene::DecalNode*>(n)->setNormalFade(static_cast<float>(f));
    }
}

void bro_scene_SceneNode_modulate_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    static thread_local double tl_mod[4];
    tl_mod[0] = 1.0; tl_mod[1] = 1.0; tl_mod[2] = 1.0; tl_mod[3] = 1.0;
    if (n && n->type() == scene::SceneNode::Type::Decal) {
        const float* m = static_cast<scene::DecalNode*>(n)->modulate();
        tl_mod[0] = m[0]; tl_mod[1] = m[1]; tl_mod[2] = m[2]; tl_mod[3] = m[3];
    }
    out->data = tl_mod;
    out->length = 4;
    out->release = nullptr;
}
void bro_scene_SceneNode_modulate_set(void* self, const double* data, uint32_t len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Decal && data && len >= 4) {
        static_cast<scene::DecalNode*>(n)->setModulate(
            static_cast<float>(data[0]), static_cast<float>(data[1]),
            static_cast<float>(data[2]), static_cast<float>(data[3]));
    }
}

int32_t bro_scene_SceneNode_particleCount_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->liveCount();
    }
    return 0;
}
bool bro_scene_SceneNode_particlePlaying_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->isPlaying();
    }
    return false;
}
double bro_scene_SceneNode_particleRate_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->rate();
    }
    return 0.0;
}
void bro_scene_SceneNode_particleRate_set(void* self, double r) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->setRate(static_cast<float>(r));
    }
}
double bro_scene_SceneNode_softness_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->softness();
    }
    return 0.0;
}
void bro_scene_SceneNode_softness_set(void* self, double s) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->setSoftness(static_cast<float>(s));
    }
}
void bro_scene_SceneNode_onFinished_set(void* self, uint64_t cbBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Particles3D) return;
    auto* pn = static_cast<scene::Particles3DNode*>(n);
    Value cb = ev::fromBits(cbBits);
    if (ev::isFunction(cb)) {
        auto p = std::make_shared<ev::Persistent>(cb);
        pn->setOnFinished([p]() {
            ev::call(p->get(), ev::undefined(), {});
        });
    } else {
        pn->setOnFinished(nullptr);
    }
}

} // extern "C"

namespace bro::bronze_host {

bool registerSceneShaderNatives(std::string* error) {
    using namespace natives;
    return fn("__bro_native.scene.SceneNode_setShaderTexture", (void*)&bro_scene_SceneNode_setShaderTexture, "bool", {"__bro_native.scene.SceneNode", "str", "i32", "i32", "i32", "i32", "f32[]", "bool", "bool"}, error) &&
           fn("__bro_native.scene.SceneNode_setBaseColorTextureData", (void*)&bro_scene_SceneNode_setBaseColorTextureData, "void", {"__bro_native.scene.SceneNode", "i32", "i32", "u8[]"}, error) &&
           fn("__bro_native.scene.SceneNode_clearBaseColorTexture", (void*)&bro_scene_SceneNode_clearBaseColorTexture, "void", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_setEmissionTextureData", (void*)&bro_scene_SceneNode_setEmissionTextureData, "void", {"__bro_native.scene.SceneNode", "i32", "i32", "u8[]"}, error) &&
           fn("__bro_native.scene.SceneNode_onAnimationEnd_set", (void*)&bro_scene_SceneNode_onAnimationEnd_set, "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneNode_renderPriority_get", (void*)&bro_scene_SceneNode_renderPriority_get, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_renderPriority_set", (void*)&bro_scene_SceneNode_renderPriority_set, "void", {"__bro_native.scene.SceneNode", "i32"}, error) &&
           fn("__bro_native.scene.SceneNode_emissionStrength_get", (void*)&bro_scene_SceneNode_emissionStrength_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_emissionStrength_set", (void*)&bro_scene_SceneNode_emissionStrength_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_upperFade_get", (void*)&bro_scene_SceneNode_upperFade_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_upperFade_set", (void*)&bro_scene_SceneNode_upperFade_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_lowerFade_get", (void*)&bro_scene_SceneNode_lowerFade_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_lowerFade_set", (void*)&bro_scene_SceneNode_lowerFade_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_normalFade_get", (void*)&bro_scene_SceneNode_normalFade_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_normalFade_set", (void*)&bro_scene_SceneNode_normalFade_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_modulate_get", (void*)&bro_scene_SceneNode_modulate_get, "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_modulate_set", (void*)&bro_scene_SceneNode_modulate_set, "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           fn("__bro_native.scene.SceneNode_particleCount_get", (void*)&bro_scene_SceneNode_particleCount_get, "i32", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_particlePlaying_get", (void*)&bro_scene_SceneNode_particlePlaying_get, "bool", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_particleRate_get", (void*)&bro_scene_SceneNode_particleRate_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_particleRate_set", (void*)&bro_scene_SceneNode_particleRate_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_softness_get", (void*)&bro_scene_SceneNode_softness_get, "f64", {"__bro_native.scene.SceneNode"}, error) &&
           fn("__bro_native.scene.SceneNode_softness_set", (void*)&bro_scene_SceneNode_softness_set, "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           fn("__bro_native.scene.SceneNode_onFinished_set", (void*)&bro_scene_SceneNode_onFinished_set, "void", {"__bro_native.scene.SceneNode", "dynamic"}, error);
}

} // namespace bro::bronze_host
