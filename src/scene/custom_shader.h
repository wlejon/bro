#pragma once

// Per-node custom shader state: the brokernel module source, imported helper
// modules, and numeric uniform overrides. Graphics-API-independent; the
// backend compiles and caches the module by `key`.

#include "scene/kernel_source_scan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::render {
class ComputeBuffer;
}

namespace bro::scene {

struct CustomShaderUniform {
    std::string name;
    int comps = 1; // 1..4
    std::array<float, 4> v = {0.0f, 0.0f, 0.0f, 0.0f};
};

/// A GPU buffer (a KernelBuffer) bound by name to a custom shader's
/// `@binding(4, 14..23) var name: read_buffer<T>;` declaration.
struct CustomShaderBuffer {
    std::string name;
    std::shared_ptr<render::ComputeBuffer> buffer;
};

inline void bindCustomShaderBuffer(std::vector<CustomShaderBuffer>& list,
                                   const std::string& name,
                                   std::shared_ptr<render::ComputeBuffer> buffer) {
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->name == name) {
            if (!buffer) list.erase(it);
            else it->buffer = std::move(buffer);
            return;
        }
    }
    if (buffer) list.push_back({name, std::move(buffer)});
}

struct CustomShaderState {
    std::string module;       // a brokernel hook module
    std::string moduleFile;   // its filename in compile errors
    /// Modules the hook imports beside the engine's (name, source).
    std::vector<std::pair<std::string, std::string>> modules;
    /// `@knob const` overrides (name, value).
    std::vector<std::pair<std::string, double>> knobs;
    uint64_t reloadGen = 0;
    std::string key;
    std::vector<CustomShaderUniform> uniforms;
    bool hasVertex = false;   // module defines fn vertex
    bool hasVertexPos = false;   // fn vertex may modify vertex position

    bool isModule() const { return !module.empty(); }

    void rebuildKey() {
        vertexReads_.clear();
        const KernelVertexScan scan = scanKernelVertex(module);
        hasVertex = scan.hasVertex;
        hasVertexPos = scan.hasVertexPos;
        size_t h = std::hash<std::string>{}(module);
        for (const auto& m : modules) {
            h ^= std::hash<std::string>{}(m.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::string>{}(m.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        for (const auto& k : knobs) {
            h ^= std::hash<std::string>{}(k.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<double>{}(k.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        if (reloadGen != 0) {
            h ^= std::hash<uint64_t>{}(reloadGen) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        key = "bk_" + std::to_string(h);
    }

    void setKnobs(std::vector<std::pair<std::string, double>> k) {
        bool changed = false;
        for (const auto& [name, val] : k) {
            for (auto& u : uniforms) {
                if (u.name == name && u.comps == 1) u.v[0] = static_cast<float>(val);
            }
            bool found = false;
            for (auto& existing : knobs) {
                if (existing.first == name) {
                    if (existing.second != val) {
                        existing.second = val;
                        changed = true;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                knobs.emplace_back(name, val);
                changed = true;
            }
        }
        if (changed) rebuildKey();
    }

    void bumpReloadGen() {
        ++reloadGen;
        rebuildKey();
    }

    static std::unique_ptr<CustomShaderState> make(
        std::string mod, std::string file,
        std::vector<std::pair<std::string, std::string>> extra = {},
        std::vector<std::pair<std::string, double>> knobOverrides = {}) {
        auto s = std::make_unique<CustomShaderState>();
        s->module = std::move(mod);
        s->moduleFile = file.empty() ? "<module>" : std::move(file);
        s->modules = std::move(extra);
        s->knobs = std::move(knobOverrides);
        s->rebuildKey();
        return s;
    }

    static std::unique_ptr<CustomShaderState> makeModule(
        std::string mod, std::string file,
        std::vector<std::pair<std::string, std::string>> extra = {},
        std::vector<std::pair<std::string, double>> knobOverrides = {}) {
        return make(std::move(mod), std::move(file), std::move(extra), std::move(knobOverrides));
    }

    /// Set a uniform's value; whether it changed.
    bool setUniform(const std::string& name, int comps, const float* vals) {
        float v[4];
        for (int i = 0; i < 4; ++i) v[i] = i < comps ? vals[i] : 0.0f;
        for (auto& u : uniforms) {
            if (u.name == name) {
                bool same = u.comps == comps;
                for (int i = 0; i < 4; ++i) same = same && u.v[i] == v[i];
                u.comps = comps;
                for (int i = 0; i < 4; ++i) u.v[i] = v[i];
                return !same;
            }
        }
        CustomShaderUniform u;
        u.name = name;
        u.comps = comps;
        for (int i = 0; i < 4; ++i) u.v[i] = v[i];
        uniforms.push_back(std::move(u));
        return true;
    }

    /// Whether fn vertex (what the shadow pass runs of the hook) reads the
    /// uniform `name`: one it does not read changes no shadow silhouette.
    bool vertexReads(const std::string& name) {
        if (!hasVertex) return false;
        for (const auto& [n, reads] : vertexReads_) {
            if (n == name) return reads;
        }
        // The hook's own modules may be handed the uniforms: any of their
        // reads of the name counts.
        bool reads = kernelFnReadsField(module, "vertex", name);
        for (const auto& m : modules) {
            reads = reads || m.second.find("." + name) != std::string::npos;
        }
        vertexReads_.emplace_back(name, reads);
        return reads;
    }

private:
    std::vector<std::pair<std::string, bool>> vertexReads_;   // vertexReads(), by uniform
};

struct ShaderStageExecStat {
    std::string name;
    std::string description;
    bool isFloat = false;
    bool isBool = false;
    double fval = 0.0;
    int64_t ival = 0;
    uint64_t uval = 0;
};

// A source function's instructions in a stage: its own, and with the
// functions it called (the stage's SPIR-V is one inlined function).
struct ShaderFunctionCost {
    std::string name;
    uint32_t self = 0;
    uint32_t total = 0;
};

struct ShaderStageStats {
    bool present = false;
    std::vector<ShaderFunctionCost> functionCosts;   // largest total first
    uint32_t spirvBytes = 0;
    uint32_t instructions = 0;
    uint32_t arithmetic = 0;
    uint32_t samples = 0;
    uint32_t loads = 0;
    uint32_t stores = 0;
    uint32_t branches = 0;
    uint32_t loops = 0;
    uint32_t sgprs = 0;
    uint32_t vgprs = 0;
    uint32_t scratchBytes = 0;
    std::vector<ShaderStageExecStat> executables;
};

struct NodeShaderStats {
    ShaderStageStats fragment;
    ShaderStageStats vertex;
    ShaderStageStats nearStage;
};

struct SceneProbeValue {
    std::string name;
    int components = 1;
    std::array<float, 4> values = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct SceneNodeProbeResult {
    uint32_t nodeId = 0;
    std::string nodeName;
    std::vector<SceneProbeValue> values;
};

} // namespace bro::scene
