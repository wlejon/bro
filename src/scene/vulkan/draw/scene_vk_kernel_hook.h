#pragma once

// A brokernel module as a scene shader hook (setShaderModule, createVolume,
// createMesh, createInstancedMesh, createSkinnedMesh) and the engine's own
// built-in scene stages: compiled in-process by brass to SPIR-V.
//
// Engine modules read the frame's camera, lighting and atmosphere at the
// engine's sets/bindings (common/scene.bk, atmosphere.bk, lighting.bk,
// custom.bk, vegetation.bk, procedural.bk) and a node's hook declares its
// own resources in set 4 (custom.bk), which CustomShaderInterface::fromKernel
// reflects.

#include "scene/vulkan/draw/scene_vk_custom_shader.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene::vk {

enum class ShadowCaster { Static, Skinned, Instanced };

struct KernelHookStage {
    std::vector<uint32_t> spirv;
    std::string entry;   // its OpEntryPoint
};

struct KernelVolumeHook {
    KernelHookStage main;       // volume_main.bk
    bool medium = false;        // the hook gives medium (and medium_range)
    KernelHookStage nearPass;   // volume_near.bk, when it does
    CustomShaderInterface iface;
};

struct KernelMeshHook {
    bool hasVertex = false;
    bool hasFragment = false;
    bool hasLight = false;
    KernelHookStage vs;         // empty when !hasVertex (uses built-in passthrough vs)
    KernelHookStage fs;         // empty when !hasFragment (uses built-in mesh_frag)
    CustomShaderInterface iface;
};

/// Modules the hook imports beside the engine's (the app's own, shared with
/// its kernels): name and source.
using KernelHookModules = std::vector<std::pair<std::string, std::string>>;

/// When BRO_SCENE_SHADER_SOURCE is set, the directory (with trailing '/')
/// shaders are read from instead of their embedded strings; "" otherwise.
const std::string& shaderSourceDir();

/// The shader directory in the source tree this binary was built from.
const char* buildShaderSourceDir();

/// `fileName`'s text from shaderSourceDir() (or setBSourceDir() while
/// compiling shader set B) when set, `embedded` otherwise.
std::string shaderSource(const char* fileName, const char* embedded);

/// Compile a built-in mesh stage entry point from draw/mesh_main.bk.
bool compileKernelMeshStage(const char* entry, KernelHookStage& out, std::string& err);

/// Compile a built-in shadow caster stage entry point from shadow/shadow.bk.
bool compileKernelShadowStage(const char* entry, KernelHookStage& out, std::string& err);

/// Compile a built-in stage entry point from `file` (with common engine modules registered).
bool compileKernelStage(const char* file, const char* embedded, const char* entry, KernelHookStage& out,
                        std::string& err);

/// Compile a built-in stage entry point from `file` and create its VkShaderModule.
VkShaderModule createKernelStageModule(VkDevice dev, const char* file, const char* embedded, const char* entry);

/// Compile a volume's hook module (`file` names it in diagnostics).
bool compileKernelVolumeHook(const std::string& module, const std::string& file, const KernelHookModules& modules,
                             KernelVolumeHook& out, std::string& err);

/// Compile a mesh's hook module for `target` (`file` names it in diagnostics).
bool compileKernelMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module,
                           const std::string& file, const KernelHookModules& modules,
                           KernelMeshHook& out, std::string& err);

/// Compile a mesh hook's fragment variant (cuts / indirect output).
bool compileKernelMeshHookFragment(const std::string& module, const std::string& file,
                                   const KernelHookModules& modules, bool hasLight, bool indirect, bool cuts,
                                   KernelHookStage& out, std::string& err);

/// Compile a mesh hook's shadow-caster vertex stage.
bool compileKernelShadowHook(ShadowCaster caster, bool cutout, const std::string& module,
                             const std::string& file, const KernelHookModules& modules,
                             KernelHookStage& out, std::string& err);

} // namespace bro::scene::vk
