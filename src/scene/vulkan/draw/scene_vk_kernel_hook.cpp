#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/debug_views.h"
#include "scene/kernel_source_scan.h"
#include "tilecache/tiles_module.h"
#include "util/log.h"
#include "util/page_pump.h"

#include <brokernel/brokernel.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "wind.bk.src.h"
#include "scene.bk.src.h"
#include "atmosphere.bk.src.h"
#include "lighting.bk.src.h"
#include "custom.bk.src.h"
#include "vegetation.bk.src.h"
#include "procedural.bk.src.h"
#include "bark.bk.src.h"
#include "mesh.bk.src.h"
#include "mesh_main.bk.src.h"
#include "shadow.bk.src.h"
#include "volume.bk.src.h"
#include "volume_main.bk.src.h"
#include "volume_near.bk.src.h"
#include "near_grid.bk.src.h"
#include "near_none.bk.src.h"

namespace bro::scene::vk {

const char* buildShaderSourceDir() {
    return BRO_SCENE_SHADER_SOURCE_DIR "/";
}

const std::string& shaderSourceDir() {
    static const std::string s_dir = []() -> std::string {
        const char* env = std::getenv("BRO_SCENE_SHADER_SOURCE");
        if (!env || !*env || std::strcmp(env, "0") == 0) return {};
        std::string dir = std::strcmp(env, "1") == 0 ? std::string(BRO_SCENE_SHADER_SOURCE_DIR) : std::string(env);
        for (char& c : dir)
            if (c == '\\') c = '/';
        if (!dir.empty() && dir.back() != '/') dir += '/';
        if (!std::ifstream(dir + "common/scene.bk")) {
            LOG_WARN("BRO_SCENE_SHADER_SOURCE=%s holds no common/scene.bk; using embedded shaders", env);
            return {};
        }
        LOG_INFO("Scene shaders: compiling from %s", dir.c_str());
        return dir;
    }();
    return s_dir;
}

std::string shaderSource(const char* fileName, const char* embedded) {
    // Set B's: a file given by name, else its tree, else set A's.
    const bool setB = shader_sets::compilingSetB();
    if (setB) {
        const auto& given = shader_sets::setBSources();
        if (auto it = given.find(fileName); it != given.end()) return it->second;
    }
    const std::string& dir = setB && !shader_sets::setBSourceDir().empty() ? shader_sets::setBSourceDir()
                                                                         : shaderSourceDir();
    if (!dir.empty()) {
        std::ifstream f(dir + fileName);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
        LOG_WARN("BRO_SCENE_SHADER_SOURCE: cannot open %s%s; using embedded copy", dir.c_str(), fileName);
    }
    return embedded ? std::string(embedded) : std::string();
}

namespace {

std::string engineSource(const char* file, const char* embedded) {
    return shaderSource(file, embedded);
}

constexpr const char* kEngineModules[] = {
    "wind", "scene", "atmosphere", "lighting", "custom", "volume", "near",
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

std::atomic<uint64_t> s_reloadGeneration{0};

void applyCompileOptions(brokernel::CompileOptions& opts, const KernelHookKnobs& knobs, bool enableProbes) {
    for (const auto& [name, val] : knobs) {
        opts.knob_overrides[name] = brokernel::ConstValue::from_f64(val);
    }
    if (enableProbes) {
        opts.enable_probes = true;
        opts.enable_fragment_probes = true;
    }
}

void registerCommonModules(brokernel::Compiler& compiler, const KernelHookModules& modules) {
    compiler.register_module("tiles", bro::tilecache::tilesModuleSource());
    for (const auto& [name, source] : modules) compiler.register_module(name, source);
    compiler.register_module("wind", engineSource("common/wind.bk", kBkWindSrc));
    compiler.register_module("scene", engineSource("common/scene.bk", kBkSceneSrc));
    compiler.register_module("atmosphere", engineSource("common/atmosphere.bk", kBkAtmosphereSrc));
    compiler.register_module("lighting", engineSource("common/lighting.bk", kBkLightingSrc));
    compiler.register_module("custom", engineSource("common/custom.bk", kBkCustomSrc));
    compiler.set_module_resolver([](std::string_view name) { return brokernel::ModuleRegistry::resolve_default(name); });
}

KernelHookModules resolveHookModules(const std::string& hook, const KernelHookModules& modules) {
    KernelHookModules out = modules;
    auto hasMod = [&](const std::string& name) {
        if (name == "tiles") return true;
        for (const char* eng : kEngineModules) {
            if (name == eng) return true;
        }
        for (const auto& [k, _] : out) {
            if (k == name) return true;
        }
        return false;
    };
    std::vector<std::string> queue;
    queue.push_back(hook);
    for (const auto& [_, src] : modules) queue.push_back(src);
    while (!queue.empty()) {
        std::string cur = std::move(queue.back());
        queue.pop_back();
        for (std::string& imp : kernelSourceImports(cur)) {
            if (hasMod(imp)) continue;
            if (auto resolved = brokernel::ModuleRegistry::resolve_default(imp)) {
                out.emplace_back(std::move(imp), *resolved);
                queue.push_back(*resolved);
            }
        }
    }
    return out;
}

// Every hook stage compiled, by all it is compiled from (the stage's file and
// entry, the hook, the app's modules, a volume's near module): a module set
// on a node compiles each stage once, on a thread of its own, and a draw
// that first takes a stage finds it built when warmMeshHook started it, or
// the module's check when it was set compiled it; the engine's own mesh and
// shadow stages are started at launch (prewarmEngineStages). A compile
// thread touches only its own stage's future; the map is locked. Not cached
// while the engine's shaders are read from the source tree (they change
// under the key) or set B compiles.
using StageResult = std::shared_ptr<const brokernel::CompileResult>;
using StageFuture = std::shared_future<StageResult>;
constexpr size_t kMaxStages = 256;

std::string stageKey(const char* stageFile, const char* entry, const std::string& near, const std::string& hook,
                     const KernelHookModules& modules, const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
    std::string key = stageFile;
    for (const char* s : {entry, near.c_str(), hook.c_str()}) {
        key += '\x1f';
        key += s;
    }
    for (const auto& [name, source] : modules) {
        key += '\x1e';
        key += name;
        key += '\x1f';
        key += source;
    }
    for (const auto& [kname, kval] : knobs) {
        key += '\x1d';
        key += kname;
        key += '=';
        key += std::to_string(kval);
    }
    if (enableProbes) key += "\x1cprobe";
    const uint64_t rgen = s_reloadGeneration.load(std::memory_order_relaxed);
    if (rgen != 0) {
        key += "\x1b" + std::to_string(rgen);
    }
    return key;
}

StageFuture stageFuture(std::string key, std::function<brokernel::CompileResult()> compile) {
    const auto run = [compile] { return std::make_shared<const brokernel::CompileResult>(compile()); };
    if (!shaderSourceDir().empty() || shader_sets::compilingSetB()) {
        std::promise<StageResult> p;
        p.set_value(run());
        return p.get_future().share();
    }
    static std::mutex s_mu;
    static std::unordered_map<std::string, StageFuture> s_stages;
    std::lock_guard<std::mutex> lock(s_mu);
    if (auto it = s_stages.find(key); it != s_stages.end()) return it->second;
    if (s_stages.size() >= kMaxStages) {
        for (auto it = s_stages.begin(); it != s_stages.end();) {
            const bool done = it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            it = done ? s_stages.erase(it) : std::next(it);
        }
    }
    StageFuture f = std::async(std::launch::async, run).share();
    s_stages.emplace(std::move(key), f);
    return f;
}

brokernel::CompileResult buildVolumeStage(const char* stageFile, const char* stageEmbedded, const char* entry,
                                          const std::string& near, const std::string& hook,
                                          const KernelHookModules& modules,
                                          const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
    brokernel::Compiler compiler;
    registerCommonModules(compiler, modules);
    compiler.register_module("volume", engineSource("draw/volume.bk", kBkVolumeSrc));
    compiler.register_module("near", near);
    compiler.register_module("hook", hook);
    brokernel::CompileOptions opts;
    opts.entry_name = entry;
    opts.compile_cpu = false;
    applyCompileOptions(opts, knobs, enableProbes);
    return compiler.compile_source(engineSource(stageFile, stageEmbedded), stageFile, opts);
}

StageFuture volumeStage(const char* stageFile, const char* stageEmbedded, const char* entry,
                        const std::string& near, const std::string& hook, const KernelHookModules& modules,
                        const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
    const KernelHookModules resolved = resolveHookModules(hook, modules);
    return stageFuture(stageKey(stageFile, entry, near, hook, resolved, knobs, enableProbes),
                       [=] { return buildVolumeStage(stageFile, stageEmbedded, entry, near, hook, resolved, knobs, enableProbes); });
}

brokernel::CompileResult compileVolumeStage(const char* stageFile, const char* stageEmbedded, const char* entry,
                                            const std::string& near, const std::string& hook,
                                            const KernelHookModules& modules,
                                            const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
    const StageFuture f = volumeStage(stageFile, stageEmbedded, entry, near, hook, modules, knobs, enableProbes);
    return *util::waitPumping(f);
}

brokernel::CompileResult buildMeshOrShadowEntry(const char* stageFile, const char* stageEmbedded, const char* entry,
                                                const std::string& hook, const KernelHookModules& modules,
                                                const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
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
    applyCompileOptions(opts, knobs, enableProbes);
    return compiler.compile_source(engineSource(stageFile, stageEmbedded), stageFile, opts);
}

StageFuture meshOrShadowStage(const char* stageFile, const char* stageEmbedded, const char* entry,
                              const std::string& hook, const KernelHookModules& modules,
                              const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
    const KernelHookModules resolved = resolveHookModules(hook, modules);
    return stageFuture(stageKey(stageFile, entry, std::string(), hook, resolved, knobs, enableProbes),
                       [=] { return buildMeshOrShadowEntry(stageFile, stageEmbedded, entry, hook, resolved, knobs, enableProbes); });
}

StageFuture meshStage(const char* entry, const std::string& hook, const KernelHookModules& modules,
                      const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
    return meshOrShadowStage("draw/mesh_main.bk", kBkMeshMainSrc, entry, hook, modules, knobs, enableProbes);
}

StageFuture shadowStage(const char* entry, const std::string& hook, const KernelHookModules& modules,
                        const KernelHookKnobs& knobs = {}) {
    return meshOrShadowStage("shadow/shadow.bk", kBkShadowSrc, entry, hook, modules, knobs, false);
}

brokernel::CompileResult compileMeshEntry(const char* entry, const std::string& hook,
                                          const KernelHookModules& modules,
                                          const KernelHookKnobs& knobs = {}, bool enableProbes = false) {
    const StageFuture f = meshStage(entry, hook, modules, knobs, enableProbes);
    return *util::waitPumping(f);
}

brokernel::CompileResult compileShadowEntry(const char* entry, const std::string& hook,
                                            const KernelHookModules& modules,
                                            const KernelHookKnobs& knobs = {}) {
    const StageFuture f = shadowStage(entry, hook, modules, knobs);
    return *util::waitPumping(f);
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
    out.costs.clear();
    for (const auto& c : k->function_costs) out.costs.push_back({c.name, c.self, c.total});
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

const char* shadowEntry(ShadowCaster caster, bool cutout) {
    if (caster == ShadowCaster::Skinned) return "shadow_hook_skinned_vert";
    if (caster == ShadowCaster::Instanced) return cutout ? "shadow_hook_instanced_cutout_vert" : "shadow_hook_instanced_vert";
    return cutout ? "shadow_hook_cutout_vert" : "shadow_hook_vert";
}

void noteStageRecipe(std::string name, const std::vector<uint32_t>& words,
                     std::function<std::vector<uint32_t>(std::string* err)> compile) {
    if (shader_sets::compilingSetB() || words.empty()) return;
    auto r = std::make_shared<ShaderRecipe>();
    r->name = std::move(name);
    r->compile = std::move(compile);
    r->words = words;
    shader_sets::noteSpirv(std::move(r));
}

// A hook's text and modules as set B's compiles take them: its own file, or
// a module given to it, replaced where set B gives a source of that name.
struct HookInputs {
    std::string module;
    KernelHookModules modules;
};
HookInputs hookInputs(const std::string& file, const std::string& module, const KernelHookModules& modules) {
    HookInputs in{module, modules};
    const auto& given = shader_sets::setBSources();
    if (!given.empty()) {
        if (auto it = given.find(file); it != given.end()) in.module = it->second;
        for (auto& [name, text] : in.modules) {
            if (auto it = given.find(name); it != given.end()) text = it->second;
        }
    }
    // SCENE_DEBUG(name) works in the modules given to a hook as in its own
    // file (the hook's own file is expanded by each compile); a module using
    // it imports scene, as the hook does.
    for (auto& [name, text] : in.modules) text = debug_views::expand(text, "scene::debug_view()");
    return in;
}

// A hook stage's recipe: the hook compiled again (as set B's compiles take
// it), that stage's SPIR-V.
using HookStageCompile = std::function<bool(std::vector<uint32_t>& words, std::string& err)>;
void noteHookRecipe(const std::string& file, const char* stage, const std::vector<uint32_t>& words,
                    HookStageCompile compile) {
    noteStageRecipe(file + ":" + stage, words, [compile](std::string* errOut) {
        std::vector<uint32_t> w;
        std::string err;
        if (!compile(w, err)) {
            if (errOut) *errOut = std::move(err);
            return std::vector<uint32_t>{};
        }
        return w;
    });
}

} // namespace

bool compileKernelMeshStage(const char* entry, KernelHookStage& out, std::string& err) {
    static std::mutex s_mu;
    static std::unordered_map<std::string, KernelHookStage> s_cache;
    const bool setB = shader_sets::compilingSetB();
    if (!setB) {
        std::lock_guard<std::mutex> lock(s_mu);
        auto it = s_cache.find(entry);
        if (it != s_cache.end()) {
            out = it->second;
            return true;
        }
    }
    const brokernel::CompileResult res = compileMeshEntry(entry, "module hook;\n", {});
    if (!takeStage(res, entry, out, err)) return false;
    if (!setB) {
        {
            std::lock_guard<std::mutex> lock(s_mu);
            s_cache.emplace(entry, out);
        }
        std::string e(entry);
        noteStageRecipe("draw/mesh_main.bk:" + e, out.spirv, [e](std::string* errOut) {
            KernelHookStage st;
            std::string localErr;
            if (!compileKernelMeshStage(e.c_str(), st, localErr)) {
                if (errOut) *errOut = std::move(localErr);
                return std::vector<uint32_t>{};
            }
            return st.spirv;
        });
    }
    return true;
}

bool compileKernelShadowStage(const char* entry, KernelHookStage& out, std::string& err) {
    static std::mutex s_mu;
    static std::unordered_map<std::string, KernelHookStage> s_cache;
    const bool setB = shader_sets::compilingSetB();
    if (!setB) {
        std::lock_guard<std::mutex> lock(s_mu);
        auto it = s_cache.find(entry);
        if (it != s_cache.end()) {
            out = it->second;
            return true;
        }
    }
    const brokernel::CompileResult res = compileShadowEntry(entry, "module hook;\n", {});
    if (!takeStage(res, entry, out, err)) return false;
    if (!setB) {
        {
            std::lock_guard<std::mutex> lock(s_mu);
            s_cache.emplace(entry, out);
        }
        std::string e(entry);
        noteStageRecipe("shadow/shadow.bk:" + e, out.spirv, [e](std::string* errOut) {
            KernelHookStage st;
            std::string localErr;
            if (!compileKernelShadowStage(e.c_str(), st, localErr)) {
                if (errOut) *errOut = std::move(localErr);
                return std::vector<uint32_t>{};
            }
            return st.spirv;
        });
    }
    return true;
}

void prewarmKernelMeshStages(std::span<const char* const> entries) {
    if (!shaderSourceDir().empty() || shader_sets::compilingSetB()) return;
    for (const char* e : entries) meshStage(e, "module hook;\n", {});
}

void prewarmKernelShadowStages(std::span<const char* const> entries) {
    if (!shaderSourceDir().empty() || shader_sets::compilingSetB()) return;
    for (const char* e : entries) shadowStage(e, "module hook;\n", {});
}

bool compileKernelStage(const char* file, const char* embedded, const char* entry, KernelHookStage& out,
                        std::string& err) {
    static std::mutex s_mu;
    static std::unordered_map<std::string, KernelHookStage> s_cache;
    const std::string key = std::string(file) + ":" + entry;
    const bool setB = shader_sets::compilingSetB();
    if (!setB) {
        std::lock_guard<std::mutex> lock(s_mu);
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            out = it->second;
            return true;
        }
    }
    brokernel::Compiler compiler;
    registerCommonModules(compiler, {});
    compiler.register_module("vegetation", engineSource("common/vegetation.bk", kBkVegetationSrc));
    compiler.register_module("procedural", engineSource("common/procedural.bk", kBkProceduralSrc));
    brokernel::CompileOptions opts;
    opts.entry_name = entry;
    opts.compile_cpu = false;
    const brokernel::CompileResult res = compiler.compile_source(engineSource(file, embedded), file, opts);
    if (!takeStage(res, entry, out, err)) return false;
    if (!setB) {
        {
            std::lock_guard<std::mutex> lock(s_mu);
            s_cache.emplace(key, out);
        }
        std::string f(file);
        std::string emb(embedded ? embedded : "");
        std::string e(entry);
        noteStageRecipe(key, out.spirv, [f, emb, e](std::string* errOut) {
            KernelHookStage st;
            std::string localErr;
            if (!compileKernelStage(f.c_str(), emb.c_str(), e.c_str(), st, localErr)) {
                if (errOut) *errOut = std::move(localErr);
                return std::vector<uint32_t>{};
            }
            return st.spirv;
        });
    }
    return true;
}

VkShaderModule createKernelStageModule(VkDevice dev, const char* file, const char* embedded, const char* entry) {
    KernelHookStage stage;
    std::string err;
    if (!compileKernelStage(file, embedded, entry, stage, err)) {
        LOG_ERROR("createKernelStageModule(%s:%s): %s", file, entry, err.c_str());
        return VK_NULL_HANDLE;
    }
    return SceneVkShaderModule::create(dev, stage.spirv);
}

uint64_t kernelHookReloadGeneration() {
    return s_reloadGeneration.load(std::memory_order_relaxed);
}

void bumpKernelHookReloadGeneration() {
    s_reloadGeneration.fetch_add(1, std::memory_order_relaxed);
}

bool compileKernelVolumeHook(const std::string& module0, const std::string& file, const KernelHookModules& modules0,
                             KernelVolumeHook& out, std::string& err,
                             const KernelHookKnobs& knobs, bool enableProbes) {
    const HookInputs in = hookInputs(file, module0, modules0);
    const std::string& module = in.module;
    const KernelHookModules& modules = in.modules;
    if (!checkModuleNames(file, modules, err)) return false;
    const auto noteRecipes = [&] {
        if (shader_sets::compilingSetB() || enableProbes) return;
        for (const bool nearPass : {false, true}) {
            if (nearPass && !out.medium) continue;
            const std::vector<uint32_t>& words = nearPass ? out.nearPass.spirv : out.main.spirv;
            if (words.empty()) continue;
            noteHookRecipe(file, nearPass ? "volume_near" : "volume_main", words,
                           [module0, file, modules0, knobs, nearPass](std::vector<uint32_t>& w, std::string& e) {
                               KernelVolumeHook h;
                               if (!compileKernelVolumeHook(module0, file, modules0, h, e, knobs, false)) return false;
                               w = nearPass ? h.nearPass.spirv : h.main.spirv;
                               return true;
                           });
        }
    };
    const std::string hook = debug_views::expand(module, "scene::debug_view()");
    out.medium = kernelSourceDefinesFn(hook, "medium");
    const std::string near = out.medium ? engineSource("draw/near_grid.bk", kBkNearGridSrc)
                                        : engineSource("draw/near_none.bk", kBkNearNoneSrc);
    const auto fail = [&] {
        err = file + ": " + err;
        return false;
    };
    const brokernel::CompileResult main =
        compileVolumeStage("draw/volume_main.bk", kBkVolumeMainSrc, "volume_main", near, hook, modules, knobs, enableProbes);
    if (!takeStage(main, "volume_main", out.main, err)) return fail();
    out.probes.clear();
    for (const auto& p : main.probes) {
        out.probes.push_back({p.name, p.index, p.type.is_vector() ? p.type.dimension : 1u});
    }
    if (out.medium) {
        const brokernel::CompileResult nearPass =
            compileVolumeStage("draw/volume_near.bk", kBkVolumeNearSrc, "volume_near", near, hook, modules, knobs, false);
        if (!takeStage(nearPass, "volume_near", out.nearPass, err)) return fail();
        if (!out.iface.fromKernel({&main, &nearPass}, err)) return fail();
        noteRecipes();
        return true;
    }
    if (!out.iface.fromKernel({&main}, err)) return fail();
    noteRecipes();
    return true;
}

bool compileKernelMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module0,
                           const std::string& file, const KernelHookModules& modules0,
                           KernelMeshHook& out, std::string& err,
                           const KernelHookKnobs& knobs, bool enableProbes) {
    const HookInputs in = hookInputs(file, module0, modules0);
    const std::string& module = in.module;
    const KernelHookModules& modules = in.modules;
    if (!checkModuleNames(file, modules, err)) return false;
    const std::string hook0 = debug_views::expand(module, "scene::debug_view()");
    const auto fail = [&] {
        err = file + ": " + err;
        return false;
    };
    std::string hook;
    if (!expandMeshHookVaryings(hook0, file, hook, err)) return fail();
    out.hasVertex = kernelSourceDefinesFn(hook, "vertex");
    out.hasFragment = kernelSourceDefinesFn(hook, "fragment");
    out.hasLight = kernelSourceDefinesFn(hook, "light");
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
        vsRes = compileMeshEntry(vsEntry, hook, modules, knobs, false);
        if (!takeStage(vsRes, vsEntry, out.vs, err)) return fail();
    }
    out.probes.clear();
    if (out.hasFragment) {
        const char* fsEntry = fragmentEntry(out.hasLight, false, false);
        fsRes = compileMeshEntry(fsEntry, hook, modules, knobs, enableProbes);
        if (!takeStage(fsRes, fsEntry, out.fs, err)) return fail();
        for (const auto& p : fsRes.probes) {
            out.probes.push_back({p.name, p.index, p.type.is_vector() ? p.type.dimension : 1u});
        }
    }
    if (out.hasVertex && out.hasFragment) {
        if (!out.iface.fromKernel({&vsRes, &fsRes}, err)) return fail();
    } else if (out.hasVertex) {
        if (!out.iface.fromKernel({&vsRes}, err)) return fail();
    } else {
        if (!out.iface.fromKernel({&fsRes}, err)) return fail();
    }
    if (!shader_sets::compilingSetB() && !enableProbes) {
        for (const bool vertex : {true, false}) {
            if (vertex ? !out.hasVertex : !out.hasFragment) continue;
            noteHookRecipe(file, vertex ? "vertex" : "fragment", vertex ? out.vs.spirv : out.fs.spirv,
                           [target, module0, file, modules0, knobs, vertex](std::vector<uint32_t>& w, std::string& e) {
                               KernelMeshHook h;
                               if (!compileKernelMeshHook(target, module0, file, modules0, h, e, knobs, false))
                                   return false;
                               w = vertex ? h.vs.spirv : h.fs.spirv;
                               return true;
                           });
        }
    }
    return true;
}

bool compileKernelMeshHookFragment(const std::string& module0, const std::string& file,
                                   const KernelHookModules& modules0, bool hasLight, bool indirect, bool cuts,
                                   KernelHookStage& out, std::string& err,
                                   const KernelHookKnobs& knobs, bool enableProbes) {
    const HookInputs in = hookInputs(file, module0, modules0);
    const std::string& module = in.module;
    const KernelHookModules& modules = in.modules;
    const std::string hook0 = debug_views::expand(module, "scene::debug_view()");
    std::string hook;
    if (!expandMeshHookVaryings(hook0, file, hook, err)) {
        err = file + ": " + err;
        return false;
    }
    const char* fsEntry = fragmentEntry(hasLight, indirect, cuts);
    const brokernel::CompileResult fsRes = compileMeshEntry(fsEntry, hook, modules, knobs, enableProbes);
    if (!takeStage(fsRes, fsEntry, out, err)) {
        err = file + ": " + err;
        return false;
    }
    if (!shader_sets::compilingSetB() && !enableProbes) {
        noteHookRecipe(file, fsEntry, out.spirv,
                       [module0, file, modules0, hasLight, indirect, cuts, knobs](std::vector<uint32_t>& w,
                                                                                 std::string& e) {
                           KernelHookStage st;
                           if (!compileKernelMeshHookFragment(module0, file, modules0, hasLight, indirect, cuts, st, e,
                                                              knobs, false))
                               return false;
                           w = st.spirv;
                           return true;
                       });
    }
    return true;
}

bool compileKernelShadowHook(ShadowCaster caster, bool cutout, const std::string& module0,
                             const std::string& file, const KernelHookModules& modules0,
                             KernelHookStage& out, std::string& err,
                             const KernelHookKnobs& knobs) {
    const HookInputs in = hookInputs(file, module0, modules0);
    const std::string& module = in.module;
    const KernelHookModules& modules = in.modules;
    const std::string hook0 = debug_views::expand(module, "scene::debug_view()");
    std::string hook;
    if (!expandMeshHookVaryings(hook0, file, hook, err)) {
        err = file + ": " + err;
        return false;
    }
    const char* vsEntry = shadowEntry(caster, cutout);
    const brokernel::CompileResult vsRes = compileShadowEntry(vsEntry, hook, modules, knobs);
    if (!takeStage(vsRes, vsEntry, out, err)) {
        err = file + ": " + err;
        return false;
    }
    if (!shader_sets::compilingSetB()) {
        noteHookRecipe(file, vsEntry, out.spirv,
                       [caster, cutout, module0, file, modules0, knobs](std::vector<uint32_t>& w, std::string& e) {
                           KernelHookStage st;
                           if (!compileKernelShadowHook(caster, cutout, module0, file, modules0, st, e, knobs))
                               return false;
                           w = st.spirv;
                           return true;
                       });
    }
    return true;
}

namespace {

KernelHookReady readyWhenAll(std::vector<StageFuture> stages) {
    return [stages = std::move(stages)] {
        for (const StageFuture& f : stages) {
            if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        }
        return true;
    };
}

}  // namespace

KernelHookReady prepareKernelMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module0,
                                      const std::string& file, const KernelHookModules& modules0,
                                      const KernelHookKnobs& knobs) {
    // Set B and the source tree compile each stage where it is taken.
    if (!shaderSourceDir().empty() || shader_sets::compilingSetB()) return [] { return true; };
    const HookInputs in = hookInputs(file, module0, modules0);
    const std::string hook0 = debug_views::expand(in.module, "scene::debug_view()");
    std::string hook;
    std::string unusedErr;
    // A hook that does not expand fails when it is set; nothing to wait for.
    if (!expandMeshHookVaryings(hook0, file, hook, unusedErr)) return [] { return true; };
    const KernelHookModules& modules = in.modules;
    const bool hasVertex = kernelSourceDefinesFn(hook, "vertex");
    const bool hasLight = kernelSourceDefinesFn(hook, "light");
    std::vector<StageFuture> stages;
    if (hasVertex) {
        stages.push_back(meshStage(vertexEntryForTarget(target), hook, modules, knobs, false));
        const ShadowCaster caster = target == SceneRenderer::CustomShaderTarget::Skinned     ? ShadowCaster::Skinned
                                  : target == SceneRenderer::CustomShaderTarget::Instanced ? ShadowCaster::Instanced
                                                                                           : ShadowCaster::Static;
        for (const bool cutout : {false, true}) stages.push_back(shadowStage(shadowEntry(caster, cutout), hook, modules, knobs));
    }
    if (kernelSourceDefinesFn(hook, "fragment")) {
        for (const bool indirect : {false, true})
            for (const bool cuts : {false, true})
                stages.push_back(meshStage(fragmentEntry(hasLight, indirect, cuts), hook, modules, knobs, false));
    }
    return readyWhenAll(std::move(stages));
}

KernelHookReady prepareKernelVolumeHook(const std::string& module0, const std::string& file,
                                        const KernelHookModules& modules0, const KernelHookKnobs& knobs) {
    if (!shaderSourceDir().empty() || shader_sets::compilingSetB()) return [] { return true; };
    const HookInputs in = hookInputs(file, module0, modules0);
    const std::string hook = debug_views::expand(in.module, "scene::debug_view()");
    const bool medium = kernelSourceDefinesFn(hook, "medium");
    const std::string near = medium ? engineSource("draw/near_grid.bk", kBkNearGridSrc)
                                    : engineSource("draw/near_none.bk", kBkNearNoneSrc);
    std::vector<StageFuture> stages;
    stages.push_back(volumeStage("draw/volume_main.bk", kBkVolumeMainSrc, "volume_main", near, hook, in.modules, knobs, false));
    if (medium) {
        stages.push_back(volumeStage("draw/volume_near.bk", kBkVolumeNearSrc, "volume_near", near, hook, in.modules, knobs, false));
    }
    return readyWhenAll(std::move(stages));
}

void warmMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module, const std::string& file,
                  const KernelHookModules& modules, const KernelHookKnobs& knobs) {
    prepareKernelMeshHook(target, module, file, modules, knobs);
}

} // namespace bro::scene::vk
