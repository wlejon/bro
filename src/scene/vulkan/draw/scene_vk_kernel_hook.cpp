#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/debug_views.h"

#include <brokernel/brokernel.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <unordered_map>

#include "scene.bk.src.h"
#include "atmosphere.bk.src.h"
#include "lighting.bk.src.h"
#include "custom.bk.src.h"
#include "vegetation.bk.src.h"
#include "procedural.bk.src.h"
#include "bark.bk.src.h"
#include "mesh.bk.src.h"
#include "mesh_main.bk.src.h"
#include "volume.bk.src.h"
#include "volume_main.bk.src.h"
#include "volume_near.bk.src.h"
#include "near_grid.bk.src.h"
#include "near_none.bk.src.h"

namespace bro::scene::vk {

namespace {

std::string engineSource(const char* file, const char* embedded) {
    return SceneVkShaderCompiler::shaderSource(file, embedded);
}

constexpr const char* kEngineModules[] = {
    "scene", "atmosphere", "lighting", "custom", "volume", "near",
    "vegetation", "procedural", "bark", "mesh", "hook"
};

bool checkModuleNames(const std::string& file, const KernelHookModules& modules, std::string& err) {
    for (const auto& m : modules) {
        for (const char* name : kEngineModules) {
            if (m.first != name) continue;
            err = file + ": module '" + m.first + "' is the engine's";
            return false;
        }
    }
    return true;
}

void registerCommonModules(brokernel::Compiler& compiler, const KernelHookModules& modules) {
    for (const auto& [name, source] : modules) compiler.register_module(name, source);
    compiler.register_module("scene", engineSource("common/scene.bk", kBkSceneSrc));
    compiler.register_module("atmosphere", engineSource("common/atmosphere.bk", kBkAtmosphereSrc));
    compiler.register_module("lighting", engineSource("common/lighting.bk", kBkLightingSrc));
    compiler.register_module("custom", engineSource("common/custom.bk", kBkCustomSrc));
}

brokernel::CompileResult compileVolumeStage(const char* stageFile, const char* stageEmbedded, const char* entry,
                                            const std::string& near, const std::string& hook,
                                            const KernelHookModules& modules) {
    brokernel::Compiler compiler;
    registerCommonModules(compiler, modules);
    compiler.register_module("volume", engineSource("draw/volume.bk", kBkVolumeSrc));
    compiler.register_module("near", near);
    compiler.register_module("hook", hook);
    brokernel::CompileOptions opts;
    opts.entry_name = entry;
    opts.compile_cpu = false;
    return compiler.compile_source(engineSource(stageFile, stageEmbedded), stageFile, opts);
}

brokernel::CompileResult compileMeshEntry(const char* entry, const std::string& hook,
                                          const KernelHookModules& modules) {
    brokernel::Compiler compiler;
    registerCommonModules(compiler, modules);
    compiler.register_module("vegetation", engineSource("common/vegetation.bk", kBkVegetationSrc));
    compiler.register_module("procedural", engineSource("common/procedural.bk", kBkProceduralSrc));
    compiler.register_module("bark", engineSource("draw/bark.bk", kBkBarkSrc));
    compiler.register_module("mesh", engineSource("draw/mesh.bk", kBkMeshSrc));
    compiler.register_module("hook", hook);
    brokernel::CompileOptions opts;
    opts.entry_name = entry;
    opts.compile_cpu = false;
    return compiler.compile_source(engineSource("draw/mesh_main.bk", kBkMeshMainSrc), "draw/mesh_main.bk", opts);
}

// BRO_KERNEL_SPIRV_DUMP=<dir> writes each stage's SPIR-V there as
// <entry>.<hash of the words>.spv, for spirv-dis.
void dumpStage(const KernelHookStage& stage) {
    static const std::string s_dir = [] {
        const char* v = std::getenv("BRO_KERNEL_SPIRV_DUMP");
        return std::string(v ? v : "");
    }();
    if (s_dir.empty()) return;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t w : stage.spirv) h = (h ^ w) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof name, ".%016llx.spv", (unsigned long long)h);
    std::error_code ec;
    std::filesystem::create_directories(s_dir, ec);
    std::ofstream f(s_dir + "/" + stage.entry + name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(stage.spirv.data()), std::streamsize(stage.spirv.size() * 4));
}

bool takeStage(const brokernel::CompileResult& result, const char* entry, KernelHookStage& out, std::string& err) {
    if (!result.ok) {
        err = result.error_message();
        return false;
    }
    const brokernel::CompiledKernel* k = result.find_kernel(entry);
    if (!k || k->spirv.words.empty()) {
        err = std::string("no SPIR-V for ") + entry;
        return false;
    }
    out.spirv = k->spirv.words;
    out.entry = k->spirv.entry;
    dumpStage(out);
    return true;
}

const char* vertexEntryForTarget(SceneRenderer::CustomShaderTarget target) {
    switch (target) {
    case SceneRenderer::CustomShaderTarget::Instanced: return "mesh_hook_instanced_vert";
    case SceneRenderer::CustomShaderTarget::Skinned: return "mesh_hook_skinned_vert";
    default: return "mesh_hook_vert";
    }
}

const char* fragmentEntry(bool hasLight, bool indirect, bool cuts) {
    if (hasLight) {
        if (indirect) return cuts ? "mesh_hook_light_indirect_cuts_frag" : "mesh_hook_light_indirect_frag";
        return cuts ? "mesh_hook_light_cuts_frag" : "mesh_hook_light_frag";
    }
    if (indirect) return cuts ? "mesh_hook_indirect_cuts_frag" : "mesh_hook_indirect_frag";
    return cuts ? "mesh_hook_cuts_frag" : "mesh_hook_frag";
}

} // namespace

bool compileKernelMeshStage(const char* entry, KernelHookStage& out, std::string& err) {
    static std::mutex s_mu;
    static std::unordered_map<std::string, KernelHookStage> s_cache;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        auto it = s_cache.find(entry);
        if (it != s_cache.end()) {
            out = it->second;
            return true;
        }
    }
    const brokernel::CompileResult res = compileMeshEntry(entry, "module hook;\n", {});
    if (!takeStage(res, entry, out, err)) return false;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_cache.emplace(entry, out);
    }
    return true;
}

bool compileKernelVolumeHook(const std::string& module, const std::string& file, const KernelHookModules& modules,
                             KernelVolumeHook& out, std::string& err) {
    if (!checkModuleNames(file, modules, err)) return false;
    const std::string hook = debug_views::expand(module, "scene::debug_view()");
    static const std::regex kMedium(R"(\bfn\s+medium\s*\()");
    out.medium = std::regex_search(hook, kMedium);
    const std::string near = out.medium ? engineSource("draw/near_grid.bk", kBkNearGridSrc)
                                        : engineSource("draw/near_none.bk", kBkNearNoneSrc);
    const auto fail = [&] {
        err = file + ": " + err;
        return false;
    };
    const brokernel::CompileResult main =
        compileVolumeStage("draw/volume_main.bk", kBkVolumeMainSrc, "volume_main", near, hook, modules);
    if (!takeStage(main, "volume_main", out.main, err)) return fail();
    if (out.medium) {
        const brokernel::CompileResult nearPass =
            compileVolumeStage("draw/volume_near.bk", kBkVolumeNearSrc, "volume_near", near, hook, modules);
        if (!takeStage(nearPass, "volume_near", out.nearPass, err)) return fail();
        if (!out.iface.fromKernel({&main, &nearPass}, err)) return fail();
        return true;
    }
    if (!out.iface.fromKernel({&main}, err)) return fail();
    return true;
}

bool compileKernelMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module,
                           const std::string& file, const KernelHookModules& modules,
                           KernelMeshHook& out, std::string& err) {
    if (!checkModuleNames(file, modules, err)) return false;
    const std::string hook = debug_views::expand(module, "scene::debug_view()");
    static const std::regex kVertex(R"(\bfn\s+vertex\s*\()");
    static const std::regex kFragment(R"(\bfn\s+fragment\s*\()");
    static const std::regex kLight(R"(\bfn\s+light\s*\()");
    out.hasVertex = std::regex_search(hook, kVertex);
    out.hasFragment = std::regex_search(hook, kFragment);
    out.hasLight = std::regex_search(hook, kLight);
    const auto fail = [&] {
        err = file + ": " + err;
        return false;
    };
    if (!out.hasVertex && !out.hasFragment) {
        err = "mesh module must define fn vertex and/or fn fragment";
        return fail();
    }
    if (out.hasLight && !out.hasFragment) {
        err = "mesh module's fn light requires fn fragment";
        return fail();
    }
    brokernel::CompileResult vsRes;
    brokernel::CompileResult fsRes;
    if (out.hasVertex) {
        const char* vsEntry = vertexEntryForTarget(target);
        vsRes = compileMeshEntry(vsEntry, hook, modules);
        if (!takeStage(vsRes, vsEntry, out.vs, err)) return fail();
    }
    if (out.hasFragment) {
        const char* fsEntry = fragmentEntry(out.hasLight, false, false);
        fsRes = compileMeshEntry(fsEntry, hook, modules);
        if (!takeStage(fsRes, fsEntry, out.fs, err)) return fail();
    }
    if (out.hasVertex && out.hasFragment) {
        if (!out.iface.fromKernel({&vsRes, &fsRes}, err)) return fail();
    } else if (out.hasVertex) {
        if (!out.iface.fromKernel({&vsRes}, err)) return fail();
    } else {
        if (!out.iface.fromKernel({&fsRes}, err)) return fail();
    }
    return true;
}

bool compileKernelMeshHookFragment(const std::string& module, const std::string& file,
                                   const KernelHookModules& modules, bool hasLight, bool indirect, bool cuts,
                                   KernelHookStage& out, std::string& err) {
    const std::string hook = debug_views::expand(module, "scene::debug_view()");
    const char* fsEntry = fragmentEntry(hasLight, indirect, cuts);
    const brokernel::CompileResult fsRes = compileMeshEntry(fsEntry, hook, modules);
    if (!takeStage(fsRes, fsEntry, out, err)) {
        err = file + ": " + err;
        return false;
    }
    return true;
}

bool compileKernelShadowHook(SceneVkCustomShader::ShadowCaster caster, bool cutout, const std::string& module,
                             const std::string& file, const KernelHookModules& modules,
                             KernelHookStage& out, std::string& err) {
    const std::string hook = debug_views::expand(module, "scene::debug_view()");
    const char* vsEntry = "shadow_hook_vert";
    if (caster == SceneVkCustomShader::ShadowCaster::Skinned) {
        vsEntry = "shadow_hook_skinned_vert";
    } else if (caster == SceneVkCustomShader::ShadowCaster::Instanced) {
        vsEntry = cutout ? "shadow_hook_instanced_cutout_vert" : "shadow_hook_instanced_vert";
    } else if (cutout) {
        vsEntry = "shadow_hook_cutout_vert";
    }
    const brokernel::CompileResult vsRes = compileMeshEntry(vsEntry, hook, modules);
    if (!takeStage(vsRes, vsEntry, out, err)) {
        err = file + ": " + err;
        return false;
    }
    return true;
}

} // namespace bro::scene::vk
