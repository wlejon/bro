#pragma once
// A set of shaders that call a world's modules (docs/scene-world-architecture.md
// §13, "worlds as code"): the built-in ones, compiled at build time with the
// engine's module files (each a module of nothing), or the same shaders with
// a world's modules in those files' places, compiled at run time. A kind's
// set says which shaders call its modules (ground/scene_cover_shaders.h, the
// grass node's; foliage/scene_plant_shaders.h, the plants node's).
//
// A world's set compiles its shaders on worker threads, each through the
// scene's SPIR-V cache (docs/code-cache.md), warmed up at boot from the
// app's last run (scene_shader_warmup.h), so a second run of a world
// compiles nothing and no frame waits on a compile.

#include "scene/vulkan/frame/scene_shader_warmup.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene::vk {

class ModuleShaders {
public:
    /// A module's source in place of an include (an engine module file).
    using Overrides = SceneVkShaderCompiler::IncludeOverrides;

    enum class State { Empty, Pending, Ready, Failed };

    /// `stages` as built.
    bool createBuiltin(VkDevice device, std::span<const BuiltinSceneShader> stages);
    /// `stages` with `overrides`' sources in their files' places, compiled
    /// off the frame thread (scene_shader_warmup.h): poll says when the
    /// modules are made. A drawer draws nothing of a set still Pending, so no
    /// frame waits on a compile.
    void start(std::span<const BuiltinSceneShader> stages, const Overrides& overrides);
    /// Pending until the compile is in; then makes the modules (Ready), or
    /// Failed with the compiler's diagnostics in `err` (said once: Failed
    /// after that leaves `err` alone).
    State poll(VkDevice device, std::string& err);
    State state() const { return state_; }
    /// start, waiting for it here. False, with the compiler's diagnostics in
    /// `err`, when one does not compile.
    bool create(VkDevice device, std::span<const BuiltinSceneShader> stages, const Overrides& overrides,
                std::string& err);
    void destroy(VkDevice device);

    VkShaderModule module(int stage) const { return modules_[static_cast<size_t>(stage)]; }
    /// Which set it is among its drawer's (0 the built-in one).
    uint32_t index = 0;
    /// The drawer's pipelines of these shaders.
    PipelineVariants pipelines;

private:
    std::vector<VkShaderModule> modules_;
    State state_ = State::Empty;
    shader_warmup::SetFuture future_;
    std::vector<std::string> stageNames_;
};

}  // namespace bro::scene::vk
