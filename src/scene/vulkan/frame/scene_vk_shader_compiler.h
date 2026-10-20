#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene::vk {

/// The built-in scene shaders, one per line of
/// scene/vulkan/shaders/builtin_shaders.def (the source file, stage and
/// DEFINES of each are there).
enum class BuiltinSceneShader {
#define BRO_SCENE_SHADER(name, out, source, stage, defines) name,
#include "scene/vulkan/shaders/builtin_shaders.def"
#undef BRO_SCENE_SHADER
    Count
};

/// The scene's shader modules: the built-in shaders (compiled at build time)
/// and run-time GLSL (clipmap terrain, custom shaders) through the engine's
/// one in-process compiler, render/glsl_compiler.h.
///
/// BRO_SCENE_SHADER_SOURCE is the developer's switch for editing shaders
/// without a rebuild (docs/hot-reload.md): `1` compiles the built-in shaders,
/// and reads the GLSL the run-time shaders include or are built from, from
/// the source tree this binary was built from; any other non-empty value is
/// a directory to read instead. Each built-in shader compiles at its first
/// use, from the same text with the same DEFINES as the build-time compile,
/// and falls back to the embedded SPIR-V (with the error logged) when it
/// does not compile. Unset, `0` or empty: the embedded SPIR-V.
class SceneVkShaderCompiler {
public:
    SceneVkShaderCompiler() = default;
    ~SceneVkShaderCompiler() = default;

    /// Compile a Vulkan GLSL source string into SPIR-V words. `#include
    /// "name"` lines pull in the shared scene GLSL by its path under
    /// vulkan/shaders (common/scene_camera.glsl, common/scene_lighting.glsl
    /// and its common/scene_atmosphere.glsl, common/scene_mesh_push.glsl,
    /// common/scene_shadow_push.glsl, ...).
    /// Returns an empty vector (and the diagnostics in *errOut) on failure.
    static std::vector<uint32_t> compileGlsl(const std::string& glslSource,
                                             VkShaderStageFlagBits stage,
                                             std::string* errOut = nullptr);

    /// The SPIR-V of a built-in shader: the embedded words, or under
    /// BRO_SCENE_SHADER_SOURCE the words compiled from source.
    static const std::vector<uint32_t>& getBuiltinSpirv(BuiltinSceneShader shader);

    /// The SPIR-V embedded at build time, whatever the switch says.
    static const std::vector<uint32_t>& embeddedSpirv(BuiltinSceneShader shader);

    /// Compile a built-in shader from the shader tree at `dir` (its includes
    /// paths from there) exactly as the build does (bro_spirv_embed). Empty, with the reason in *errOut, on
    /// failure.
    static std::vector<uint32_t> compileBuiltinFromSource(BuiltinSceneShader shader,
                                                          const std::string& dir,
                                                          std::string* errOut = nullptr);

    /// The built-in shader's enum name ("MeshVegFrag"), for logs and tests.
    static const char* builtinName(BuiltinSceneShader shader);

    /// The directory BRO_SCENE_SHADER_SOURCE names ("" when it is off), with
    /// a trailing slash.
    static const std::string& shaderSourceDir();
    /// The source tree's shader directory this binary was built from.
    static const char* buildShaderSourceDir();

    /// A scene GLSL file by its path under vulkan/shaders: read from
    /// shaderSourceDir() when the switch is on and the file is there, else
    /// `embedded`.
    static std::string shaderSource(const char* fileName, const char* embedded);

    /// Convenience: Create a VkShaderModule for a built-in scene shader.
    static VkShaderModule createBuiltinModule(VkDevice device, BuiltinSceneShader shader);

    /// Create a VkShaderModule from SPIR-V bytecode words.
    static VkShaderModule createModule(VkDevice device, const uint32_t* code, size_t sizeBytes);
    static VkShaderModule createModule(VkDevice device, const std::vector<uint32_t>& spirv);

    /// Destroy a previously created VkShaderModule.
    static void destroyModule(VkDevice device, VkShaderModule module);
};

} // namespace bro::scene::vk
