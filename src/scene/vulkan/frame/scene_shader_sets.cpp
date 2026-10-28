#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace bro::scene::vk {

namespace {

uint64_t hashWords(const uint32_t* w, size_t n) {
    uint64_t h = 1469598103934665603ull ^ n;
    for (size_t i = 0; i < n; ++i) h = (h ^ w[i]) * 1099511628211ull;
    return h;
}

struct PipelineRecord {
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::optional<SceneVkPipelineBuilder> builder;   // graphics; compute when empty
    std::string name;                               // compute: its stats label
    std::vector<std::shared_ptr<ShaderRecipe>> stages;
    VkPipeline twin = VK_NULL_HANDLE;
};

// One lock for all of it (recursive: a twin's build makes modules and
// compiles, which note themselves). Binds take it only while B is active.
std::recursive_mutex& lock() {
    static std::recursive_mutex m;
    return m;
}
std::unordered_map<uint64_t, std::shared_ptr<ShaderRecipe>>& recipes() {
    static std::unordered_map<uint64_t, std::shared_ptr<ShaderRecipe>> m;
    return m;
}
std::unordered_map<VkShaderModule, std::shared_ptr<ShaderRecipe>>& modules() {
    static std::unordered_map<VkShaderModule, std::shared_ptr<ShaderRecipe>> m;
    return m;
}
std::unordered_map<VkPipeline, PipelineRecord>& pipelines() {
    static std::unordered_map<VkPipeline, PipelineRecord> m;
    return m;
}

std::atomic<int> s_active{0};
uint64_t s_generation = 0;     // set B's load, counted from 1; 0 = none loaded
std::string s_dirB;
std::vector<std::string> s_definesB;
thread_local bool t_compilingB = false;
thread_local bool t_buildingTwin = false;

// Set B's SPIR-V of a recipe, made once a load.
void compileB(ShaderRecipe& r) {
    if (r.bGeneration == s_generation) return;
    r.bGeneration = s_generation;
    r.bWords.clear();
    r.bError.clear();
    std::string err;
    t_compilingB = true;
    switch (r.kind) {
        case ShaderRecipe::Kind::Words: break;
        case ShaderRecipe::Kind::Builtin:
            r.bWords = SceneVkShaderCompiler::compileBuiltinFromSource(r.builtin, s_dirB, &err);
            break;
        case ShaderRecipe::Kind::BuiltinWith:
            r.bWords = SceneVkShaderCompiler::compileBuiltinWith(r.builtin, r.overrides, &err);
            break;
        case ShaderRecipe::Kind::Glsl:
            r.bWords = SceneVkShaderCompiler::compileGlsl(r.glsl, r.stage, &err, r.overrides);
            break;
    }
    t_compilingB = false;
    if (r.kind != ShaderRecipe::Kind::Words && r.bWords.empty()) {
        r.bError = err.empty() ? std::string("did not compile") : err;
        LOG_ERROR("Shader set B: %s did not compile from %s; it keeps set A's:\n%s",
                  r.kind == ShaderRecipe::Kind::Glsl ? "a run-time shader" : SceneVkShaderCompiler::builtinName(r.builtin),
                  s_dirB.c_str(), r.bError.c_str());
    }
    r.bDiffers = !r.bWords.empty() && r.bWords != r.words;
}

std::string recipeName(const ShaderRecipe& r) {
    switch (r.kind) {
        case ShaderRecipe::Kind::Builtin: return SceneVkShaderCompiler::builtinName(r.builtin);
        case ShaderRecipe::Kind::BuiltinWith: return std::string(SceneVkShaderCompiler::builtinName(r.builtin)) + " (with modules)";
        case ShaderRecipe::Kind::Glsl: return "run-time GLSL";
        default: return "SPIR-V";
    }
}

// The record's twin from set B's SPIR-V; none when no stage differs.
void buildTwin(PipelineRecord& rec) {
    bool differs = false;
    for (auto& st : rec.stages) {
        compileB(*st);
        differs = differs || st->bDiffers;
    }
    if (!differs) return;
    std::vector<VkShaderModule> made;
    for (auto& st : rec.stages)
        made.push_back(SceneVkShaderModule::create(rec.device, st->bDiffers ? st->bWords : st->words));
    t_buildingTwin = true;
    bool ok = true;
    for (VkShaderModule m : made) ok = ok && m != VK_NULL_HANDLE;
    if (ok && rec.builder) {
        SceneVkPipelineBuilder b = *rec.builder;
        for (size_t i = 0; i < made.size(); ++i) b.setStageModule(i, made[i]);
        rec.twin = b.build(rec.device, rec.layout);
    } else if (ok && !made.empty()) {
        rec.twin = createComputePipeline(rec.device, made[0], rec.layout, rec.name.c_str());
    }
    t_buildingTwin = false;
    for (VkShaderModule m : made) SceneVkShaderModule::destroy(rec.device, m);
}

void destroyTwins() {
    auto& ps = pipelines();
    std::unordered_set<VkDevice> idle;
    for (auto& [p, rec] : ps) {
        if (!rec.twin) continue;
        // (Replacing the whole set is a teardown of the twins: the frames
        // that bound them may still be on the GPU.)
        if (idle.insert(rec.device).second) vkDeviceWaitIdle(rec.device);
        vkDestroyPipeline(rec.device, rec.twin, nullptr);
        rec.twin = VK_NULL_HANDLE;
    }
}

std::shared_ptr<ShaderRecipe> recipeOf(VkShaderModule m) {
    auto it = modules().find(m);
    return it == modules().end() ? nullptr : it->second;
}

} // namespace

namespace shader_sets {

void noteSpirv(std::shared_ptr<ShaderRecipe> recipe) {
    if (t_compilingB || !recipe || recipe->words.empty()) return;
    std::lock_guard<std::recursive_mutex> g(lock());
    recipes()[hashWords(recipe->words.data(), recipe->words.size())] = std::move(recipe);
}

bool compilingSetB() { return t_compilingB; }

const std::string& setBSourceDir() {
    static const std::string s_none;
    return t_compilingB ? s_dirB : s_none;
}

const std::vector<std::string>& setBDefines() {
    static const std::vector<std::string> s_none;
    return t_compilingB ? s_definesB : s_none;
}

void noteModule(VkShaderModule module, const uint32_t* code, size_t sizeBytes) {
    if (!module || !code) return;
    const size_t n = sizeBytes / sizeof(uint32_t);
    std::lock_guard<std::recursive_mutex> g(lock());
    auto it = recipes().find(hashWords(code, n));
    std::shared_ptr<ShaderRecipe> r;
    if (it != recipes().end() && it->second->words.size() == n &&
        std::equal(code, code + n, it->second->words.begin())) {
        r = it->second;
    } else {
        // Not one the compiler made: kept as it is, the same under B.
        r = std::make_shared<ShaderRecipe>();
        r->words.assign(code, code + n);
    }
    modules()[module] = std::move(r);
}

void forgetModule(VkShaderModule module) {
    std::lock_guard<std::recursive_mutex> g(lock());
    modules().erase(module);
}

void noteGraphicsPipeline(VkDevice device, VkPipelineLayout layout, const SceneVkPipelineBuilder& builder,
                          VkPipeline pipeline) {
    if (t_buildingTwin || !pipeline) return;
    std::lock_guard<std::recursive_mutex> g(lock());
    PipelineRecord rec;
    rec.device = device;
    rec.layout = layout;
    rec.builder = builder;
    for (const auto& st : builder.stages()) {
        auto r = recipeOf(st.module);
        if (!r) return;   // (a module not made by SceneVkShaderModule: no twin)
        rec.stages.push_back(std::move(r));
    }
    if (s_generation) buildTwin(rec);
    pipelines()[pipeline] = std::move(rec);
}

void noteComputePipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule module, const char* name,
                         VkPipeline pipeline) {
    if (t_buildingTwin || !pipeline) return;
    std::lock_guard<std::recursive_mutex> g(lock());
    auto r = recipeOf(module);
    if (!r) return;
    PipelineRecord rec;
    rec.device = device;
    rec.layout = layout;
    rec.name = name ? name : "";
    rec.stages.push_back(std::move(r));
    if (s_generation) buildTwin(rec);
    pipelines()[pipeline] = std::move(rec);
}

} // namespace shader_sets

bool loadShaderSetB(const std::string& dirIn, ShaderSetInfo* info, std::string* error,
                    const std::vector<std::string>& defines) {
    for (const std::string& d : defines) {
        const bool ident = !d.empty() && !std::isdigit(static_cast<unsigned char>(d[0])) &&
                           std::all_of(d.begin(), d.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        if (!ident) {
            if (error) *error = "shader set: define '" + d + "' is not an identifier";
            return false;
        }
    }
    std::string dir = dirIn;
    for (char& c : dir)
        if (c == '\\') c = '/';
    if (!dir.empty() && dir.back() != '/') dir += '/';
    if (dir.empty() || !std::ifstream(dir + "builtin_shaders.def")) {
        if (error) *error = "shader set: " + dirIn + " holds no builtin_shaders.def";
        return false;
    }
    std::lock_guard<std::recursive_mutex> g(lock());
    destroyTwins();
    s_dirB = dir;
    s_definesB = defines;
    ++s_generation;
    std::string named;
    for (const std::string& d : defines) named += " " + d;
    LOG_INFO("Shader set B: %s%s%s", dir.c_str(), named.empty() ? "" : ", defining", named.c_str());
    for (auto& [p, rec] : pipelines()) buildTwin(rec);
    if (info) *info = shaderSetInfo();
    return true;
}

std::atomic<uint64_t> s_bakeEpoch{0};

bool useShaderSet(int set, bool rebake) {
    if (set != 0 && set != 1) return false;
    std::lock_guard<std::recursive_mutex> g(lock());
    if (set == 1 && !s_generation) return false;
    s_active.store(set, std::memory_order_relaxed);
    if (rebake) s_bakeEpoch.fetch_add(1, std::memory_order_relaxed);
    return true;
}

int activeShaderSet() { return s_active.load(std::memory_order_relaxed); }

uint64_t shaderSetBakeEpoch() { return s_bakeEpoch.load(std::memory_order_relaxed); }

ShaderSetInfo shaderSetInfo() {
    std::lock_guard<std::recursive_mutex> g(lock());
    ShaderSetInfo info;
    info.dir = s_dirB;
    std::unordered_set<const ShaderRecipe*> seen;
    for (auto& [p, rec] : pipelines()) {
        ++info.pipelines;
        if (rec.twin) ++info.twinned;
        for (auto& st : rec.stages) {
            if (st->kind == ShaderRecipe::Kind::Words || !seen.insert(st.get()).second) continue;
            ++info.shaders;
            if (st->bGeneration == s_generation && s_generation) {
                if (st->bDiffers) ++info.differing;
                if (!st->bError.empty()) info.failed.push_back(recipeName(*st));
            }
        }
    }
    return info;
}

void bindScenePipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (s_active.load(std::memory_order_relaxed) == 1) {
        std::lock_guard<std::recursive_mutex> g(lock());
        auto it = pipelines().find(pipeline);
        if (it != pipelines().end() && it->second.twin) pipeline = it->second.twin;
    }
    vkCmdBindPipeline(cmd, point, pipeline);
}

void destroyScenePipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    if (!pipeline) return;
    {
        std::lock_guard<std::recursive_mutex> g(lock());
        auto it = pipelines().find(pipeline);
        if (it != pipelines().end()) {
            if (it->second.twin) vkDestroyPipeline(device, it->second.twin, allocator);
            pipelines().erase(it);
        }
    }
    vkDestroyPipeline(device, pipeline, allocator);
}

} // namespace bro::scene::vk
