#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "render/glsl_compiler.h"
#include "util/log.h"


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

template<size_t N>
std::vector<uint32_t> arrayToVector(const uint32_t (&arr)[N]) {
    return std::vector<uint32_t>(arr, arr + N);
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
    std::vector<uint32_t> spirv = render::compileGlslToSpirv(glslSource, glStage, &log);
    if (spirv.empty()) LOG_ERROR("SceneVkShaderCompiler: GLSL compilation failed:\n%s", log.c_str());
    if (errOut) *errOut = std::move(log);
    return spirv;
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
