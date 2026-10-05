#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene::vk {

/// Built-in shader identifier enum for standard scene passes.
enum class BuiltinSceneShader {
    MeshVert,
    MeshInstancedVert,
    MeshSkinnedVert,
    MeshFrag,
    ShadowVert,
    ShadowInstancedVert,
    ShadowSkinnedVert,
    ShadowFrag,
    SkyboxVert,
    EnvironmentFrag,
    PostFxVert,
    TonemapFrag,
    BloomFrag,
    FxaaFrag,
    BillboardVert,
    BillboardFrag,
    ParticlesVert,
    ParticlesFrag,
    DecalVert,
    DecalFrag,
    BlurFrag,
    ColorLutFrag,
    SsaoFrag,
    SsrFrag,
    DofFrag,
    ApplyAoFrag,
    GaussianSplatVert,
    GaussianSplatFrag
};

/// The scene's shader modules: the built-in shaders (compiled at build time)
/// and run-time GLSL (clipmap terrain, custom shaders) through the engine's
/// one in-process compiler, render/glsl_compiler.h.
class SceneVkShaderCompiler {
public:
    SceneVkShaderCompiler() = default;
    ~SceneVkShaderCompiler() = default;

    /// Compile a Vulkan GLSL source string into SPIR-V words. Returns an
    /// empty vector (and the compiler's diagnostics in *errOut) on failure.
    static std::vector<uint32_t> compileGlsl(const std::string& glslSource,
                                             VkShaderStageFlagBits stage,
                                             std::string* errOut = nullptr);

    /// Retrieve the precompiled SPIR-V bytecode for a built-in shader.
    static const std::vector<uint32_t>& getBuiltinSpirv(BuiltinSceneShader shader);

    /// Convenience: Create a VkShaderModule for a built-in scene shader.
    static VkShaderModule createBuiltinModule(VkDevice device, BuiltinSceneShader shader);

    /// Create a VkShaderModule from SPIR-V bytecode words.
    static VkShaderModule createModule(VkDevice device, const uint32_t* code, size_t sizeBytes);
    static VkShaderModule createModule(VkDevice device, const std::vector<uint32_t>& spirv);

    /// Destroy a previously created VkShaderModule.
    static void destroyModule(VkDevice device, VkShaderModule module);
};

} // namespace bro::scene::vk
