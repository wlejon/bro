#pragma once

// Per-node custom shader state: the brokernel module source, imported helper
// modules, and numeric uniform overrides. Graphics-API-independent; the
// backend compiles and caches the module by `key`.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <regex>
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

    bool isModule() const { return !module.empty(); }

    void rebuildKey() {
        static const std::regex kVertexRe(R"(\bfn\s+vertex\s*\()");
        hasVertex = std::regex_search(module, kVertexRe);
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
        for (const auto& [name, val] : k) {
            bool found = false;
            for (auto& existing : knobs) {
                if (existing.first == name) {
                    existing.second = val;
                    found = true;
                    break;
                }
            }
            if (!found) knobs.emplace_back(name, val);
        }
        rebuildKey();
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

    void setUniform(const std::string& name, int comps, const float* vals) {
        for (auto& u : uniforms) {
            if (u.name == name) {
                u.comps = comps;
                for (int i = 0; i < 4; ++i) u.v[i] = i < comps ? vals[i] : 0.0f;
                return;
            }
        }
        CustomShaderUniform u;
        u.name = name;
        u.comps = comps;
        for (int i = 0; i < 4; ++i) u.v[i] = i < comps ? vals[i] : 0.0f;
        uniforms.push_back(std::move(u));
    }
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

struct ShaderStageStats {
    bool present = false;
    uint32_t spirvBytes = 0;
    uint32_t instructions = 0;
    uint32_t arithmetic = 0;
    uint32_t samples = 0;
    uint32_t loads = 0;
    uint32_t stores = 0;
    uint32_t branches = 0;
    uint32_t loops = 0;
    uint32_t functions = 0;
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
