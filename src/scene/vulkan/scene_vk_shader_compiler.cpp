#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "render/glsl_compiler.h"
#include "util/log.h"

#include "scene_camera.glsl.src.h"
#include "scene_lighting.glsl.src.h"
#include "scene_atmosphere.glsl.src.h"
#include "scene_mesh_push.glsl.src.h"
#include "scene_shadow_push.glsl.src.h"
#include "scene_vegetation.glsl.src.h"
#include "scene_forest_sprite.glsl.src.h"
#include "scene_forest_mass.glsl.src.h"
#include "scene_procedural.glsl.src.h"
#include "scene_forest_layout.h.src.h"
#include "scene_forest_crown.glsl.src.h"
#include "scene_bark.glsl.src.h"
#include "scene_forest_spray.glsl.src.h"
#include "scene_forest_bloom.glsl.src.h"
#include "scene_grass.glsl.src.h"
#include "scene_grass_surface.glsl.src.h"
#include "scene_forest_surface.glsl.src.h"
#include "scene_ground.glsl.src.h"
#include "scene_ground_layers.glsl.src.h"
#include "scene_herb_mix.glsl.src.h"
#include "scene_herb_surface.glsl.src.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

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

// The GLSL a run-time scene shader may #include, by name.
const char* embeddedInclude(const std::string& name) {
    static const struct { const char* name; const char* source; } kIncludes[] = {
        {"scene_camera.glsl", kVkSceneCameraSrc},
        {"scene_lighting.glsl", kVkSceneLightingSrc},
        {"scene_atmosphere.glsl", kVkSceneAtmosphereSrc},
        {"scene_mesh_push.glsl", kVkSceneMeshPushSrc},
        {"scene_shadow_push.glsl", kVkSceneShadowPushSrc},
        {"scene_vegetation.glsl", kVkSceneVegetationSrc},
        {"scene_forest_sprite.glsl", kVkSceneForestSpriteSrc},
        {"scene_forest_mass.glsl", kVkSceneForestMassSrc},
        // mesh.frag's forest-sprite half; spliced in whole, compiled out.
        {"scene_procedural.glsl", kVkSceneProceduralSrc},
        {"scene_forest_layout.h", kVkSceneForestLayoutSrc},
        {"scene_forest_crown.glsl", kVkSceneForestCrownSrc},
        {"scene_bark.glsl", kVkSceneBarkSrc},
        {"scene_forest_spray.glsl", kVkSceneForestSpraySrc},
        {"scene_forest_bloom.glsl", kVkSceneForestBloomSrc},
        {"scene_grass.glsl", kVkSceneGrassSrc},
        {"scene_grass_surface.glsl", kVkSceneGrassSurfaceSrc},
        {"scene_forest_surface.glsl", kVkSceneForestSurfaceSrc},
        {"scene_ground.glsl", kVkSceneGroundSrc},
        {"scene_ground_layers.glsl", kVkSceneGroundLayersSrc},
        {"scene_herb_mix.glsl", kVkSceneHerbMixSrc},
        {"scene_herb_surface.glsl", kVkSceneHerbSurfaceSrc},
    };
    for (const auto& inc : kIncludes) {
        if (name == inc.name) return inc.source;
    }
    return nullptr;
}

// Replace each `#include "name"` line with the named source (recursively), as
// bro_spirv_embed does for the build-time compiles. False on an unknown name.
bool expandIncludes(const std::string& source, std::string& out, std::string* errOut, int depth = 0) {
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t hash = line.find_first_not_of(" \t");
        if (hash != std::string::npos && line.compare(hash, 8, "#include") == 0) {
            const size_t open = line.find('"', hash);
            const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
            const std::string name = close == std::string::npos ? std::string() : line.substr(open + 1, close - open - 1);
            const char* embedded = name.empty() ? nullptr : embeddedInclude(name);
            const std::string included = embedded ? SceneVkShaderCompiler::shaderSource(name.c_str(), embedded) : std::string();
            if (!embedded || depth > 8 || !expandIncludes(included, out, errOut, depth + 1)) {
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

} // namespace

std::vector<uint32_t> SceneVkShaderCompiler::compileGlsl(const std::string& glslSource,
                                                        VkShaderStageFlagBits stage,
                                                        std::string* errOut) {
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
    if (!expandIncludes(glslSource, source, &log)) {
        LOG_ERROR("SceneVkShaderCompiler: %s", log.c_str());
        if (errOut) *errOut = std::move(log);
        return {};
    }
    std::vector<uint32_t> spirv = render::compileGlslToSpirv(source, glStage, &log);
    if (spirv.empty()) LOG_ERROR("SceneVkShaderCompiler: GLSL compilation failed:\n%s", log.c_str());
    if (errOut) *errOut = std::move(log);
    return spirv;
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
    if (!render::preprocessGlslFile(path, defines, text, nullptr, &err)) {
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
