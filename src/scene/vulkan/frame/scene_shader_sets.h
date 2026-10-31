#pragma once

// A second set of the scene's shaders, for A/B measurement in one process
// (docs/headless.md "Shader A/B"). Set A is what the scene runs (the
// embedded brokernel sources, or BRO_SCENE_SHADER_SOURCE's); set B is the
// same shaders compiled from another source tree (the layout
// BRO_SCENE_SHADER_SOURCE takes). Both stay compiled: every scene pipeline
// whose SPIR-V differs under B gets a twin built from B's, and the scene
// binds the twin while B is the active set, so switching costs a flag and no
// compile.
//
// It works from how each SPIR-V blob was made (its recipe), noted as the
// compiler makes it, and from what each pipeline was built of (the builder's
// state, its stages' recipes), noted as it is built; so the pipelines made
// before B was loaded get their twins too. For that every scene pipeline is
// bound with bindScenePipeline and destroyed with destroyScenePipeline, in
// place of the plain Vulkan calls.

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::scene::vk {

class SceneVkPipelineBuilder;

/// How a SPIR-V blob was made, so it can be made again from another tree.
struct ShaderRecipe {
    std::string name;
    std::function<std::vector<uint32_t>(std::string* err)> compile; // empty: nothing to remake
    std::vector<uint32_t> words;      // set A's SPIR-V
    // Set B's, made at the set's load (or at first use after it).
    uint64_t bGeneration = 0;
    std::vector<uint32_t> bWords;
    bool bDiffers = false;
    std::string bError;               // B did not compile: the twin keeps A's
};

namespace shader_sets {

/// The compiler notes each blob it hands out with its recipe.
void noteSpirv(std::shared_ptr<ShaderRecipe> recipe);
/// True while set B's own compiles run (their blobs are not noted).
bool compilingSetB();
/// The tree set B's compiles read their sources from ("" otherwise).
const std::string& setBSourceDir();
/// Optional define names passed to loadShaderSetB.
const std::vector<std::string>& setBDefines();

/// SceneVkShaderModule notes each module it makes and forgets it at destroy.
void noteModule(VkShaderModule module, const uint32_t* code, size_t sizeBytes);
void forgetModule(VkShaderModule module);

/// The pipeline builders note each pipeline they make.
void noteGraphicsPipeline(VkDevice device, VkPipelineLayout layout, const SceneVkPipelineBuilder& builder,
                          VkPipeline pipeline);
void noteComputePipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule module, const char* name,
                         const char* entry, VkPipeline pipeline);

} // namespace shader_sets

/// What loading set B found.
struct ShaderSetInfo {
    std::string dir;
    int shaders = 0;          // distinct shaders live pipelines are built of
    int differing = 0;        // of those, whose SPIR-V differs under B
    int pipelines = 0;        // live scene pipelines
    int twinned = 0;          // of those, given a twin of B's
    std::vector<std::string> failed;  // shaders that did not compile from B (they keep A's)
};

/// Load set B from the shader tree at `dir` (it must hold common/scene.bk),
/// replacing a set loaded before. False with the reason in *error when the
/// tree is not one or a define name is not an identifier.
bool loadShaderSetB(const std::string& dir, ShaderSetInfo* info, std::string* error,
                    const std::vector<std::string>& defines = {});
/// The set the scene binds from now on: 0 (A) or 1 (B; only once loaded).
bool useShaderSet(int set);
int activeShaderSet();
/// Counts as now (live pipelines, twins) for a set loaded before.
ShaderSetInfo shaderSetInfo();

/// vkCmdBindPipeline of a scene pipeline: its twin while set B is active.
void bindScenePipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline);
/// vkDestroyPipeline of a scene pipeline, and of its twin.
void destroyScenePipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator);

} // namespace bro::scene::vk
