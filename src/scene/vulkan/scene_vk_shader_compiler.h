#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

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
    DecalFrag
};

/// SPIR-V shader compilation manager supporting both runtime glslc compilation
/// with caching and access to build-time compiled built-in scene shaders.
class SceneVkShaderCompiler {
public:
    SceneVkShaderCompiler() = default;
    ~SceneVkShaderCompiler() = default;

    /// Compile a GLSL source string into SPIR-V words using glslc.
    /// Returns an empty vector if compilation fails.
    static std::vector<uint32_t> compileGlsl(
        const std::string& glslSource,
        VkShaderStageFlagBits stage,
        const std::string& entryPoint = "main",
        const std::vector<std::string>& defines = {});

    /// Retrieve the precompiled SPIR-V bytecode for a built-in shader.
    static const std::vector<uint32_t>& getBuiltinSpirv(BuiltinSceneShader shader);

    /// Convenience: Create a VkShaderModule for a built-in scene shader.
    static VkShaderModule createBuiltinModule(VkDevice device, BuiltinSceneShader shader);

    /// Create a VkShaderModule from SPIR-V bytecode words.
    static VkShaderModule createModule(VkDevice device, const uint32_t* code, size_t sizeBytes);
    static VkShaderModule createModule(VkDevice device, const std::vector<uint32_t>& spirv);

    /// Destroy a previously created VkShaderModule.
    static void destroyModule(VkDevice device, VkShaderModule module);

    /// Check if glslc is available on the system.
    static bool hasGlslc();

private:
    static std::unordered_map<std::string, std::vector<uint32_t>> s_compileCache;
};

} // namespace bro::scene::vk
