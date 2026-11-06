#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/debug_views.h"
#include "tilecache/tiles_module.h"
#include "util/log.h"

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
#include <regex>
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
    const std::string& dir = shader_sets::compilingSetB() ? shader_sets::setBSourceDir() : shaderSourceDir();
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
    static const std::regex kImportPatterns[] = {
        std::regex(R"(\bimport\s+([A-Za-z_][A-Za-z0-9_:]*)\s*(?:as\s+[A-Za-z_][A-Za-z0-9_]*\s*)?;)"),
        std::regex(R"(\bfrom\s+([A-Za-z_][A-Za-z0-9_:]*)\s+import\b)"),
        std::regex(R"(\buse\s+([A-Za-z_][A-Za-z0-9_:]*)::\{)"),
        std::regex(R"(\buse\s+([A-Za-z_][A-Za-z0-9_:]*)::[A-Za-z_*][A-Za-z0-9_]*\s*(?:as\s+[A-Za-z_][A-Za-z0-9_]*\s*)?;)"),
    };
    std::vector<std::string> queue;
    queue.push_back(hook);
    for (const auto& [_, src] : modules) queue.push_back(src);
    while (!queue.empty()) {
        std::string cur = std::move(queue.back());
        queue.pop_back();
        for (const auto& pat : kImportPatterns) {
            for (std::sregex_iterator it(cur.begin(), cur.end(), pat), end; it != end; ++it) {
                std::string imp = (*it)[1].str();
                if (hasMod(imp)) continue;
                if (auto resolved = brokernel::ModuleRegistry::resolve_default(imp)) {
                    out.emplace_back(imp, *resolved);
                    queue.push_back(*resolved);
                }
            }
        }
    }
    return out;
}

// Every hook stage compiled, by all it is compiled from (the stage's file and
// entry, the hook, the app's modules, a volume's near module): a module set
// on a node compiles each stage once, on a thread of its own, and a draw
// that first takes a stage finds it built when warmMeshHook started it, or
// the module's check when it was set compiled it. The page thread's alone
// (a pipeline built off the frame records its create-info here, on the page
// thread; only the driver's build is off it), and a compile thread touches
// only its own stage's future, so nothing here is shared. Not cached while
// the engine's shaders are read from the source tree (they change under the
// key) or set B compiles.
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
    static std::unordered_map<std::string, StageFuture> s_stages;
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
    return *volumeStage(stageFile, stageEmbedded, entry, near, hook, modules, knobs, enableProbes).get();
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
    return *meshStage(entry, hook, modules, knobs, enableProbes).get();
}

brokernel::CompileResult compileShadowEntry(const char* entry, const std::string& hook,
                                            const KernelHookModules& modules,
                                            const KernelHookKnobs& knobs = {}) {
    return *shadowStage(entry, hook, modules, knobs).get();
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

bool compileKernelVolumeHook(const std::string& module, const std::string& file, const KernelHookModules& modules,
                             KernelVolumeHook& out, std::string& err,
                             const KernelHookKnobs& knobs, bool enableProbes) {
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
        return true;
    }
    if (!out.iface.fromKernel({&main}, err)) return fail();
    return true;
}

bool compileKernelMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module,
                           const std::string& file, const KernelHookModules& modules,
                           KernelMeshHook& out, std::string& err,
                           const KernelHookKnobs& knobs, bool enableProbes) {
    if (!checkModuleNames(file, modules, err)) return false;
    const std::string hook0 = debug_views::expand(module, "scene::debug_view()");
    const auto fail = [&] {
        err = file + ": " + err;
        return false;
    };
    std::string hook;
    if (!expandMeshHookVaryings(hook0, file, hook, err)) return fail();
    static const std::regex kVertex(R"(\bfn\s+vertex\s*\()");
    static const std::regex kFragment(R"(\bfn\s+fragment\s*\()");
    static const std::regex kLight(R"(\bfn\s+light\s*\()");
    out.hasVertex = std::regex_search(hook, kVertex);
    out.hasFragment = std::regex_search(hook, kFragment);
    out.hasLight = std::regex_search(hook, kLight);
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
    return true;
}

bool compileKernelMeshHookFragment(const std::string& module, const std::string& file,
                                   const KernelHookModules& modules, bool hasLight, bool indirect, bool cuts,
                                   KernelHookStage& out, std::string& err,
                                   const KernelHookKnobs& knobs, bool enableProbes) {
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
    return true;
}

bool compileKernelShadowHook(ShadowCaster caster, bool cutout, const std::string& module,
                             const std::string& file, const KernelHookModules& modules,
                             KernelHookStage& out, std::string& err,
                             const KernelHookKnobs& knobs) {
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
    return true;
}

void warmMeshHook(SceneRenderer::CustomShaderTarget target, const std::string& module,
                  const KernelHookModules& modules, const KernelHookKnobs& knobs) {
    const std::string hook0 = debug_views::expand(module, "scene::debug_view()");
    std::string hook;
    std::string unusedErr;
    if (!expandMeshHookVaryings(hook0, "<warm>", hook, unusedErr)) return;
    static const std::regex kVertex(R"(\bfn\s+vertex\s*\()");
    static const std::regex kFragment(R"(\bfn\s+fragment\s*\()");
    static const std::regex kLight(R"(\bfn\s+light\s*\()");
    const bool hasVertex = std::regex_search(hook, kVertex);
    const bool hasLight = std::regex_search(hook, kLight);
    if (hasVertex) {
        meshStage(vertexEntryForTarget(target), hook, modules, knobs, false);
        const ShadowCaster caster = target == SceneRenderer::CustomShaderTarget::Skinned     ? ShadowCaster::Skinned
                                  : target == SceneRenderer::CustomShaderTarget::Instanced ? ShadowCaster::Instanced
                                                                                           : ShadowCaster::Static;
        for (const bool cutout : {false, true}) shadowStage(shadowEntry(caster, cutout), hook, modules, knobs);
    }
    if (std::regex_search(hook, kFragment)) {
        for (const bool indirect : {false, true})
            for (const bool cuts : {false, true}) meshStage(fragmentEntry(hasLight, indirect, cuts), hook, modules, knobs, false);
    }
}

} // namespace bro::scene::vk
