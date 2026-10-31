#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/debug_views.h"

#include <brokernel/brokernel.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

#include "scene.bk.src.h"
#include "atmosphere.bk.src.h"
#include "lighting.bk.src.h"
#include "custom.bk.src.h"
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

// `entry` of `stageFile` composed with the engine's modules, `near` and the
// hook.
constexpr const char* kEngineModules[] = {"scene", "atmosphere", "lighting", "custom", "volume", "near", "hook"};

brokernel::CompileResult compileStage(const char* stageFile, const char* stageEmbedded, const char* entry,
                                      const std::string& near, const std::string& hook,
                                      const KernelHookModules& modules) {
    brokernel::Compiler compiler;
    for (const auto& [name, source] : modules) compiler.register_module(name, source);
    compiler.register_module("scene", engineSource("common/scene.bk", kBkSceneSrc));
    compiler.register_module("atmosphere", engineSource("common/atmosphere.bk", kBkAtmosphereSrc));
    compiler.register_module("lighting", engineSource("common/lighting.bk", kBkLightingSrc));
    compiler.register_module("custom", engineSource("common/custom.bk", kBkCustomSrc));
    compiler.register_module("volume", engineSource("draw/volume.bk", kBkVolumeSrc));
    compiler.register_module("near", near);
    compiler.register_module("hook", hook);
    brokernel::CompileOptions opts;
    opts.entry_name = entry;
    opts.compile_cpu = false;
    return compiler.compile_source(engineSource(stageFile, stageEmbedded), stageFile, opts);
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

} // namespace

bool compileKernelVolumeHook(const std::string& module, const std::string& file, const KernelHookModules& modules,
                             KernelVolumeHook& out, std::string& err) {
    for (const auto& m : modules) {
        for (const char* name : kEngineModules) {
            if (m.first != name) continue;
            err = file + ": module '" + m.first + "' is the engine's";
            return false;
        }
    }
    const std::string hook = debug_views::expand(module, "scene::debug_view()");
    static const std::regex kMedium(R"(\bfn\s+medium\s*\()");
    out.medium = std::regex_search(hook, kMedium);
    const std::string near = out.medium ? engineSource("draw/near_grid.bk", kBkNearGridSrc)
                                        : engineSource("draw/near_none.bk", kBkNearNoneSrc);
    // Errors name the hook's file (the module `hook` in the diagnostics).
    const auto fail = [&] {
        err = file + ": " + err;
        return false;
    };
    const brokernel::CompileResult main =
        compileStage("draw/volume_main.bk", kBkVolumeMainSrc, "volume_main", near, hook, modules);
    if (!takeStage(main, "volume_main", out.main, err)) return fail();
    if (out.medium) {
        const brokernel::CompileResult nearPass =
            compileStage("draw/volume_near.bk", kBkVolumeNearSrc, "volume_near", near, hook, modules);
        if (!takeStage(nearPass, "volume_near", out.nearPass, err)) return fail();
        if (!out.iface.fromKernel({&main, &nearPass}, err)) return fail();
        return true;
    }
    if (!out.iface.fromKernel({&main}, err)) return fail();
    return true;
}

} // namespace bro::scene::vk
