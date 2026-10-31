#pragma once

// A brokernel module as a scene shader hook (setShaderModule, createVolume,
// createMesh, createInstancedMesh, createSkinnedMesh): the node's module,
// registered as `hook` beside any modules it brings (the app's, which its
// kernels share), composed with the engine's own (scene.bk, atmosphere.bk,
// lighting.bk, custom.bk and the stage's) and compiled by brass to the
// stage's SPIR-V. The hook reads the engine's frame state through those
// modules' resources and declares its own in set 4 (custom.bk), which
// CustomShaderInterface::fromKernel reflects.

#include "scene/vulkan/draw/scene_vk_custom_shader.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene::vk {

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

/// Compile a built-in mesh stage entry point from draw/mesh_main.bk.
bool compileKernelMeshStage(const char* entry, KernelHookStage& out, std::string& err);

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
bool compileKernelShadowHook(SceneVkCustomShader::ShadowCaster caster, bool cutout, const std::string& module,
                             const std::string& file, const KernelHookModules& modules,
                             KernelHookStage& out, std::string& err);

} // namespace bro::scene::vk
