#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "util/log.h"

#include "util/subprocess.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#if BRO_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif


namespace bro::scene::vk {

namespace {

static const uint32_t kSpvMeshVert[] =
#include "mesh.vert.spv.h"
;
static const uint32_t kSpvMeshInstancedVert[] =
#include "mesh_instanced.vert.spv.h"
;
static const uint32_t kSpvMeshSkinnedVert[] =
#include "mesh_skinned.vert.spv.h"
;
static const uint32_t kSpvMeshFrag[] =
#include "mesh.frag.spv.h"
;
static const uint32_t kSpvShadowVert[] =
#include "shadow.vert.spv.h"
;
static const uint32_t kSpvShadowInstancedVert[] =
#include "shadow_instanced.vert.spv.h"
;
static const uint32_t kSpvShadowSkinnedVert[] =
#include "shadow_skinned.vert.spv.h"
;
static const uint32_t kSpvShadowFrag[] =
#include "shadow.frag.spv.h"
;
static const uint32_t kSpvSkyboxVert[] =
#include "skybox.vert.spv.h"
;
static const uint32_t kSpvEnvironmentFrag[] =
#include "environment.frag.spv.h"
;
static const uint32_t kSpvPostFxVert[] =
#include "postfx.vert.spv.h"
;
static const uint32_t kSpvTonemapFrag[] =
#include "tonemap.frag.spv.h"
;
static const uint32_t kSpvBloomFrag[] =
#include "bloom.frag.spv.h"
;
static const uint32_t kSpvFxaaFrag[] =
#include "fxaa.frag.spv.h"
;
static const uint32_t kSpvBillboardVert[] =
#include "billboard.vert.spv.h"
;
static const uint32_t kSpvBillboardFrag[] =
#include "billboard.frag.spv.h"
;
static const uint32_t kSpvParticlesVert[] =
#include "particles.vert.spv.h"
;
static const uint32_t kSpvParticlesFrag[] =
#include "particles.frag.spv.h"
;
static const uint32_t kSpvDecalVert[] =
#include "decal.vert.spv.h"
;
static const uint32_t kSpvDecalFrag[] =
#include "decal.frag.spv.h"
;
static const uint32_t kSpvBlurFrag[] =
#include "blur.frag.spv.h"
;
static const uint32_t kSpvColorLutFrag[] =
#include "color_lut.frag.spv.h"
;
static const uint32_t kSpvSsaoFrag[] =
#include "ssao.frag.spv.h"
;
static const uint32_t kSpvSsrFrag[] =
#include "ssr.frag.spv.h"
;
static const uint32_t kSpvDofFrag[] =
#include "dof.frag.spv.h"
;
static const uint32_t kSpvApplyAoFrag[] =
#include "apply_ao.frag.spv.h"
;
static const uint32_t kSpvGaussianSplatVert[] =
#include "gaussian_splat.vert.spv.h"
;
static const uint32_t kSpvGaussianSplatFrag[] =
#include "gaussian_splat.frag.spv.h"
;

std::mutex s_compilerMutex;

template<size_t N>
std::vector<uint32_t> arrayToVector(const uint32_t (&arr)[N]) {
    return std::vector<uint32_t>(arr, arr + N);
}

} // namespace

std::unordered_map<std::string, std::vector<uint32_t>> SceneVkShaderCompiler::s_compileCache;

bool SceneVkShaderCompiler::hasGlslc() {
#if BRO_HAS_SHADERC
    return true;
#else
    return util::hasExecutableOnPath("glslc");
#endif
}

std::vector<uint32_t> SceneVkShaderCompiler::compileGlsl(
    const std::string& glslSource,
    VkShaderStageFlagBits stage,
    const std::string& entryPoint,
    const std::vector<std::string>& defines,
    std::string* errOut) {

    std::lock_guard<std::mutex> lock(s_compilerMutex);

    std::string stageStr;
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:   stageStr = "vertex"; break;
        case VK_SHADER_STAGE_FRAGMENT_BIT: stageStr = "fragment"; break;
        case VK_SHADER_STAGE_COMPUTE_BIT:  stageStr = "compute"; break;
        default:
            LOG_ERROR("SceneVkShaderCompiler: Unsupported shader stage");
            return {};
    }

    std::ostringstream cacheKeyStream;
    cacheKeyStream << stageStr << "|" << entryPoint << "|";
    for (const auto& d : defines) cacheKeyStream << d << ";";
    cacheKeyStream << "|" << glslSource;
    std::string cacheKey = cacheKeyStream.str();

    auto it = s_compileCache.find(cacheKey);
    if (it != s_compileCache.end()) {
        return it->second;
    }

#if BRO_HAS_SHADERC
    shaderc::Compiler compiler;
    if (compiler.IsValid()) {
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
        for (const auto& d : defines) {
            size_t eqPos = d.find('=');
            if (eqPos != std::string::npos) {
                std::string name = d.substr(0, eqPos);
                std::string val = d.substr(eqPos + 1);
                options.AddMacroDefinition(name.c_str(), name.size(), val.c_str(), val.size());
            } else {
                options.AddMacroDefinition(d.c_str(), d.size(), nullptr, 0);
            }
        }

        shaderc_shader_kind kind = shaderc_glsl_infer_from_source;
        switch (stage) {
            case VK_SHADER_STAGE_VERTEX_BIT:   kind = shaderc_vertex_shader; break;
            case VK_SHADER_STAGE_FRAGMENT_BIT: kind = shaderc_fragment_shader; break;
            case VK_SHADER_STAGE_COMPUTE_BIT:  kind = shaderc_compute_shader; break;
            default: break;
        }

        const char* ep = entryPoint.empty() ? "main" : entryPoint.c_str();
        auto result = compiler.CompileGlslToSpv(glslSource.c_str(), glslSource.size(), kind, "source", ep, options);
        if (result.GetCompilationStatus() == shaderc_compilation_status_success) {
            std::vector<uint32_t> spirv(result.cbegin(), result.cend());
            if (!spirv.empty() && spirv[0] == 0x07230203) {
                s_compileCache[cacheKey] = spirv;
                return spirv;
            } else {
                LOG_ERROR("SceneVkShaderCompiler: Invalid SPIR-V generated by shaderc");
                if (errOut) *errOut = "Invalid SPIR-V generated by shaderc";
                return {};
            }
        } else {
            std::string errStr = result.GetErrorMessage();
            LOG_ERROR("SceneVkShaderCompiler: shaderc compilation failed: %s", errStr.c_str());
            if (errOut) *errOut = errStr;
            return {};
        }
    }
#endif

    if (!util::hasExecutableOnPath("glslc")) {
        LOG_ERROR("SceneVkShaderCompiler: glslc not found on system PATH");
        if (errOut) *errOut = "glslc not found on system PATH";
        return {};
    }

    std::vector<std::string> args = {
        "glslc",
        "-fshader-stage=" + stageStr,
        "-fentry-point=" + (entryPoint.empty() ? "main" : entryPoint)
    };
    for (const auto& d : defines) {
        args.push_back("-D" + d);
    }
    args.push_back("-");
    args.push_back("-o");
    args.push_back("-");

    auto res = util::runSubprocess(args, glslSource);
    if (res.success && res.stdOut.size() >= 4 && (res.stdOut.size() % 4 == 0)) {
        size_t wordCount = res.stdOut.size() / 4;
        std::vector<uint32_t> spirv(wordCount);
        std::memcpy(spirv.data(), res.stdOut.data(), res.stdOut.size());

        if (spirv[0] == 0x07230203) {
            s_compileCache[cacheKey] = spirv;
            return spirv;
        } else {
            LOG_ERROR("SceneVkShaderCompiler: Invalid SPIR-V magic number");
            if (errOut) *errOut = "Invalid SPIR-V magic number";
        }
    } else {
        LOG_ERROR("SceneVkShaderCompiler: glslc execution failed (status=%d): %s", res.exitCode, res.stdErr.c_str());
        if (errOut) *errOut = res.stdErr;
    }

    return {};
}

const std::vector<uint32_t>& SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader shader) {
    static const std::vector<uint32_t> s_meshVert = arrayToVector(kSpvMeshVert);
    static const std::vector<uint32_t> s_meshInstVert = arrayToVector(kSpvMeshInstancedVert);
    static const std::vector<uint32_t> s_meshSkinnedVert = arrayToVector(kSpvMeshSkinnedVert);
    static const std::vector<uint32_t> s_meshFrag = arrayToVector(kSpvMeshFrag);
    static const std::vector<uint32_t> s_shadowVert = arrayToVector(kSpvShadowVert);
    static const std::vector<uint32_t> s_shadowInstVert = arrayToVector(kSpvShadowInstancedVert);
    static const std::vector<uint32_t> s_shadowSkinnedVert = arrayToVector(kSpvShadowSkinnedVert);
    static const std::vector<uint32_t> s_shadowFrag = arrayToVector(kSpvShadowFrag);
    static const std::vector<uint32_t> s_skyboxVert = arrayToVector(kSpvSkyboxVert);
    static const std::vector<uint32_t> s_envFrag = arrayToVector(kSpvEnvironmentFrag);
    static const std::vector<uint32_t> s_postFxVert = arrayToVector(kSpvPostFxVert);
    static const std::vector<uint32_t> s_tonemapFrag = arrayToVector(kSpvTonemapFrag);
    static const std::vector<uint32_t> s_bloomFrag = arrayToVector(kSpvBloomFrag);
    static const std::vector<uint32_t> s_fxaaFrag = arrayToVector(kSpvFxaaFrag);
    static const std::vector<uint32_t> s_billboardVert = arrayToVector(kSpvBillboardVert);
    static const std::vector<uint32_t> s_billboardFrag = arrayToVector(kSpvBillboardFrag);
    static const std::vector<uint32_t> s_particlesVert = arrayToVector(kSpvParticlesVert);
    static const std::vector<uint32_t> s_particlesFrag = arrayToVector(kSpvParticlesFrag);
    static const std::vector<uint32_t> s_decalVert = arrayToVector(kSpvDecalVert);
    static const std::vector<uint32_t> s_decalFrag = arrayToVector(kSpvDecalFrag);
    static const std::vector<uint32_t> s_blurFrag = arrayToVector(kSpvBlurFrag);
    static const std::vector<uint32_t> s_colorLutFrag = arrayToVector(kSpvColorLutFrag);
    static const std::vector<uint32_t> s_ssaoFrag = arrayToVector(kSpvSsaoFrag);
    static const std::vector<uint32_t> s_ssrFrag = arrayToVector(kSpvSsrFrag);
    static const std::vector<uint32_t> s_dofFrag = arrayToVector(kSpvDofFrag);
    static const std::vector<uint32_t> s_applyAoFrag = arrayToVector(kSpvApplyAoFrag);
    static const std::vector<uint32_t> s_gaussianSplatVert = arrayToVector(kSpvGaussianSplatVert);
    static const std::vector<uint32_t> s_gaussianSplatFrag = arrayToVector(kSpvGaussianSplatFrag);
    static const std::vector<uint32_t> s_empty;

    switch (shader) {
        case BuiltinSceneShader::MeshVert:          return s_meshVert;
        case BuiltinSceneShader::MeshInstancedVert: return s_meshInstVert;
        case BuiltinSceneShader::MeshSkinnedVert:   return s_meshSkinnedVert;
        case BuiltinSceneShader::MeshFrag:          return s_meshFrag;
        case BuiltinSceneShader::ShadowVert:        return s_shadowVert;
        case BuiltinSceneShader::ShadowInstancedVert: return s_shadowInstVert;
        case BuiltinSceneShader::ShadowSkinnedVert: return s_shadowSkinnedVert;
        case BuiltinSceneShader::ShadowFrag:        return s_shadowFrag;
        case BuiltinSceneShader::SkyboxVert:        return s_skyboxVert;
        case BuiltinSceneShader::EnvironmentFrag:   return s_envFrag;
        case BuiltinSceneShader::PostFxVert:        return s_postFxVert;
        case BuiltinSceneShader::TonemapFrag:       return s_tonemapFrag;
        case BuiltinSceneShader::BloomFrag:         return s_bloomFrag;
        case BuiltinSceneShader::FxaaFrag:          return s_fxaaFrag;
        case BuiltinSceneShader::BillboardVert:     return s_billboardVert;
        case BuiltinSceneShader::BillboardFrag:     return s_billboardFrag;
        case BuiltinSceneShader::ParticlesVert:     return s_particlesVert;
        case BuiltinSceneShader::ParticlesFrag:     return s_particlesFrag;
        case BuiltinSceneShader::DecalVert:         return s_decalVert;
        case BuiltinSceneShader::DecalFrag:         return s_decalFrag;
        case BuiltinSceneShader::BlurFrag:          return s_blurFrag;
        case BuiltinSceneShader::ColorLutFrag:      return s_colorLutFrag;
        case BuiltinSceneShader::SsaoFrag:          return s_ssaoFrag;
        case BuiltinSceneShader::SsrFrag:           return s_ssrFrag;
        case BuiltinSceneShader::DofFrag:           return s_dofFrag;
        case BuiltinSceneShader::ApplyAoFrag:       return s_applyAoFrag;
        case BuiltinSceneShader::GaussianSplatVert: return s_gaussianSplatVert;
        case BuiltinSceneShader::GaussianSplatFrag: return s_gaussianSplatFrag;
        default: return s_empty;
    }
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
