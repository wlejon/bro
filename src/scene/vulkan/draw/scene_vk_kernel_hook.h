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
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene::vk {

enum class ShadowCaster { Static, Skinned, Instanced };

// A source function's share of a stage (brokernel's function_costs: its own
// instructions, and with those of the functions it called).
struct StageFunctionCost {
    std::string name;
    uint32_t self = 0;
    uint32_t total = 0;
};

struct KernelHookStage {
    std::vector<uint32_t> spirv;
    std::string entry;   // its OpEntryPoint
    std::vector<StageFunctionCost> costs;   // largest total first
};

struct KernelProbeSlot {
    std::string name;
    uint32_t index = 0;
    uint32_t components = 1;
};

struct KernelVolumeHook {
    KernelHookStage main;       // volume_main.bk
    bool medium = false;        // the hook gives medium (and medium_range)
    KernelHookStage nearPass;   // volume_near.bk, when it does
    CustomShaderInterface iface;
    std::vector<KernelProbeSlot> probes;
};

struct KernelMeshHook {
    bool hasVertex = false;
    bool hasFragment = false;
    bool hasLight = false;
    KernelHookStage vs;         // empty when !hasVertex (uses built-in passthrough vs)
    KernelHookStage fs;         // empty when !hasFragment (uses built-in mesh_frag)
    CustomShaderInterface iface;
    std::vector<KernelProbeSlot> probes;
};

/// Modules the hook imports beside the engine's (the app's own, shared with
/// its kernels): name and source.
using KernelHookModules = std::vector<std::pair<std::string, std::string>>;
using KernelHookKnobs = std::vector<std::pair<std::string, double>>;

uint64_t kernelHookReloadGeneration();
void bumpKernelHookReloadGeneration();

bool expandMeshHookVaryings(const std::string& hook, const std::string& file,
                            std::string& outHook, std::string& err);

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

/// Start compiling these built-in mesh / shadow stages, each on a thread of
/// its own; the compileKernel*Stage of one takes its result (waiting for it
/// if it is still compiling). Nothing while the shaders are read from the
/// source tree or set B compiles.
void prewarmKernelMeshStages(std::span<const char* const> entries);
void prewarmKernelShadowStages(std::span<const char* const> entries);
/// Wait for every in-flight stage compile started by prewarm or prepare.
void waitForKernelStageCompiles();

/// Compile a built-in stage entry point from `file` (with common engine modules registered).
bool compileKernelStage(const char* file, const char* embedded, const char* entry, KernelHookStage& out,
                        std::string& err);

/// Compile a built-in stage entry point from `file` and create its VkShaderModule.
VkShaderModule createKernelStageModule(VkDevice dev, const char* file, const char* embedded, const char* entry);

/// Compile a volume's hook module (`file` names it in diagnostics).
bool compileKernelVolumeHook(const std::string& module, const std::string& file, const KernelHookModules& modules,
                             KernelVolumeHook& out, std::string& err,
                             const KernelHookKnobs& knobs = {}, bool enableProbes = false);

/// Compile a mesh's hook module for `target` (`file` names it in diagnostics).
bool compileKernelMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module,
                           const std::string& file, const KernelHookModules& modules,
                           KernelMeshHook& out, std::string& err,
                           const KernelHookKnobs& knobs = {}, bool enableProbes = false);

/// Compile a mesh hook's fragment variant (cuts / indirect output).
bool compileKernelMeshHookFragment(const std::string& module, const std::string& file,
                                   const KernelHookModules& modules, bool hasLight, bool indirect, bool cuts,
                                   KernelHookStage& out, std::string& err,
                                   const KernelHookKnobs& knobs = {}, bool enableProbes = false);

/// True once every stage a prepare started is built (or failed: the compile
/// that takes it reports why). Callable from the page thread at any time.
using KernelHookReady = std::function<bool()>;

/// Start compiling, each on a thread of its own, the stages a mesh hook set
/// for `target` may draw with: its vertex stage, its fragment stage's
/// variants (indirect, cuts) and its shadow casters. A stage is compiled once
/// for all its callers, keyed as compileKernelMeshHook keys it, so the
/// compile or draw that first takes one finds it built rather than compiling
/// it then.
KernelHookReady prepareKernelMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module,
                                      const std::string& file, const KernelHookModules& modules,
                                      const KernelHookKnobs& knobs = {});
/// The same for a volume's hook: volume_main, and volume_near when it gives
/// a medium.
KernelHookReady prepareKernelVolumeHook(const std::string& module, const std::string& file,
                                        const KernelHookModules& modules, const KernelHookKnobs& knobs = {});
/// prepareKernelMeshHook, not waiting on what it started.
void warmMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module, const std::string& file,
                  const KernelHookModules& modules, const KernelHookKnobs& knobs = {});

/// Compile a mesh hook's shadow-caster vertex stage.
bool compileKernelShadowHook(ShadowCaster caster, bool cutout, const std::string& module,
                             const std::string& file, const KernelHookModules& modules,
                             KernelHookStage& out, std::string& err,
                             const KernelHookKnobs& knobs = {});

} // namespace bro::scene::vk
