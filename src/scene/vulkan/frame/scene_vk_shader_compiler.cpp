#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "render/glsl_compiler.h"
#include "util/log.h"

#include "scene_camera.glsl.src.h"
#include "scene_lighting.glsl.src.h"
#include "scene_atmosphere.glsl.src.h"
#include "scene_atmosphere_params.glsl.src.h"
#include "scene_probe_grid.glsl.src.h"
#include "probe_grid_layout.h.src.h"
#include "scene_mesh_push.glsl.src.h"
#include "scene_shadow_push.glsl.src.h"
#include "scene_shadow_pages.glsl.src.h"
#include "shadow_pages_layout.h.src.h"
#include "scene_shadow_cutout.glsl.src.h"
#include "scene_vegetation.glsl.src.h"
#include "scene_cover_mask.glsl.src.h"
#include "scene_subpixel_mark.glsl.src.h"
#include "scene_procedural.glsl.src.h"
#include "scene_bark.glsl.src.h"
#include "scene_grass.glsl.src.h"
#include "scene_ground_noise.glsl.src.h"
#include "scene_sward_layer.glsl.src.h"
#include "sward_module.glsl.src.h"
#include "scene_ground.glsl.src.h"
#include "scene_ground_layers.glsl.src.h"
#include "scene_cover_plan.glsl.src.h"
#include "scene_cover_far.glsl.src.h"
#include "scene_herb.glsl.src.h"
#include "scene_herb_layer.glsl.src.h"
#include "herb_module.glsl.src.h"
#include "mesh.frag.src.h"
#include "cover_plan.comp.src.h"
#include "mesh_grass.vert.src.h"
#include "mesh_herb.vert.src.h"
#include "mesh_cover_far.vert.src.h"
#include "shadow_grass.vert.src.h"
#include "ground_layer_fill.frag.src.h"
#include "ground_chart_fill.vert.src.h"
#include "scene_plants.glsl.src.h"
#include "scene_plant_records.glsl.src.h"
#include "scene_plant_layer.glsl.src.h"
#include "plant_module.glsl.src.h"
#include "plants.vert.src.h"
#include "plants.frag.src.h"
#include "shadow_plants.vert.src.h"
#include "plants_pose.comp.src.h"
#include "scene_land_layout.h.src.h"
#include "land_sun_layout.h.src.h"
#include "scene_land.glsl.src.h"
#include "scene_land_layer.glsl.src.h"
#include "land_module.glsl.src.h"
#include "land_canopy.glsl.src.h"
#include "land_body.glsl.src.h"
#include "land_fill.comp.src.h"
#include "land_grid.glsl.src.h"
#include "land_grid_config.glsl.src.h"
#include "land_process_layout.h.src.h"
#include "land_process_layer.glsl.src.h"
#include "land_process_module.glsl.src.h"
#include "land_process_dispatch.glsl.src.h"
#include "land_process.comp.src.h"
#include "clouds_layout.h.src.h"
#include "scene_cloud_layer.glsl.src.h"
#include "cloud_module.glsl.src.h"
#include "clouds.frag.src.h"
#include "clouds_cover.comp.src.h"
#include "clouds_shadow.comp.src.h"
#include "util/user_dirs.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef BRO_SCENE_SHADER_SOURCE_DIR
#define BRO_SCENE_SHADER_SOURCE_DIR ""
#endif

// The build's SPIR-V, one generated translation unit a shader
// (bro_spirv_embed --cpp, from builtin_shaders.def).
namespace bro::scene::vk::spv {
#define BRO_SCENE_SHADER(name, out, source, stage, defines) \
    extern const uint32_t kSpv##name[];                     \
    extern const size_t kSpv##name##Words;
#include "scene/vulkan/shaders/builtin_shaders.def"
#undef BRO_SCENE_SHADER
} // namespace bro::scene::vk::spv

namespace bro::scene::vk {

namespace {

constexpr size_t kBuiltinCount = static_cast<size_t>(BuiltinSceneShader::Count);

struct BuiltinInfo {
    const char* name;
    const char* source;
    const char* stage;
    const char* defines;
    const uint32_t* words;
    const size_t* count;
};

const BuiltinInfo kBuiltins[] = {
#define BRO_SCENE_SHADER(name, out, source, stage, defines) \
    {#name, source, stage, defines, spv::kSpv##name, &spv::kSpv##name##Words},
#include "scene/vulkan/shaders/builtin_shaders.def"
#undef BRO_SCENE_SHADER
};
static_assert(sizeof(kBuiltins) / sizeof(kBuiltins[0]) == kBuiltinCount);

const BuiltinInfo* infoOf(BuiltinSceneShader shader) {
    const size_t i = static_cast<size_t>(shader);
    return i < kBuiltinCount ? &kBuiltins[i] : nullptr;
}

std::string resolveSourceDir() {
    const char* env = std::getenv("BRO_SCENE_SHADER_SOURCE");
    if (!env || !*env || std::strcmp(env, "0") == 0) return {};
    std::string dir = std::strcmp(env, "1") == 0 ? std::string(BRO_SCENE_SHADER_SOURCE_DIR) : std::string(env);
    if (dir.empty()) {
        LOG_ERROR("BRO_SCENE_SHADER_SOURCE=1, but this build does not know its shader source tree");
        return {};
    }
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    if (!std::ifstream(dir + "builtin_shaders.def")) {
        LOG_ERROR("BRO_SCENE_SHADER_SOURCE: %s holds no builtin_shaders.def; using the embedded shaders", dir.c_str());
        return {};
    }
    LOG_INFO("Scene shaders: compiling from %s (BRO_SCENE_SHADER_SOURCE)", dir.c_str());
    return dir;
}

// The GLSL a run-time scene shader may #include, by its path under
// vulkan/shaders (as the built-in shaders name their includes).
const char* embeddedInclude(const std::string& name) {
    static const struct { const char* name; const char* source; } kIncludes[] = {
        {"common/scene_camera.glsl", kVkSceneCameraSrc},
        {"common/scene_lighting.glsl", kVkSceneLightingSrc},
        {"common/scene_atmosphere.glsl", kVkSceneAtmosphereSrc},
        {"common/scene_atmosphere_params.glsl", kVkSceneAtmosphereParamsSrc},
        {"lighting/scene_probe_grid.glsl", kVkSceneProbeGridSrc},
        {"lighting/probe_grid_layout.h", kVkProbeGridLayoutSrc},
        {"common/scene_mesh_push.glsl", kVkSceneMeshPushSrc},
        {"common/scene_shadow_push.glsl", kVkSceneShadowPushSrc},
        {"common/scene_shadow_pages.glsl", kVkSceneShadowPagesSrc},
        {"shadow/shadow_pages_layout.h", kVkShadowPagesLayoutSrc},
        {"shadow/scene_shadow_cutout.glsl", kVkSceneShadowCutoutSrc},
        {"common/scene_vegetation.glsl", kVkSceneVegetationSrc},
        {"common/scene_cover_mask.glsl", kVkSceneCoverMaskSrc},
        {"common/scene_subpixel_mark.glsl", kVkSceneSubpixelMarkSrc},
        {"common/scene_procedural.glsl", kVkSceneProceduralSrc},
        {"draw/scene_bark.glsl", kVkSceneBarkSrc},
        {"ground/scene_grass.glsl", kVkSceneGrassSrc},
        {"ground/scene_ground_noise.glsl", kVkSceneGroundNoiseSrc},
        {"ground/scene_sward_layer.glsl", kVkSceneSwardLayerSrc},
        {"ground/sward_module.glsl", kVkSwardModuleSrc},
        {"ground/scene_ground.glsl", kVkSceneGroundSrc},
        {"ground/scene_ground_layers.glsl", kVkSceneGroundLayersSrc},
        {"ground/scene_cover_plan.glsl", kVkSceneCoverPlanSrc},
        {"ground/scene_cover_far.glsl", kVkSceneCoverFarSrc},
        {"ground/scene_herb.glsl", kVkSceneHerbSrc},
        {"ground/scene_herb_layer.glsl", kVkSceneHerbLayerSrc},
        {"ground/herb_module.glsl", kVkHerbModuleSrc},
        {"foliage/scene_plants.glsl", kVkScenePlantsSrc},
        {"foliage/scene_plant_records.glsl", kVkScenePlantRecordsSrc},
        {"foliage/scene_plant_layer.glsl", kVkScenePlantLayerSrc},
        {"foliage/plant_module.glsl", kVkPlantModuleSrc},
        {"ground/scene_land_layout.h", kVkSceneLandLayoutSrc},
        {"ground/land_sun_layout.h", kVkLandSunLayoutSrc},
        {"ground/scene_land.glsl", kVkSceneLandSrc},
        {"ground/scene_land_layer.glsl", kVkSceneLandLayerSrc},
        {"ground/land_module.glsl", kVkLandModuleSrc},
        {"ground/land_canopy.glsl", kVkLandCanopySrc},
        {"ground/land_body.glsl", kVkLandBodySrc},
        {"ground/land_grid.glsl", kVkLandGridSrc},
        {"ground/land_grid_config.glsl", kVkLandGridConfigSrc},
        {"ground/land_process_layout.h", kVkLandProcessLayoutSrc},
        {"ground/land_process_layer.glsl", kVkLandProcessLayerSrc},
        {"ground/land_process_module.glsl", kVkLandProcessModuleSrc},
        {"ground/land_process_dispatch.glsl", kVkLandProcessDispatchSrc},
        {"sky/clouds_layout.h", kVkCloudsLayoutSrc},
        {"sky/scene_cloud_layer.glsl", kVkSceneCloudLayerSrc},
        {"sky/cloud_module.glsl", kVkCloudModuleSrc},
        // The built-in shaders compileBuiltinWith builds.
        {"draw/mesh.frag", kVkMeshFragSrc},
        {"ground/cover_plan.comp", kVkCoverPlanCompSrc},
        {"ground/mesh_grass.vert", kVkMeshGrassVertSrc},
        {"ground/mesh_herb.vert", kVkMeshHerbVertSrc},
        {"ground/mesh_cover_far.vert", kVkMeshCoverFarVertSrc},
        {"ground/shadow_grass.vert", kVkShadowGrassVertSrc},
        {"ground/ground_layer_fill.frag", kVkGroundLayerFillFragSrc},
        {"ground/ground_chart_fill.vert", kVkGroundChartFillVertSrc},
        {"foliage/plants.vert", kVkPlantsVertSrc},
        {"foliage/plants.frag", kVkPlantsFragSrc},
        {"foliage/shadow_plants.vert", kVkShadowPlantsVertSrc},
        {"foliage/plants_pose.comp", kVkPlantsPoseCompSrc},
        {"ground/land_fill.comp", kVkLandFillCompSrc},
        {"ground/land_process.comp", kVkLandProcessCompSrc},
        {"sky/clouds.frag", kVkCloudsFragSrc},
        {"sky/clouds_cover.comp", kVkCloudsCoverCompSrc},
        {"sky/clouds_shadow.comp", kVkCloudsShadowCompSrc},
    };
    for (const auto& inc : kIncludes) {
        if (name == inc.name) return inc.source;
    }
    return nullptr;
}

// Replace each `#include "name"` line with the named source (recursively), as
// bro_spirv_embed does for the build-time compiles. False on an unknown name.
// An override names its include in place of the embedded file.
bool expandIncludes(const std::string& source, std::string& out, std::string* errOut,
                    const SceneVkShaderCompiler::IncludeOverrides& overrides, int depth = 0) {
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t hash = line.find_first_not_of(" \t");
        if (hash != std::string::npos && line.compare(hash, 8, "#include") == 0) {
            const size_t open = line.find('"', hash);
            const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
            const std::string name = close == std::string::npos ? std::string() : line.substr(open + 1, close - open - 1);
            const std::string* replaced = nullptr;
            for (const auto& o : overrides)
                if (o.first == name) replaced = &o.second;
            const char* embedded = name.empty() || replaced ? nullptr : embeddedInclude(name);
            const std::string included = replaced ? *replaced
                                       : embedded ? SceneVkShaderCompiler::shaderSource(name.c_str(), embedded)
                                                  : std::string();
            if ((!embedded && !replaced) || depth > 8 || !expandIncludes(included, out, errOut, overrides, depth + 1)) {
                if (errOut && errOut->empty()) *errOut = "unknown or recursive include: " + line;
                return false;
            }
            continue;
        }
        out += line;
        out += '\n';
    }
    return true;
}

// The SPIR-V cache (docs/code-cache.md): a file a source under
// <user cache dir>/spirv-cache, named by two 64-bit FNV-1a hashes of the
// stage and the expanded source (different offsets, so 128 bits).
const std::string& spirvCacheDir() {
    static const std::string s_dir = [] {
        if (const char* off = std::getenv("BRO_SPIRV_CACHE"); off && std::strcmp(off, "0") == 0) return std::string();
        const std::string base = util::userCacheDir();
        return base.empty() ? std::string() : base + "/spirv-cache/";
    }();
    return s_dir;
}

std::string spirvCachePath(const std::string& source, VkShaderStageFlagBits stage) {
    const std::string& dir = spirvCacheDir();
    if (dir.empty()) return {};
    uint64_t a = 1469598103934665603ull ^ uint64_t(stage), b = 0x9e3779b97f4a7c15ull ^ uint64_t(stage);
    for (unsigned char c : source) {
        a = (a ^ c) * 1099511628211ull;
        b = (b ^ c) * 0x100000001b3ull + 0x51ed27ull;
    }
    char name[40];
    std::snprintf(name, sizeof name, "%016llx%016llx.spv", (unsigned long long)a, (unsigned long long)b);
    return dir + name;
}

std::vector<uint32_t> readSpirvCache(const std::string& path) {
    std::vector<uint32_t> words;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return words;
    const std::streamsize size = in.tellg();
    if (size < 20 || size % 4 != 0) return words;
    words.resize(size_t(size) / 4);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(words.data()), size) || words[0] != 0x07230203u) words.clear();
    return words;
}

// Written to a file of its own and renamed into place, so a reader (another
// process, another thread) sees the whole of it or nothing.
void writeSpirvCache(const std::string& path, const std::vector<uint32_t>& words) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned> s_serial{0};
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()))
                          + "." + std::to_string(s_serial++);
    bool written = false;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
        written = static_cast<bool>(out);
    }
    if (written) fs::rename(tmp, path, ec);
    if (!written || ec) fs::remove(tmp, ec);
}

} // namespace

std::vector<uint32_t> SceneVkShaderCompiler::compileGlsl(const std::string& glslSource,
                                                        VkShaderStageFlagBits stage,
                                                        std::string* errOut,
                                                        const IncludeOverrides& overrides) {
    render::ShaderStage glStage;
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:   glStage = render::ShaderStage::Vertex; break;
        case VK_SHADER_STAGE_FRAGMENT_BIT: glStage = render::ShaderStage::Fragment; break;
        case VK_SHADER_STAGE_COMPUTE_BIT:  glStage = render::ShaderStage::Compute; break;
        default:
            if (errOut) *errOut = "unsupported shader stage";
            LOG_ERROR("SceneVkShaderCompiler: unsupported shader stage 0x%x", unsigned(stage));
            return {};
    }
    std::string log;
    std::string source;
    if (!expandIncludes(glslSource, source, &log, overrides)) {
        LOG_ERROR("SceneVkShaderCompiler: %s", log.c_str());
        if (errOut) *errOut = std::move(log);
        return {};
    }
    const std::string cachePath = spirvCachePath(source, stage);
    if (!cachePath.empty()) {
        std::vector<uint32_t> cached = readSpirvCache(cachePath);
        if (!cached.empty()) {
            if (errOut) errOut->clear();
            return cached;
        }
    }
    std::vector<uint32_t> spirv = render::compileGlslToSpirv(source, glStage, &log);
    if (spirv.empty()) LOG_ERROR("SceneVkShaderCompiler: GLSL compilation failed:\n%s", log.c_str());
    else if (!cachePath.empty()) writeSpirvCache(cachePath, spirv);
    if (errOut) *errOut = std::move(log);
    return spirv;
}

std::vector<uint32_t> SceneVkShaderCompiler::compileBuiltinWith(BuiltinSceneShader shader,
                                                               const IncludeOverrides& overrides,
                                                               std::string* errOut) {
    const BuiltinInfo* info = infoOf(shader);
    const char* embedded = info ? embeddedInclude(info->source) : nullptr;
    if (!embedded) {
        if (errOut) *errOut = std::string("no embedded source for ") + builtinName(shader);
        return {};
    }
    std::string text = shaderSource(info->source, embedded);
    std::string defines;
    std::istringstream words(info->defines);
    for (std::string d; words >> d;) defines += "#define " + d + " 1\n";
    const size_t version = text.find("#version");
    const size_t lineEnd = version == std::string::npos ? std::string::npos : text.find('\n', version);
    if (lineEnd == std::string::npos) {
        if (errOut) *errOut = std::string(info->source) + " has no #version line";
        return {};
    }
    text.insert(lineEnd + 1, defines);
    const VkShaderStageFlagBits stage = std::strcmp(info->stage, "vert") == 0   ? VK_SHADER_STAGE_VERTEX_BIT
                                        : std::strcmp(info->stage, "frag") == 0 ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                                                : VK_SHADER_STAGE_COMPUTE_BIT;
    return compileGlsl(text, stage, errOut, overrides);
}

const std::string& SceneVkShaderCompiler::shaderSourceDir() {
    static const std::string s_dir = resolveSourceDir();
    return s_dir;
}

const char* SceneVkShaderCompiler::buildShaderSourceDir() {
    return BRO_SCENE_SHADER_SOURCE_DIR;
}

std::string SceneVkShaderCompiler::shaderSource(const char* fileName, const char* embedded) {
    const std::string& dir = shaderSourceDir();
    if (!dir.empty()) {
        std::ifstream in(dir + fileName, std::ios::binary);
        if (in) {
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }
    }
    return embedded ? std::string(embedded) : std::string();
}

const char* SceneVkShaderCompiler::builtinName(BuiltinSceneShader shader) {
    const BuiltinInfo* info = infoOf(shader);
    return info ? info->name : "";
}

const std::vector<uint32_t>& SceneVkShaderCompiler::embeddedSpirv(BuiltinSceneShader shader) {
    static const auto s_table = [] {
        std::array<std::vector<uint32_t>, kBuiltinCount> table;
        for (size_t i = 0; i < kBuiltinCount; ++i)
            table[i].assign(kBuiltins[i].words, kBuiltins[i].words + *kBuiltins[i].count);
        return table;
    }();
    static const std::vector<uint32_t> s_empty;
    const size_t i = static_cast<size_t>(shader);
    return i < kBuiltinCount ? s_table[i] : s_empty;
}

std::vector<uint32_t> SceneVkShaderCompiler::compileBuiltinFromSource(BuiltinSceneShader shader,
                                                                      const std::string& dir,
                                                                      std::string* errOut) {
    const BuiltinInfo* info = infoOf(shader);
    if (!info) {
        if (errOut) *errOut = "not a built-in shader";
        return {};
    }
    std::string path = dir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += info->source;
    std::vector<std::string> defines;
    std::istringstream words(info->defines);
    for (std::string d; words >> d;) defines.push_back(d);
    std::string text, err;
    if (!render::preprocessGlslFile(path, defines, text, nullptr, &err, dir)) {
        if (errOut) *errOut = std::move(err);
        return {};
    }
    const render::ShaderStage stage = std::strcmp(info->stage, "vert") == 0   ? render::ShaderStage::Vertex
                                      : std::strcmp(info->stage, "frag") == 0 ? render::ShaderStage::Fragment
                                                                              : render::ShaderStage::Compute;
    std::vector<uint32_t> spirv = render::compileGlslToSpirv(text, stage, &err);
    if (spirv.empty() && errOut) *errOut = std::move(err);
    return spirv;
}

const std::vector<uint32_t>& SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader shader) {
    const std::string& dir = shaderSourceDir();
    if (dir.empty() || !infoOf(shader)) return embeddedSpirv(shader);

    // Compiled at first use, kept for the life of the process.
    static std::mutex s_mutex;
    static std::array<std::unique_ptr<std::vector<uint32_t>>, kBuiltinCount> s_compiled;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto& slot = s_compiled[static_cast<size_t>(shader)];
    if (!slot) {
        std::string err;
        auto spirv = compileBuiltinFromSource(shader, dir, &err);
        if (spirv.empty()) {
            LOG_ERROR("Scene shader %s (%s) did not compile from %s; using the embedded SPIR-V:\n%s",
                      builtinName(shader), infoOf(shader)->source, dir.c_str(), err.c_str());
            slot = std::make_unique<std::vector<uint32_t>>(embeddedSpirv(shader));
        } else {
            slot = std::make_unique<std::vector<uint32_t>>(std::move(spirv));
        }
    }
    return *slot;
}

VkShaderModule SceneVkShaderCompiler::createBuiltinModule(VkDevice device, BuiltinSceneShader shader) {
    const auto& spirv = getBuiltinSpirv(shader);
    if (spirv.empty()) return VK_NULL_HANDLE;
    return createModule(device, spirv);
}

VkShaderModule SceneVkShaderCompiler::createModule(VkDevice device, const uint32_t* code, size_t sizeBytes) {
    return SceneVkShaderModule::create(device, code, sizeBytes);
}

VkShaderModule SceneVkShaderCompiler::createModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    return SceneVkShaderModule::create(device, spirv);
}

void SceneVkShaderCompiler::destroyModule(VkDevice device, VkShaderModule module) {
    SceneVkShaderModule::destroy(device, module);
}

} // namespace bro::scene::vk
