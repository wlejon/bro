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
//   - each `sampler2D` / `sampler2DArray` / `sampler3D` takes a binding 1..kMaxSamplers of
//     set 4, a bare image its name #defines into a sampler of the set's
//     own (the texture's repeat and clampT pick it: the block's hidden
//     customSamplerModes);
//   - each `buffer <type> u_name[];` (a GPU buffer the node binds by name,
//     setShaderBuffer: a KernelBuffer) becomes a GL_EXT_buffer_reference
//     block whose address and element count (`u_name_length`) are members of
//     the uniform block, `u_name` #defined onto its array; an unbound one
//     reads a zeroed buffer with length 0;
//   - each custom varying (`out` in the vertex chunk, `in` in the fragment
//     chunk) gets an explicit location after the engine's;
//   - the GL names are #defines onto the Vulkan shaders' own.

#include "scene/nodes/mesh_node.h"
#include "scene/scene_renderer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace brokernel {
struct CompileResult;
}

namespace bro::scene::vk {

class CustomShaderInterface {
public:
    static constexpr uint32_t kMaxSamplers = MeshNode::kMaxUserTextures;
    /// The set's samplers, an immutable array at the binding after the
    /// images: a handful however many images the set holds, so the
    /// material's, the custom set's and the lighting's samplers stay within
    /// the 16 a stage some devices take (MoltenVK). A texture's is two bits
    /// of the block's customSamplerModes, one of these.
    static constexpr uint32_t kSamplersBinding = kMaxSamplers + 1;
    enum SamplerMode : uint32_t {
        kLinear = 0,    // linear, mips, clamped
        kRepeat = 1,    // linear, mips, repeating
        kRepeatS = 2,   // linear, mips, repeating across (s), clamped in t
        kNearest = 3,   // nearest, clamped (integer images)
        kSamplerModes = 4,
    };
    /// A brokernel hook's (custom.bk): the same samplers at bindings of
    /// their own, which the hook names (custom::linear ...), and its storage
    /// buffers (setShaderBuffer) after them.
    static constexpr uint32_t kKernelSamplersBinding = kSamplersBinding + 1;
    static constexpr uint32_t kBuffersBinding = kKernelSamplersBinding + kSamplerModes;
    static constexpr uint32_t kMaxBuffers = 6;

    struct Uniform {
        std::string type;
        std::string name;
        uint32_t offset = 0;   // std140, of element 0 for an array
        bool integer = false;  // int / ivec / uint / bool: values convert from float
    };

    struct Sampler {
        std::string name;
        bool array = false;   // sampler2DArray
        bool volume = false;  // sampler3D
    };

    /// A `buffer <type> u_name[];` declaration: the block holds its address
    /// at `addressOffset` and its element count at `lengthOffset`.
    struct Buffer {
        std::string type;
        std::string name;
        uint32_t stride = 0;          // std430 array stride of `type`
        uint32_t addressOffset = 0;
        uint32_t lengthOffset = 0;
    };

    /// Parse both chunks. False with `err` on a declaration Vulkan cannot
    /// take (arrays of samplers, samplers other than 2D / 2D array, too many
    /// samplers, a fragment input no vertex output feeds).
    bool parse(const std::string& vertexChunk, const std::string& fragmentChunk, std::string& err);

    /// A brokernel hook's set 4 from its module's resources (custom.bk):
    /// the uniform at binding 0 (its fields the uniforms, by name), the
    /// textures at 1 to kMaxSamplers (samplers(), indexed by binding - 1,
    /// unnamed where there is none) and the storage buffers from
    /// kBuffersBinding. False with `err` on a resource of set 4 the set does
    /// not hold.
    /// Every stage of the hook gives its own; one set serves them all.
    bool fromKernel(std::initializer_list<const brokernel::CompileResult*> stages, std::string& err);

    /// A storage buffer of a brokernel hook (setShaderBuffer by name).
    struct KernelBuffer {
        std::string name;
        uint32_t binding = 0;
    };
    const std::vector<KernelBuffer>& kernelBuffers() const { return kernelBuffers_; }
    /// Whether the block holds the samplers' modes (a GLSL chunk's): a
    /// brokernel hook names its samplers itself.
    bool hasSamplerModes() const { return !kernel_ && !samplers_.empty(); }

    /// Each chunk rewritten for Vulkan (the mesh and shadow vertex stages
    /// share the vertex one), empty when the chunk is.
    std::string vertexSource() const;
    std::string fragmentSource() const;

    const std::vector<Uniform>& uniforms() const { return uniforms_; }
    const std::vector<Sampler>& samplers() const { return samplers_; }
    const std::vector<Buffer>& buffers() const { return buffers_; }
    /// Each chunk-built stage needs GL_EXT_buffer_reference
    /// (withExtensions).
    std::string withExtensions(std::string stageSource) const;
    /// The block's size, a multiple of 16 and at least 16.
    uint32_t uboSize() const { return uboSize_; }
    /// Where the block keeps each sampler's mode (SamplerMode, two bits a
    /// sampler in declaration order), when there are samplers.
    uint32_t samplerModesOffset() const { return samplerModesOffset_; }

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
    bool declareBuffer(const std::string& type, const std::string& name, uint32_t& offset, std::string& err);

    std::vector<Uniform> uniforms_;
    std::vector<std::string> uniformDecls_;   // block members, in order
    std::vector<Sampler> samplers_;
    std::vector<Buffer> buffers_;
    std::vector<KernelBuffer> kernelBuffers_;
    bool kernel_ = false;
    std::vector<Varying> varyings_;
    std::string vertexBody_;
    std::string fragmentBody_;
    bool hasVertex_ = false;
    bool hasFragment_ = false;
    uint32_t uboSize_ = 16;
    uint32_t samplerModesOffset_ = 0;
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

    /// A volume's fragment stage: the fragment chunk (its userVolume)
    /// spliced into volume.frag. The vertex stage is the built-in
    /// VolumeVert. `nearPass`: the stage that integrates a chunk's
    /// userMedium over the near grid (volume.frag's VOLUME_NEAR_PASS).
    static bool compileVolumeModule(VkDevice device, const CustomShaderInterface& iface, bool nearPass,
                                    VkShaderModule& outFs, std::string& errOut);

    /// Whether a volume's fragment chunk gives its near medium point by
    /// point (userMedium and userMediumRange, volume.frag).
    static bool volumeHasMedium(const std::string& fragmentChunk);

    /// The shadow vertex stage a chunk is spliced into: shadow.vert,
    /// shadow_skinned.vert or shadow_instanced.vert.
    enum class ShadowCaster { Static, Skinned, Instanced };
    /// Compile the shadow vertex stage with the spliced user vertex chunk
    /// (`cutout`: its SHADOW_CUTOUT build, scene_shadow_cutout.glsl).
    static bool compileCustomShadowShaderModule(VkDevice device,
                                                ShadowCaster caster,
                                                bool cutout,
                                                const CustomShaderInterface& iface,
                                                VkShaderModule& outVs,
                                                std::string& errOut);
};

} // namespace bro::scene::vk
