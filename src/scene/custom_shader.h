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
    std::string key;
    std::vector<CustomShaderUniform> uniforms;
    bool hasVertex = false;   // module defines fn vertex

    bool isModule() const { return !module.empty(); }

    static std::unique_ptr<CustomShaderState> make(
        std::string mod, std::string file,
        std::vector<std::pair<std::string, std::string>> extra = {}) {
        auto s = std::make_unique<CustomShaderState>();
        s->module = std::move(mod);
        s->moduleFile = file.empty() ? "<module>" : std::move(file);
        s->modules = std::move(extra);
        static const std::regex kVertexRe(R"(\bfn\s+vertex\s*\()");
        s->hasVertex = std::regex_search(s->module, kVertexRe);
        size_t h = std::hash<std::string>{}(s->module);
        for (const auto& m : s->modules) {
            h ^= std::hash<std::string>{}(m.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::string>{}(m.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        s->key = "bk_" + std::to_string(h);
        return s;
    }

    static std::unique_ptr<CustomShaderState> makeModule(
        std::string mod, std::string file,
        std::vector<std::pair<std::string, std::string>> extra = {}) {
        return make(std::move(mod), std::move(file), std::move(extra));
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

} // namespace bro::scene
