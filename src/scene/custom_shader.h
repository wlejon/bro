#pragma once

#include <memory>
#include <string>
#include <vector>

namespace bro::scene {

// Custom-shader state shared by MeshNode and InstancedMeshNode: user GLSL
// chunks spliced into the mesh uber-shaders (see the //__USER_CHUNK__
// markers in mesh.vert / mesh.frag / mesh_instanced.vert / shadow.vert).
// A node stores only sources + numeric uniform values — the compiled
// program variants live in SceneRenderer's cache, keyed by `key` plus a
// variant tag, so identical sources across nodes share one program per
// variant. Compilation/validation happens in the renderer
// (SceneGraph::compileCustomShader) BEFORE this state is set, so a node
// with custom-shader state always maps to a linked program.

struct CustomShaderUniform {
    std::string name;      // must carry the `u_` user-namespace prefix
    int comps = 1;         // 1..4 → float / vec2 / vec3 / vec4
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct CustomShaderState {
    std::string vertexChunk;
    std::string fragmentChunk;
    std::string key;       // program-cache key: vertex + '\x1f' + fragment
    std::vector<CustomShaderUniform> uniforms;

    static std::unique_ptr<CustomShaderState> make(std::string vertexChunk,
                                                   std::string fragmentChunk) {
        auto st = std::make_unique<CustomShaderState>();
        st->key = vertexChunk + '\x1f' + fragmentChunk;
        st->vertexChunk = std::move(vertexChunk);
        st->fragmentChunk = std::move(fragmentChunk);
        return st;
    }

    /// Set (or update) a numeric user-uniform value. Values are plain floats
    /// — nothing JS-owned — and are uploaded per draw, so two nodes sharing
    /// a program can carry different values.
    void setUniform(const std::string& name, int comps, const float* vals) {
        if (comps < 1) comps = 1;
        if (comps > 4) comps = 4;
        for (auto& u : uniforms) {
            if (u.name == name) {
                u.comps = comps;
                for (int i = 0; i < comps; ++i) u.v[i] = vals[i];
                return;
            }
        }
        CustomShaderUniform u;
        u.name = name;
        u.comps = comps;
        for (int i = 0; i < comps; ++i) u.v[i] = vals[i];
        uniforms.push_back(std::move(u));
    }
};

} // namespace bro::scene
