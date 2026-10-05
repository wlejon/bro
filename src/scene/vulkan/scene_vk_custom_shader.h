#pragma once

// Custom-shader chunks (setShader) spliced into the mesh and shadow shaders.
//
// A chunk is written against the GL-era contract: plain `uniform` / `out` /
// `in` declarations, the engine varyings vWorldPos (camera-relative), vNormal,
// vUV, vColor, vCamDist, vTangentW, vBitangentW, the vertex inputs aPos,
// aNormal, aUV, aColor, aTangent, and the wind uniforms. CustomShaderInterface
// parses both chunks once and rewrites them for Vulkan:
//   - every numeric `u_*` uniform of either chunk (several to a line,
//     precision qualifiers and initialisers allowed; an initialiser is
//     dropped, the value comes from the node) goes into ONE std140 block
//     (set 4, binding 0) declared identically in every stage, so the vertex,
//     fragment and shadow stages agree on the offsets the node's values are
//     written at;
//   - each `sampler2D` / `sampler2DArray` takes a binding 1..kMaxSamplers of
//     set 4;
//   - each custom varying (`out` in the vertex chunk, `in` in the fragment
//     chunk) gets an explicit location after the engine's;
//   - the GL names are #defines onto the Vulkan shaders' own.

#include "scene/mesh_node.h"
#include "scene/scene_renderer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene::vk {

class CustomShaderInterface {
public:
    static constexpr uint32_t kMaxSamplers = MeshNode::kMaxUserTextures;

    struct Uniform {
        std::string type;
        std::string name;
        uint32_t offset = 0;   // std140, of element 0 for an array
        bool integer = false;  // int / ivec / uint / bool: values convert from float
    };

    struct Sampler {
        std::string name;
        bool array = false;   // sampler2DArray
    };

    /// Parse both chunks. False with `err` on a declaration Vulkan cannot
    /// take (arrays of samplers, samplers other than 2D / 2D array, too many
    /// samplers, a fragment input no vertex output feeds).
    bool parse(const std::string& vertexChunk, const std::string& fragmentChunk, std::string& err);

    /// Each chunk rewritten for Vulkan (the mesh and shadow vertex stages
    /// share the vertex one), empty when the chunk is.
    std::string vertexSource() const;
    std::string fragmentSource() const;

    const std::vector<Uniform>& uniforms() const { return uniforms_; }
    const std::vector<Sampler>& samplers() const { return samplers_; }
    /// The block's size, a multiple of 16 and at least 16.
    uint32_t uboSize() const { return uboSize_; }

private:
    struct Varying {
        std::string qualifier;   // "flat" / "noperspective" / ""
        std::string type;
        std::string name;
        std::string array;       // "[N]" or ""
        uint32_t location = 0;
    };

    std::string declarations(bool vertex) const;
    bool scan(const std::string& chunk, bool vertex, std::string& body, uint32_t& offset, std::string& err);
    bool declareUniform(const std::string& type, const std::string& declarator, uint32_t& offset, std::string& err);

    std::vector<Uniform> uniforms_;
    std::vector<std::string> uniformDecls_;   // block members, in order
    std::vector<Sampler> samplers_;
    std::vector<Varying> varyings_;
    std::string vertexBody_;
    std::string fragmentBody_;
    bool hasVertex_ = false;
    bool hasFragment_ = false;
    uint32_t uboSize_ = 16;
};

class SceneVkCustomShader {
public:
    /// Validate GLSL syntax and hooks by compiling it.
    static bool validateCustomShader(SceneRenderer::CustomShaderTarget target,
                                     const std::string& vertexChunk,
                                     const std::string& fragmentChunk,
                                     std::string& errOut);

    /// Compile vertex and fragment shader stages with spliced user chunks.
    /// `indirectOutput` builds the fragment stage that also writes the
    /// indirect light (location 1) for the SSAO opaque pass.
    static bool compileCustomShaderModules(VkDevice device,
                                           SceneRenderer::CustomShaderTarget target,
                                           const CustomShaderInterface& iface,
                                           VkShaderModule& outVs,
                                           VkShaderModule& outFs,
                                           bool indirectOutput,
                                           std::string& errOut);

    /// Compile the shadow vertex stage with the spliced user vertex chunk.
    static bool compileCustomShadowShaderModule(VkDevice device,
                                                bool isSkinned,
                                                const CustomShaderInterface& iface,
                                                VkShaderModule& outVs,
                                                std::string& errOut);
};

} // namespace bro::scene::vk
