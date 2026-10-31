#pragma once

// A brokernel module as a scene shader hook (createVolume({ module })): the
// node's module, registered as `hook` beside any modules it brings (the
// app's, which its kernels share), composed with the engine's own
// (scene.bk, atmosphere.bk, lighting.bk, custom.bk and the stage's) and
// compiled by brass to the stage's SPIR-V. The hook reads the engine's frame
// state through those modules' resources and declares its own in set 4
// (custom.bk), which CustomShaderInterface::fromKernel reflects.
//
// A volume's hook gives
//   pub fn volume(ray: volume::Ray) -> volume::Light
// and, for a near medium the grid integrates (PassVolumeNear),
//   pub fn medium_range() -> f32
//   pub fn medium(pos: vec3<f32>, dir: vec3<f32>, footprint: f32) -> volume::Medium
// (volume.bk). The engine's sources come from the source tree under
// BRO_SCENE_SHADER_SOURCE, as the GLSL ones do.

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

/// Modules the hook imports beside the engine's (the app's own, shared with
/// its kernels): name and source.
using KernelHookModules = std::vector<std::pair<std::string, std::string>>;

/// Compile a volume's hook module (`file` names it in diagnostics). False
/// with `err` on a compile error, a set-4 resource the set cannot hold, or
/// one of `modules` named as an engine module.
bool compileKernelVolumeHook(const std::string& module, const std::string& file, const KernelHookModules& modules,
                             KernelVolumeHook& out, std::string& err);

} // namespace bro::scene::vk
