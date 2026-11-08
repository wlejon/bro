#include "bronze_host/native_scene_internal.h"
#include "natives/scene/native_scene_decl.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_rooted.h"
#if BRO_WITH_KERNEL
#include "bronze_host/host_kernel_gpu.h"
#endif
#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/api.h>
#include "render/vulkan_compute.h"
#include "scene/nodes/particles3d_node.h"
#include "scene/nodes/particle_node.h"
#include "scene/nodes/decal_node.h"
#include "scene/nodes/sprite_node.h"
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

const char* bro_scene_SceneNode_setShaderModule(void* self, const char* module, const char* file,
                                                const char* modulesJson) {
    auto* n = nodeOf(self);
    auto* cell = nodeCellOf(self);
    auto* g = cell ? cell->graph() : nullptr;
    if (!n || !g) {
        tl_shaderErr = "setShaderModule: node or scene destroyed";
        return tl_shaderErr.c_str();
    }
    ShaderModuleArgs args;
    std::string err;
    if (!readShaderModuleArgs(n, module, file, modulesJson, args, err)) {
        tl_shaderErr = "setShaderModule: " + err;
        return tl_shaderErr.c_str();
    }
    const auto target = args.target;
    const bool isSkinned = args.isSkinned;
    const std::string& source = args.source;
    const std::string& name = args.file;
    auto& modules = args.modules;
    auto& knobs = args.knobs;
    bool hasVertex = false;
    if (!g->compileShaderModule(target, source, name, modules, hasVertex, err, knobs)) {
        tl_shaderErr = err.empty() ? "module compilation failed" : err;
        return tl_shaderErr.c_str();
    }
    if (isSkinned) {
        bool staticVertex = false;
        std::string errStatic;
        g->compileShaderModule(scene::SceneRenderer::CustomShaderTarget::Static, source, name, modules, staticVertex,
                               errStatic, knobs);
    }
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setCustomShaderModule(source, name, std::move(modules), hasVertex,
                                                                std::move(knobs));
    } else {
        static_cast<scene::InstancedMeshNode*>(n)->setCustomShaderModule(source, name, std::move(modules), hasVertex,
                                                                         std::move(knobs));
    }
    return "";
}

const char* bro_scene_SceneNode_setShaderKnobs(void* self, const char* knobsJson) {
    auto* n = nodeOf(self);
    auto* cell = nodeCellOf(self);
    auto* g = cell ? cell->graph() : nullptr;
    if (!n || !g) {
        tl_shaderErr = "setShaderKnobs: node or scene destroyed";
        return tl_shaderErr.c_str();
    }
    const scene::CustomShaderState* cs = nullptr;
    scene::SceneRenderer::CustomShaderTarget target = scene::SceneRenderer::CustomShaderTarget::Static;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* mn = static_cast<scene::MeshNode*>(n);
        cs = mn->customShader();
        if (mn->isVolume()) target = scene::SceneRenderer::CustomShaderTarget::Volume;
        else if (mn->asSkinnedMesh()) target = scene::SceneRenderer::CustomShaderTarget::Skinned;
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        cs = static_cast<scene::InstancedMeshNode*>(n)->customShader();
        target = scene::SceneRenderer::CustomShaderTarget::Instanced;
    } else {
        tl_shaderErr = "setShaderKnobs: node is not a MeshNode or InstancedMeshNode";
        return tl_shaderErr.c_str();
    }
    if (!cs || !cs->isModule()) {
        tl_shaderErr = "setShaderKnobs: node has no custom shader module";
        return tl_shaderErr.c_str();
    }
    std::vector<std::pair<std::string, double>> overrides;
    if (knobsJson && *knobsJson) {
        auto j = json::parse(knobsJson, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            tl_shaderErr = "setShaderKnobs: expected an object mapping knob names to numbers/booleans";
            return tl_shaderErr.c_str();
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_number()) {
                overrides.emplace_back(it.key(), it.value().get<double>());
            } else if (it.value().is_boolean()) {
                overrides.emplace_back(it.key(), it.value().get<bool>() ? 1.0 : 0.0);
            } else {
                tl_shaderErr = "setShaderKnobs: knob '" + it.key() + "' must be a number or boolean";
                return tl_shaderErr.c_str();
            }
        }
    }
    std::vector<std::pair<std::string, double>> merged = cs->knobs;
    for (const auto& [k, v] : overrides) {
        bool found = false;
        for (auto& ex : merged) {
            if (ex.first == k) { ex.second = v; found = true; break; }
        }
        if (!found) merged.emplace_back(k, v);
    }
    std::string err;
    bool hasVertex = false;
    if (!g->compileShaderModule(target, cs->module, cs->moduleFile, cs->modules, hasVertex, err, merged)) {
        tl_shaderErr = err.empty() ? "knob specialization failed" : err;
        return tl_shaderErr.c_str();
    }
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setCustomShaderKnobs(std::move(overrides));
    } else {
        static_cast<scene::InstancedMeshNode*>(n)->setCustomShaderKnobs(std::move(overrides));
    }
    return "";
}

const char* bro_scene_SceneNode_shaderStatsJson(void* self) {
    static thread_local std::string tl_statsJson;
    auto* n = nodeOf(self);
    auto* cell = nodeCellOf(self);
    auto* g = cell ? cell->graph() : nullptr;
    if (!n || !g) return "";
    scene::NodeShaderStats stats;
    if (!g->shaderStats(n->id(), stats)) return "";
    auto stageToJson = [](const scene::ShaderStageStats& s) {
        json j = json::object();
        j["spirvBytes"] = s.spirvBytes;
        j["instructions"] = s.instructions;
        j["arithmetic"] = s.arithmetic;
        j["samples"] = s.samples;
        j["loads"] = s.loads;
        j["stores"] = s.stores;
        j["branches"] = s.branches;
        j["loops"] = s.loops;
        // Source functions by what each built (the stage is one inlined
        // function): self, total with its callees, total's share of the stage.
        json fns = json::array();
        const double whole = s.functionCosts.empty() ? 0.0 : double(s.functionCosts.front().total);
        for (const auto& c : s.functionCosts) {
            fns.push_back({{"name", c.name}, {"self", c.self}, {"total", c.total},
                           {"share", whole > 0.0 ? c.total / whole : 0.0}});
        }
        j["functions"] = std::move(fns);
        j["sgprs"] = s.sgprs;
        j["vgprs"] = s.vgprs;
        j["scratchBytes"] = s.scratchBytes;
        json execs = json::array();
        for (const auto& ex : s.executables) {
            json ej = json::object();
            ej["name"] = ex.name;
            ej["description"] = ex.description;
            if (ex.isBool) ej["value"] = (ex.uval != 0);
            else if (ex.isFloat) ej["value"] = ex.fval;
            else if (ex.ival < 0) ej["value"] = ex.ival;
            else ej["value"] = ex.uval;
            execs.push_back(std::move(ej));
        }
        j["executables"] = std::move(execs);
        return j;
    };
    json out = json::object();
    if (stats.fragment.present) out["fragment"] = stageToJson(stats.fragment);
    if (stats.vertex.present) out["vertex"] = stageToJson(stats.vertex);
    if (stats.nearStage.present) out["near"] = stageToJson(stats.nearStage);
    tl_statsJson = out.dump();
    return tl_statsJson.c_str();
}

void bro_scene_SceneGraph_reloadShaders(void* self, const char* sourcesJson) {
    auto* g = graphOf(self);
    if (!g) return;
    // { name: source }: a hook whose file or imported module is a name takes it.
    std::vector<std::pair<std::string, std::string>> sources;
    if (sourcesJson && *sourcesJson) {
        auto j = json::parse(sourcesJson, nullptr, false);
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_string()) sources.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
    }
    g->reloadShaders(sources);
}

const char* bro_scene_SceneGraph_probeJson(void* self, int32_t x, int32_t y) {
    static thread_local std::string tl_probeJson;
    auto* g = graphOf(self);
    if (!g) return "{}";
    const auto results = g->probe(x, y);
    json out = json::object();
    json nodesObj = json::object();
    for (const auto& r : results) {
        json nodeJson = json::object();
        for (const auto& pv : r.values) {
            json val;
            if (pv.components <= 1) {
                val = pv.values[0];
            } else {
                val = json::array();
                for (int c = 0; c < pv.components; ++c) val.push_back(pv.values[c]);
            }
            nodeJson[pv.name] = val;
            out[pv.name] = val;
        }
        nodesObj[r.nodeName] = nodeJson;
        if (!out.contains(r.nodeName)) {
            out[r.nodeName] = nodeJson;
        }
    }
    out["nodes"] = std::move(nodesObj);
    tl_probeJson = out.dump();
    return tl_probeJson.c_str();
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

// SceneNode.updateMesh (docs/scene-nodes-api.js): swap a MeshNode's geometry
// without touching the node. `mesh` is either a Mesh handle or a
// {positions, indices, normals?, uvs?, colors?, tangents?} object, which is
// the shape createMesh reads; normals are computed when absent or when
// asked for, exactly as createMesh does it.
void bro_scene_SceneNode_updateMesh(void* self, uint64_t meshBits, bool recomputeNormals) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) {
        ev::throwTypeError("updateMesh: node is not a MeshNode");
        return;
    }
    const Rooted meshVal(ev::fromBits(meshBits));
    if (!ev::isObject(meshVal)) {
        ev::throwTypeError("updateMesh: expected a Mesh or { positions, indices, ... }");
        return;
    }
    bromesh::MeshData meshData;
    if (const auto* md = bromesh::api::meshDataOf(meshVal)) {
        meshData = *md;
    } else {
        if (!readFloatVector(ev::getProperty(meshVal, "positions"), meshData.positions) ||
            !readU32Vector(ev::getProperty(meshVal, "indices"), meshData.indices)) {
            ev::throwTypeError("updateMesh: positions (Float32Array) and indices (Uint32Array) are required");
            return;
        }
        readFloatVector(ev::getProperty(meshVal, "normals"), meshData.normals);
        readFloatVector(ev::getProperty(meshVal, "uvs"), meshData.uvs);
        readFloatVector(ev::getProperty(meshVal, "colors"), meshData.colors);
        readFloatVector(ev::getProperty(meshVal, "tangents"), meshData.tangents);
    }
    if ((recomputeNormals || meshData.normals.empty()) && !meshData.positions.empty()) {
        bromesh::computeNormals(meshData);
    }
    static_cast<scene::MeshNode*>(n)->setMesh(std::move(meshData));
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
    mn->setExternalSceneProvider([weak]() -> scene::SceneGraph* {
        auto locked = weak.lock();
        return locked ? locked->graph : nullptr;
    });
}

bool bro_scene_SceneNode_setShaderBuffer(void* self, const char* name, uint64_t bufferBits) {
    auto* n = nodeOf(self);
    if (!n || !name) return false;
    const Rooted value(ev::fromBits(bufferBits));
    std::shared_ptr<bro::render::ComputeBuffer> buffer;
    if (!ev::isNull(value) && !ev::isUndefined(value)) {
#if BRO_WITH_KERNEL
        buffer = computeBufferOf(value);
#endif
        if (!buffer) {
            ev::throwTypeError("setShaderBuffer: the buffer is not a KernelBuffer (bro.kernel.createBuffer)");
            return false;
        }
    }
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setCustomShaderBuffer(name, std::move(buffer));
        return true;
    }
    if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setCustomShaderBuffer(name, std::move(buffer));
        return true;
    }
    return false;
}

bool bro_scene_SceneNode_setInstanceCountBuffer(void* self, uint64_t bufferBits, int32_t word) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::InstancedMesh) return false;
    const Rooted value(ev::fromBits(bufferBits));
    std::shared_ptr<bro::render::ComputeBuffer> buffer;
    if (!ev::isNull(value) && !ev::isUndefined(value)) {
#if BRO_WITH_KERNEL
        buffer = computeBufferOf(value);
#endif
        if (!buffer) {
            ev::throwTypeError("setInstanceCountBuffer: the buffer is not a KernelBuffer (bro.kernel.createBuffer)");
            return false;
        }
        if (word < 0 || (static_cast<uint64_t>(word) + 1) * 4 > buffer->size()) {
            ev::throwRangeError("setInstanceCountBuffer: the word is outside the buffer");
            return false;
        }
    }
    static_cast<scene::InstancedMeshNode*>(n)->setInstanceCountBuffer(std::move(buffer),
                                                                      static_cast<uint32_t>(word < 0 ? 0 : word));
    return true;
}

bool bro_scene_SceneNode_setInstanceCullSpheres(void* self, uint64_t bufferBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::InstancedMesh) return false;
    const Rooted value(ev::fromBits(bufferBits));
    std::shared_ptr<bro::render::ComputeBuffer> buffer;
    if (!ev::isNull(value) && !ev::isUndefined(value)) {
#if BRO_WITH_KERNEL
        buffer = computeBufferOf(value);
#endif
        if (!buffer) {
            ev::throwTypeError("setInstanceCullSpheres: the buffer is not a KernelBuffer (bro.kernel.createBuffer)");
            return false;
        }
    }
    static_cast<scene::InstancedMeshNode*>(n)->setInstanceCullSpheres(std::move(buffer));
    return true;
}

bool bro_scene_SceneNode_setCullBounds(void* self, bool set, double dx0, double dy0, double dz0, double dx1, double dy1, double dz1) {
    const float x0 = float(dx0), y0 = float(dy0), z0 = float(dz0), x1 = float(dx1), y1 = float(dy1), z1 = float(dz1);
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::InstancedMesh) return false;
    auto* m = static_cast<scene::InstancedMeshNode*>(n);
    if (!set) {
        m->setCullBounds(std::nullopt);
        return true;
    }
    m->setCullBounds(bromath::AABB3{{x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, z0 < z1 ? z0 : z1},
                                    {x0 < x1 ? x1 : x0, y0 < y1 ? y1 : y0, z0 < z1 ? z1 : z0}});
    return true;
}

bool bro_scene_SceneNode_setShaderTextureBuffer(void* self, const char* name, uint64_t bufferBits, int32_t width,
                                                int32_t height, int32_t layers, int32_t channels, bool repeat,
                                                bool volume, int32_t encoding, int32_t atlas) {
    auto* n = nodeOf(self);
    if (!n || !name) return false;
    const Rooted value(ev::fromBits(bufferBits));
    std::shared_ptr<bro::render::ComputeBuffer> buffer;
#if BRO_WITH_KERNEL
    buffer = computeBufferOf(value);
#endif
    if (!buffer) {
        ev::throwTypeError("setShaderTexture: buffer is not a KernelBuffer (bro.kernel.createBuffer)");
        return false;
    }
    if (n->type() != scene::SceneNode::Type::Mesh && n->type() != scene::SceneNode::Type::InstancedMesh) return false;
    if (channels != 1 && channels != 2 && channels != 4) {
        ev::throwRangeError("setShaderTexture: channels must be 1, 2 or 4 for a buffer");
        return false;
    }
    using Enc = scene::MeshNode::UserTexture::Encoding;
    if (encoding < 0 || encoding > static_cast<int32_t>(Enc::Float16)) {
        ev::throwRangeError("setShaderTexture: format is 'float32', 'unorm8', 'unorm16' or 'float16'");
        return false;
    }
    const Enc enc = static_cast<Enc>(encoding);
    if (atlas < 1 || (atlas > 1 && (volume || layers <= 0))) {
        ev::throwRangeError("setShaderTexture: atlas is at least 1, and above 1 only for layers");
        return false;
    }
    const uint64_t bytes = static_cast<uint64_t>(std::max(width, 0)) * static_cast<uint64_t>(std::max(height, 0)) *
                           static_cast<uint64_t>(std::max(layers, 1)) * static_cast<uint64_t>(channels) *
                           static_cast<uint64_t>(scene::MeshNode::encodingBytes(enc));
    if (buffer->size() < bytes) {
        ev::throwRangeError("setShaderTexture: the buffer holds fewer than width * height * layers texels");
        return false;
    }
    if (n->type() == scene::SceneNode::Type::Mesh) {
        return static_cast<scene::MeshNode*>(n)->setCustomShaderTextureBuffer(
            name, width, height, layers, channels, std::move(buffer), repeat, volume, enc, atlas);
    }
    return static_cast<scene::InstancedMeshNode*>(n)->setCustomShaderTextureBuffer(
        name, width, height, layers, channels, std::move(buffer), repeat, volume, enc, atlas);
}

bool bro_scene_SceneNode_setShaderTexture(void* self, const char* name, int32_t x, int32_t y,
                                          int32_t width, int32_t height,
                                          const float* data, uint32_t dataCount,
                                          bool mipmap, bool isSub, bool repeat, int32_t layers, bool volume) {
    auto* n = nodeOf(self);
    if (!n || !name) return false;
    auto apply = [&](auto* mn) -> bool {
        if (volume && isSub) {
            ev::throwTypeError("setShaderTexture: a 3D slot (depth) is written whole, not by sub-rects");
            return false;
        }
        // An array slot: `layers` slices of width * height floats, one after
        // another in `data`, for a sampler2DArray (a sampler3D's z with
        // `volume`).
        if (layers > 0 && !isSub) {
            if (width <= 0 || height <= 0) {
                mn->clearCustomShaderTexture(name);
                return true;
            }
            const uint64_t slice = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
            if (!data || static_cast<uint64_t>(dataCount) < slice * static_cast<uint64_t>(layers)) {
                ev::throwRangeError("setShaderTexture: data holds fewer than width * height * layers floats");
                return false;
            }
            if (!mn->setCustomShaderTextureArray(name, width, height, layers, 1, mipmap, repeat, false, volume))
                return false;
            for (int32_t l = 0; l < layers; ++l)
                if (!mn->setCustomShaderTextureArrayLayer(name, l, data + slice * static_cast<uint64_t>(l))) return false;
            return true;
        }
        // The slot copies width * height * channels floats out of `data`: one
        // channel for a new slot, the slot's own count for a sub-rect write
        // (an engine-made slot may be RGBA). A shorter
        // array is refused rather than read past. A zero or negative extent
        // is the documented release, which reads nothing.
        uint64_t channels = 1;
        if (isSub) {
            for (const auto& t : mn->customShaderTextures()) {
                if (t.name == name) { channels = static_cast<uint64_t>(std::max(t.channels, 1)); break; }
            }
        }
        if (width > 0 && height > 0 && data &&
            static_cast<uint64_t>(dataCount) <
                static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * channels) {
            ev::throwRangeError("setShaderTexture: data holds fewer than width * height * channels floats");
            return false;
        }
        if (isSub) {
            return mn->updateCustomShaderTexture(name, x, y, width, height, data);
        } else {
            return mn->setCustomShaderTexture(name, width, height, data, mipmap, repeat);
        }
    };
    if (n->type() == scene::SceneNode::Type::Mesh) return apply(static_cast<scene::MeshNode*>(n));
    if (n->type() == scene::SceneNode::Type::InstancedMesh) return apply(static_cast<scene::InstancedMeshNode*>(n));
    return false;
}

}  // extern "C"

namespace {

// An RGBA8 upload copies width * height * 4 bytes out of `data`: a shorter
// array (or a size past the host buffer cap) throws instead of being read
// past. A zero side is the clear, and reads nothing.
bool rgbaUploadFits(const char* who, int32_t width, int32_t height, const uint8_t* data, uint32_t len) {
    if (!data || width <= 0 || height <= 0) return true;
    if (!rgbaBufferFits(width, height) ||
        static_cast<uint64_t>(len) < static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u) {
        ev::throwRangeError(std::string(who) + ": data holds fewer than width * height * 4 bytes");
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

void bro_scene_SceneNode_setBaseColorTextureData(void* self, int32_t width, int32_t height,
                                                 const uint8_t* data, uint32_t len) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (!rgbaUploadFits("setBaseColorTexture", width, height, data, len)) return;
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
    if (!rgbaUploadFits("setEmissionTexture", width, height, data, len)) return;
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

// The emitter getters answer both particle node kinds: the 2D ParticleNode
// and the 3D Particles3DNode.
int32_t bro_scene_SceneNode_particleCount_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->liveCount();
    }
    if (n && n->type() == scene::SceneNode::Type::Particles) {
        return static_cast<scene::ParticleNode*>(n)->liveCount();
    }
    return 0;
}
bool bro_scene_SceneNode_particlePlaying_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->isPlaying();
    }
    if (n && n->type() == scene::SceneNode::Type::Particles) {
        return static_cast<scene::ParticleNode*>(n)->isPlaying();
    }
    return false;
}
double bro_scene_SceneNode_particleRate_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->rate();
    }
    if (n && n->type() == scene::SceneNode::Type::Particles) {
        return static_cast<scene::ParticleNode*>(n)->rate();
    }
    return 0.0;
}
void bro_scene_SceneNode_particleRate_set(void* self, double r) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->setRate(static_cast<float>(r));
    } else if (n && n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->setRate(static_cast<float>(r));
    }
}
// Particles3D emit(): a packed override so a pooled system can spawn one
// effect per call without JSON. Layout: count, position xyz, direction xyz,
// spreadDeg, speed, speedSpread, sizeScale, lifeScale, tint rgba.
void bro_scene_SceneNode_particleEmit(void* self, const double* d, uint32_t len) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Particles3D || !d || len < 16) return;
    scene::Particles3DNode::EmitOverride o;
    const int count = static_cast<int>(d[0]);
    o.position = {static_cast<float>(d[1]), static_cast<float>(d[2]), static_cast<float>(d[3])};
    o.direction = {static_cast<float>(d[4]), static_cast<float>(d[5]), static_cast<float>(d[6])};
    o.spreadDeg = static_cast<float>(d[7]);
    o.speed = static_cast<float>(d[8]);
    o.speedSpread = static_cast<float>(d[9]);
    o.sizeScale = static_cast<float>(d[10]);
    o.lifeScale = static_cast<float>(d[11]) > 0.0f ? static_cast<float>(d[11]) : 1.0f;
    float a = static_cast<float>(d[15]);
    o.tint = {static_cast<float>(d[12]), static_cast<float>(d[13]), static_cast<float>(d[14]),
              a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a)};
    if (count > 0) static_cast<scene::Particles3DNode*>(n)->emit(count, o);
}
bool bro_scene_SceneNode_particlePaused_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Particles3D) {
        return static_cast<scene::Particles3DNode*>(n)->paused();
    }
    return false;
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

} // namespace bro::bronze_host
