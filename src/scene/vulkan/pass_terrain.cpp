#include "scene/vulkan/pass_terrain.h"
#include "scene/vulkan/pass_mesh.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/mesh_node.h"
#include "util/log.h"

#include "clipmap_common.glsl.h"
#include "clipmap_detail.glsl.h"
#include "clipmap_cubic_height.glsl.h"
#include "clipmap_cubic.glsl.h"
#include "clipmap_material.glsl.h"
#include "clipmap.vert.glsl.h"
#include "clipmap.frag.glsl.h"

#include <cmath>
#include <cstring>
#include <sstream>

namespace bro::scene::vk {

namespace {

struct alignas(16) TerrainUniforms {
    float _u_heightsSize[4];
    float _u_surfacesSize[4];
    float _u_lWrapX[4];
    float _u_lWrapX45[4];
    float _u_lBandLimited[4];
    float _u_lBandLimited45[4];
    float _u_camXZ[4];
    float _u_chartXZ[4];
    float _u_detailAnchor[4];
    float _u_detailOffset[4];
    float _u_l0a[4]; float _u_l0b[4];
    float _u_l1a[4]; float _u_l1b[4];
    float _u_l2a[4]; float _u_l2b[4];
    float _u_l3a[4]; float _u_l3b[4];
    float _u_l4a[4]; float _u_l4b[4];
    float _u_l5a[4]; float _u_l5b[4];
    float _u_surfA[4]; float _u_surfB[4];
    float _u_surf1A[4]; float _u_surf1B[4];
    float _u_surf2A[4]; float _u_surf2B[4];
    float _u_surf3A[4]; float _u_surf3B[4];
    float _u_surf4A[4]; float _u_surf4B[4];
    float _u_surf5A[4]; float _u_surf5B[4];
    float _u_albedoRock[4];
    float _u_albedoSnow[4];
    float _u_albedoSand[4];
    float _u_albedoGrass[4];
    float _u_albedoForest[4];
    float _u_miscParams0[4];
    float _u_miscParams1[4];
    float _u_miscParams2[4];
    float _u_miscParams3[4];
    float _u_miscParams4[4];
    float _u_miscParams5[4];
};

std::string stripUniforms(const std::string& src) {
    std::istringstream stream(src);
    std::string line;
    std::string out;
    while (std::getline(stream, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && line.compare(first, 8, "uniform ") == 0) {
            out += "// " + line + "\n";
        } else {
            out += line + "\n";
        }
    }
    return out;
}

const char* kTerrainHeader = R"(
#define u_heightsSize (_u_heightsSize.xy)
#define u_surfacesSize (_u_surfacesSize.xy)
#define u_lWrapX (_u_lWrapX)
#define u_lWrapX45 (_u_lWrapX45.xy)
#define u_lBandLimited (_u_lBandLimited)
#define u_lBandLimited45 (_u_lBandLimited45.xy)
#define u_camXZ (_u_camXZ.xy)
#define u_chartXZ (_u_chartXZ.xy)
#define u_detailAnchor (_u_detailAnchor.xy)
#define u_detailOffset (_u_detailOffset.xy)
#define u_l0a (_u_l0a.xyz)
#define u_l0b (_u_l0b.xy)
#define u_l1a (_u_l1a.xyz)
#define u_l1b (_u_l1b.xy)
#define u_l2a (_u_l2a.xyz)
#define u_l2b (_u_l2b.xy)
#define u_l3a (_u_l3a.xyz)
#define u_l3b (_u_l3b.xy)
#define u_l4a (_u_l4a.xyz)
#define u_l4b (_u_l4b.xy)
#define u_l5a (_u_l5a.xyz)
#define u_l5b (_u_l5b.xy)
#define u_surfA (_u_surfA.xyz)
#define u_surfB (_u_surfB.xy)
#define u_surf1A (_u_surf1A.xyz)
#define u_surf1B (_u_surf1B.xy)
#define u_surf2A (_u_surf2A.xyz)
#define u_surf2B (_u_surf2B.xy)
#define u_surf3A (_u_surf3A.xyz)
#define u_surf3B (_u_surf3B.xy)
#define u_surf4A (_u_surf4A.xyz)
#define u_surf4B (_u_surf4B.xy)
#define u_surf5A (_u_surf5A.xyz)
#define u_surf5B (_u_surf5B.xy)
#define u_albedoRock (_u_albedoRock.xyz)
#define u_albedoSnow (_u_albedoSnow.xyz)
#define u_albedoSand (_u_albedoSand.xyz)
#define u_albedoGrass (_u_albedoGrass.xyz)
#define u_albedoForest (_u_albedoForest.xyz)
#define u_cellSize (_u_miscParams0.x)
#define u_cellScale (_u_miscParams0.y)
#define u_invK (_u_miscParams0.z)
#define u_pixelScale (_u_miscParams0.w)
#define u_layerCount (_u_miscParams1.x)
#define u_heightScale (_u_miscParams1.y)
#define u_seaLevel (_u_miscParams1.z)
#define u_camGroundY (_u_miscParams1.w)
#define u_planetRadius (_u_miscParams2.x)
#define u_layerFade (_u_miscParams2.y)
#define u_surfaceCount (_u_miscParams2.z)
#define u_surfPresent (_u_miscParams2.w)
#define u_detailWavelength (_u_miscParams3.x)
#define u_detailRelief (_u_miscParams3.y)
#define u_detailGain (_u_miscParams3.z)
#define u_detailOctaves (_u_miscParams3.w)
#define u_snowLine (_u_miscParams4.x)
#define u_roughnessRock (_u_miscParams4.y)
#define u_roughnessSnow (_u_miscParams4.z)
#define u_roughnessSand (_u_miscParams4.w)
#define u_roughnessGrass (_u_miscParams5.x)
#define u_forestTint (_u_miscParams5.y)
#define u_camY (_u_miscParams5.z)
)";

} // namespace

PassTerrain::~PassTerrain() {
}

bool PassTerrain::init(SceneVkDevice& device, SceneVkAllocator& allocator,
                       VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout lightingLayout) {
    device_ = &device;
    allocator_ = &allocator;
    cameraLayout_ = cameraLayout;
    lightingLayout_ = lightingLayout;
    VkDevice dev = device.device();

    // 1. Samplers
    VkSamplerCreateInfo sInfo{};
    sInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter = VK_FILTER_LINEAR;
    sInfo.minFilter = VK_FILTER_LINEAR;
    sInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.maxLod = 16.0f;
    if (vkCreateSampler(dev, &sInfo, nullptr, &heightsSampler_) != VK_SUCCESS) return false;

    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sInfo.maxLod = 0.0f;
    if (vkCreateSampler(dev, &sInfo, nullptr, &surfacesSampler_) != VK_SUCCESS) return false;

    // 2. Terrain Descriptor Set Layout (Set 2):
    // binding 0: sampler2DArray u_heights
    // binding 1: sampler2DArray u_surfaces
    // binding 2: UBO TerrainUniforms
    SceneVkDescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    layoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    layoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    terrainLayout_ = layoutBuilder.build(dev);
    if (!terrainLayout_) return false;

    // 3. Pipeline Layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(MeshPushConstants);

    std::array<VkDescriptorSetLayout, 3> layouts = {
        cameraLayout_, lightingLayout_, terrainLayout_
    };

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
    plInfo.pSetLayouts = layouts.data();
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(dev, &plInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    // 4. Descriptor pool for per-node terrain descriptors
    terrainDescPool_.init(dev, 64, {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}
    });

    return true;
}

void PassTerrain::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    for (int h = 0; h < 2; ++h) {
        for (int s = 0; s < 2; ++s) {
            if (pipelines_[h][s] != VK_NULL_HANDLE) {
                vkDestroyPipeline(dev, pipelines_[h][s], nullptr);
                pipelines_[h][s] = VK_NULL_HANDLE;
            }
        }
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (terrainLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, terrainLayout_, nullptr);
        terrainLayout_ = VK_NULL_HANDLE;
    }
    if (heightsSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, heightsSampler_, nullptr);
        heightsSampler_ = VK_NULL_HANDLE;
    }
    if (surfacesSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, surfacesSampler_, nullptr);
        surfacesSampler_ = VK_NULL_HANDLE;
    }
    for (auto& [k, res] : nodeResources_) {
        allocator.destroyImage(res.heightsImage);
        allocator.destroyImage(res.surfacesImage);
        allocator.destroyBuffer(res.terrainUbo);
    }
    nodeResources_.clear();
    terrainDescPool_.destroy();
}

VkPipeline PassTerrain::getOrCreatePipeline(bool cubicHeight, bool cubicSurface) {
    int h = cubicHeight ? 1 : 0;
    int s = cubicSurface ? 1 : 0;
    if (pipelines_[h][s] != VK_NULL_HANDLE) return pipelines_[h][s];

    std::string vsCommon = std::string(kClipmapCommonSrc) + kClipmapDetailSrc;
    if (cubicHeight) vsCommon += kClipmapCubicHeightSrc;
    std::string vsCode = "#version 450\n"
        "layout(location = 0) in vec3 inPos;\n"
        "layout(location = 1) in vec3 inNormal;\n"
        "layout(location = 2) in vec2 inUV;\n"
        "layout(location = 3) in vec4 inColor;\n"
        "layout(location = 4) in vec4 inTangent;\n"
        "layout(location = 0) out vec3 vWorldPos;\n"
        "layout(location = 1) out vec3 vNormal;\n"
        "layout(location = 2) out vec2 vUV;\n"
        "layout(location = 3) out vec4 vColor;\n"
        "layout(location = 4) out vec3 vTangentW;\n"
        "layout(location = 5) out vec3 vBitangentW;\n"
        "layout(location = 6) out float vCamDist;\n"
        "layout(set = 0, binding = 0) uniform CameraUBO {\n"
        "    mat4 view; mat4 proj; mat4 viewProj; mat4 invView; mat4 invProj;\n"
        "    vec4 eyePos; vec4 viewport; vec4 fogParams; vec4 fogColor;\n"
        "} camera;\n"
        "layout(push_constant) uniform PushConstants {\n"
        "    mat4 model; vec4 baseColor; vec4 emissive; vec4 pbrParams;\n"
        "} push;\n"
        "layout(set = 2, binding = 0) uniform sampler2DArray u_heights;\n"
        "layout(set = 2, binding = 1) uniform sampler2DArray u_surfaces;\n"
        "layout(set = 2, binding = 2, std140) uniform CustomUniforms {\n"
        "    vec4 _u_heightsSize; vec4 _u_surfacesSize;\n"
        "    vec4 _u_lWrapX; vec4 _u_lWrapX45; vec4 _u_lBandLimited; vec4 _u_lBandLimited45;\n"
        "    vec4 _u_camXZ; vec4 _u_chartXZ; vec4 _u_detailAnchor; vec4 _u_detailOffset;\n"
        "    vec4 _u_l0a; vec4 _u_l0b; vec4 _u_l1a; vec4 _u_l1b; vec4 _u_l2a; vec4 _u_l2b;\n"
        "    vec4 _u_l3a; vec4 _u_l3b; vec4 _u_l4a; vec4 _u_l4b; vec4 _u_l5a; vec4 _u_l5b;\n"
        "    vec4 _u_surfA; vec4 _u_surfB; vec4 _u_surf1A; vec4 _u_surf1B;\n"
        "    vec4 _u_surf2A; vec4 _u_surf2B; vec4 _u_surf3A; vec4 _u_surf3B;\n"
        "    vec4 _u_surf4A; vec4 _u_surf4B; vec4 _u_surf5A; vec4 _u_surf5B;\n"
        "    vec4 _u_albedoRock; vec4 _u_albedoSnow; vec4 _u_albedoSand; vec4 _u_albedoGrass; vec4 _u_albedoForest;\n"
        "    vec4 _u_miscParams0; vec4 _u_miscParams1; vec4 _u_miscParams2;\n"
        "    vec4 _u_miscParams3; vec4 _u_miscParams4; vec4 _u_miscParams5;\n"
        "};\n"
        + std::string(kTerrainHeader)
        + stripUniforms(vsCommon + kClipmapVertSrc)
        + R"(
void main() {
    vec3 pos = inPos;
    vec3 normal = inNormal;
    vec2 uv = inUV;
    userVertex(pos, normal, uv);
    vec4 worldPos = push.model * vec4(pos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = normal;
    vUV = uv;
    vColor = inColor;
    vCamDist = length(pos);
    gl_Position = camera.viewProj * worldPos;
}
)";

    std::string fsCommon = std::string(kClipmapCommonSrc) + kClipmapDetailSrc;
    if (cubicHeight) fsCommon += kClipmapCubicHeightSrc;
    if (cubicSurface) fsCommon += kClipmapCubicSrc;
    fsCommon += kClipmapMaterialSrc;

    std::string fsCode = "#version 450\n"
        "layout(location = 0) in vec3 vWorldPos;\n"
        "layout(location = 1) in vec3 vNormal;\n"
        "layout(location = 2) in vec2 vUV;\n"
        "layout(location = 3) in vec4 vColor;\n"
        "layout(location = 4) in vec3 vTangentW;\n"
        "layout(location = 5) in vec3 vBitangentW;\n"
        "layout(location = 6) in float vCamDist;\n"
        "layout(location = 0) out vec4 outColor;\n"
        "layout(set = 0, binding = 0) uniform CameraUBO {\n"
        "    mat4 view; mat4 proj; mat4 viewProj; mat4 invView; mat4 invProj;\n"
        "    vec4 eyePos; vec4 viewport; vec4 fogParams; vec4 fogColor;\n"
        "} camera;\n"
        "struct PointLight { vec4 position; vec4 color; };\n"
        "layout(set = 1, binding = 0) uniform LightingUBO {\n"
        "    vec4 sunDirection; vec4 sunColor; vec4 ambientColor;\n"
        "    vec4 shadowSplits; mat4 shadowCascadeProj; vec4 numLights;\n"
        "    PointLight pointLights[16];\n"
        "    mat4 probeWorldToLocal; mat4 probeLocalToWorld;\n"
        "    vec4 probePos; vec4 probeBoxSize; vec4 probeParams;\n"
        "    vec4 shadeOrigin; vec4 shadeParams;\n"
        "} lighting;\n"
        "layout(set = 1, binding = 1) uniform sampler2DArrayShadow shadowMapArray;\n"
        "layout(set = 1, binding = 2) uniform samplerCube texReflectionProbe;\n"
        "layout(set = 1, binding = 3) uniform sampler2D texShadeMap;\n"
        "layout(push_constant) uniform PushConstants {\n"
        "    mat4 model; vec4 baseColor; vec4 emissive; vec4 pbrParams;\n"
        "} push;\n"
        "layout(set = 2, binding = 0) uniform sampler2DArray u_heights;\n"
        "layout(set = 2, binding = 1) uniform sampler2DArray u_surfaces;\n"
        "layout(set = 2, binding = 2, std140) uniform CustomUniforms {\n"
        "    vec4 _u_heightsSize; vec4 _u_surfacesSize;\n"
        "    vec4 _u_lWrapX; vec4 _u_lWrapX45; vec4 _u_lBandLimited; vec4 _u_lBandLimited45;\n"
        "    vec4 _u_camXZ; vec4 _u_chartXZ; vec4 _u_detailAnchor; vec4 _u_detailOffset;\n"
        "    vec4 _u_l0a; vec4 _u_l0b; vec4 _u_l1a; vec4 _u_l1b; vec4 _u_l2a; vec4 _u_l2b;\n"
        "    vec4 _u_l3a; vec4 _u_l3b; vec4 _u_l4a; vec4 _u_l4b; vec4 _u_l5a; vec4 _u_l5b;\n"
        "    vec4 _u_surfA; vec4 _u_surfB; vec4 _u_surf1A; vec4 _u_surf1B;\n"
        "    vec4 _u_surf2A; vec4 _u_surf2B; vec4 _u_surf3A; vec4 _u_surf3B;\n"
        "    vec4 _u_surf4A; vec4 _u_surf4B; vec4 _u_surf5A; vec4 _u_surf5B;\n"
        "    vec4 _u_albedoRock; vec4 _u_albedoSnow; vec4 _u_albedoSand; vec4 _u_albedoGrass; vec4 _u_albedoForest;\n"
        "    vec4 _u_miscParams0; vec4 _u_miscParams1; vec4 _u_miscParams2;\n"
        "    vec4 _u_miscParams3; vec4 _u_miscParams4; vec4 _u_miscParams5;\n"
        "};\n"
        + std::string(kTerrainHeader)
        + R"(
float cellShade() {
    float R = max(lighting.shadeParams.x, 1e-6);
    vec3 nudged = vWorldPos - normalize(vNormal) * (0.05 * R);
    vec2 p = nudged.xz - lighting.shadeOrigin.xz;
    int cx, cy;
    if (lighting.shadeParams.y > 0.5) {
        float r = p.y / (1.5 * R);
        float q = p.x / (1.7320508 * R) - r * 0.5;
        float x = q, z = r, y = -x - z;
        float rx = floor(x + 0.5), ry = floor(y + 0.5), rz = floor(z + 0.5);
        float dx = abs(rx - x), dy = abs(ry - y), dz = abs(rz - z);
        if (dx > dy && dx > dz) rx = -ry - rz;
        else if (dy > dz)       ry = -rx - rz;
        else                    rz = -rx - ry;
        int hq = int(rx), hr = int(rz);
        cy = hr;
        cx = hq + (hr - (hr & 1)) / 2;
    } else {
        cx = int(floor(p.x / R));
        cy = int(floor(p.y / R));
    }
    if (cx < 0 || cy < 0 || cx >= int(lighting.shadeParams.z) || cy >= int(lighting.shadeParams.w))
        return 1.0;
    return texelFetch(texShadeMap, ivec2(cx, cy), 0).r;
}
)"
        + stripUniforms(fsCommon + kClipmapFragSrc)
        + R"(
void main() {
    vec3 baseColor = push.baseColor.rgb;
    vec3 normal = normalize(vNormal);
    float metallic = push.pbrParams.x;
    float roughness = push.pbrParams.y;
    vec3 emissive = push.emissive.rgb * push.emissive.a;
    float alpha = push.baseColor.a;

    userFragment(baseColor, normal, metallic, roughness, emissive, alpha);

    vec3 N = normalize(normal);
    vec3 V = normalize(camera.eyePos.xyz - vWorldPos);
    vec3 L = normalize(-lighting.sunDirection.xyz);
    float NdotL = max(dot(N, L), 0.0);
    vec3 radiance = lighting.sunColor.rgb * lighting.sunColor.a;
    vec3 direct = (baseColor / 3.14159265359) * radiance * NdotL;
    vec3 ambient = lighting.ambientColor.rgb * lighting.ambientColor.a * baseColor;
    vec3 color = ambient + direct + emissive;

    uint flags = uint(push.pbrParams.w);
    if ((flags & 32u) != 0u && lighting.shadeOrigin.w > 0.5) {
        color *= cellShade();
    }

    outColor = vec4(color, alpha);
}
)";

    auto vsSpv = SceneVkShaderCompiler::compileGlsl(vsCode, VK_SHADER_STAGE_VERTEX_BIT);
    auto fsSpv = SceneVkShaderCompiler::compileGlsl(fsCode, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vsSpv.empty() || fsSpv.empty()) {
        LOG_ERROR("PassTerrain: Failed compiling shaders for terrain (cubicHeight=%d, cubicSurface=%d)",
                  cubicHeight, cubicSurface);
        return VK_NULL_HANDLE;
    }

    VkShaderModule vsModule = SceneVkShaderCompiler::createModule(device_->device(), vsSpv);
    VkShaderModule fsModule = SceneVkShaderCompiler::createModule(device_->device(), fsSpv);

    VkVertexInputBindingDescription staticBinding{};
    staticBinding.binding = 0;
    staticBinding.stride = 64;
    staticBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> staticAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48}
    };

    SceneVkPipelineBuilder builder;
    builder.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsModule)
           .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fsModule)
           .setVertexInput({staticBinding}, staticAttributes)
           .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
           .setPolygonMode(VK_POLYGON_MODE_FILL)
           .setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
           .setMultisampling(sampleCount_)
           .disableBlending(1)
           .enableDepthTest(true, VK_COMPARE_OP_GREATER_OR_EQUAL)
           .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_D32_SFLOAT);

    VkPipeline pipeline = builder.build(device_->device(), pipelineLayout_);

    SceneVkShaderCompiler::destroyModule(device_->device(), vsModule);
    SceneVkShaderCompiler::destroyModule(device_->device(), fsModule);

    pipelines_[h][s] = pipeline;
    return pipeline;
}

PassTerrain::NodeTerrainResources& PassTerrain::getOrCreateNodeResources(MeshNode* node) {
    auto& res = nodeResources_[node];
    if (!res.terrainUbo.isValid()) {
        allocator_->createUniformBuffer(sizeof(TerrainUniforms), res.terrainUbo);
        res.terrainDescSet = terrainDescPool_.allocate(terrainLayout_);

        allocator_->createImage(1, 1, VK_FORMAT_R32_SFLOAT,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                res.heightsImage, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                1, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY);

        allocator_->createImage(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                res.surfacesImage, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                1, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY);

        SceneVkDescriptorWriter writer;
        writer.writeImage(0, res.heightsImage.view, heightsSampler_);
        writer.writeImage(1, res.surfacesImage.view, surfacesSampler_);
        writer.writeBuffer(2, res.terrainUbo.buffer, sizeof(TerrainUniforms));
        writer.updateSet(device_->device(), res.terrainDescSet);
    }
    return res;
}

bool PassTerrain::syncTextures(MeshNode* node, NodeTerrainResources& res) {
    bool updated = false;
    for (auto& t : node->customShaderTextures()) {
        if (t.name == "u_heights") {
            int w = t.w, h = t.h, layers = t.layers;
            if (w <= 0 || h <= 0 || layers <= 0) continue;

            uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(std::min(w, h)))) + 1;
            bool realloc = (!res.heightsImage.isValid() || res.currentHeightsW != w || res.currentHeightsH != h || res.currentHeightsLayers != layers);

            if (realloc) {
                allocator_->destroyImage(res.heightsImage);
                allocator_->createImage(w, h, VK_FORMAT_R32_SFLOAT,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        res.heightsImage, mips, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                        layers, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
                res.currentHeightsW = w;
                res.currentHeightsH = h;
                res.currentHeightsLayers = layers;
                updated = true;
            }

            if (!t.sliceUpdates.empty()) {
                for (const auto& u : t.sliceUpdates) {
                    if (u.layer >= layers || u.data.empty()) continue;
                    allocator_->stageAndUploadImageLayer(res.heightsImage.image, VK_FORMAT_R32_SFLOAT,
                                                         w, h, u.layer, 0,
                                                         u.data.data(), u.data.size() * sizeof(float));
                    if (mips > 1) {
                        allocator_->device().executeImmediate([&](VkCommandBuffer cmd) {
                            allocator_->generateMipmaps(cmd, res.heightsImage.image, VK_FORMAT_R32_SFLOAT,
                                                        w, h, mips, u.layer, 1);
                        });
                    }
                }
                t.sliceUpdates.clear();
                updated = true;
            }
        } else if (t.name == "u_surfaces") {
            int w = t.w, h = t.h, layers = t.layers;
            if (w <= 0 || h <= 0 || layers <= 0) continue;

            bool realloc = (!res.surfacesImage.isValid() || res.currentSurfsW != w || res.currentSurfsH != h || res.currentSurfsLayers != layers);

            if (realloc) {
                allocator_->destroyImage(res.surfacesImage);
                allocator_->createImage(w, h, VK_FORMAT_R32G32B32A32_SFLOAT,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        res.surfacesImage, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                        layers, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
                res.currentSurfsW = w;
                res.currentSurfsH = h;
                res.currentSurfsLayers = layers;
                updated = true;
            }

            if (!t.sliceUpdates.empty()) {
                for (const auto& u : t.sliceUpdates) {
                    if (u.layer >= layers || u.data.empty()) continue;
                    allocator_->stageAndUploadImageLayer(res.surfacesImage.image, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                         w, h, u.layer, 0,
                                                         u.data.data(), u.data.size() * sizeof(float));
                }
                t.sliceUpdates.clear();
                updated = true;
            }
        }
    }

    if (updated) {
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, res.heightsImage.view, heightsSampler_);
        writer.writeImage(1, res.surfacesImage.view, surfacesSampler_);
        writer.writeBuffer(2, res.terrainUbo.buffer, sizeof(TerrainUniforms));
        writer.updateSet(device_->device(), res.terrainDescSet);
    }
    return true;
}

void PassTerrain::syncUniforms(MeshNode* node, NodeTerrainResources& res) {
    TerrainUniforms u{};
    std::memset(&u, 0, sizeof(u));
    if (!node || !node->customShader()) return;

    const auto& uniforms = node->customShader()->uniforms;
    auto getU = [&](const std::string& name, int count, float* dst) {
        for (const auto& unif : uniforms) {
            if (unif.name == name) {
                int n = std::min<int>(count, unif.comps);
                for (int i = 0; i < n; ++i) dst[i] = unif.v[i];
                return;
            }
        }
    };

    getU("u_heightsSize", 2, u._u_heightsSize);
    getU("u_surfacesSize", 2, u._u_surfacesSize);
    getU("u_lWrapX", 4, u._u_lWrapX);
    getU("u_lWrapX45", 2, u._u_lWrapX45);
    getU("u_lBandLimited", 4, u._u_lBandLimited);
    getU("u_lBandLimited45", 2, u._u_lBandLimited45);
    getU("u_camXZ", 2, u._u_camXZ);
    getU("u_chartXZ", 2, u._u_chartXZ);
    getU("u_detailAnchor", 2, u._u_detailAnchor);
    getU("u_detailOffset", 2, u._u_detailOffset);
    getU("u_l0a", 3, u._u_l0a); getU("u_l0b", 2, u._u_l0b);
    getU("u_l1a", 3, u._u_l1a); getU("u_l1b", 2, u._u_l1b);
    getU("u_l2a", 3, u._u_l2a); getU("u_l2b", 2, u._u_l2b);
    getU("u_l3a", 3, u._u_l3a); getU("u_l3b", 2, u._u_l3b);
    getU("u_l4a", 3, u._u_l4a); getU("u_l4b", 2, u._u_l4b);
    getU("u_l5a", 3, u._u_l5a); getU("u_l5b", 2, u._u_l5b);
    getU("u_surfA", 3, u._u_surfA); getU("u_surfB", 2, u._u_surfB);
    getU("u_surf1A", 3, u._u_surf1A); getU("u_surf1B", 2, u._u_surf1B);
    getU("u_surf2A", 3, u._u_surf2A); getU("u_surf2B", 2, u._u_surf2B);
    getU("u_surf3A", 3, u._u_surf3A); getU("u_surf3B", 2, u._u_surf3B);
    getU("u_surf4A", 3, u._u_surf4A); getU("u_surf4B", 2, u._u_surf4B);
    getU("u_surf5A", 3, u._u_surf5A); getU("u_surf5B", 2, u._u_surf5B);
    getU("u_albedoRock", 3, u._u_albedoRock);
    getU("u_albedoSnow", 3, u._u_albedoSnow);
    getU("u_albedoSand", 3, u._u_albedoSand);
    getU("u_albedoGrass", 3, u._u_albedoGrass);
    getU("u_albedoForest", 3, u._u_albedoForest);

    getU("u_cellSize", 1, &u._u_miscParams0[0]);
    getU("u_cellScale", 1, &u._u_miscParams0[1]);
    getU("u_invK", 1, &u._u_miscParams0[2]);
    getU("u_pixelScale", 1, &u._u_miscParams0[3]);

    getU("u_layerCount", 1, &u._u_miscParams1[0]);
    getU("u_heightScale", 1, &u._u_miscParams1[1]);
    getU("u_seaLevel", 1, &u._u_miscParams1[2]);
    getU("u_camGroundY", 1, &u._u_miscParams1[3]);

    getU("u_planetRadius", 1, &u._u_miscParams2[0]);
    getU("u_layerFade", 1, &u._u_miscParams2[1]);
    getU("u_surfaceCount", 1, &u._u_miscParams2[2]);
    getU("u_surfPresent", 1, &u._u_miscParams2[3]);

    getU("u_detailWavelength", 1, &u._u_miscParams3[0]);
    getU("u_detailRelief", 1, &u._u_miscParams3[1]);
    getU("u_detailGain", 1, &u._u_miscParams3[2]);
    getU("u_detailOctaves", 1, &u._u_miscParams3[3]);

    getU("u_snowLine", 1, &u._u_miscParams4[0]);
    getU("u_roughnessRock", 1, &u._u_miscParams4[1]);
    getU("u_roughnessSnow", 1, &u._u_miscParams4[2]);
    getU("u_roughnessSand", 1, &u._u_miscParams4[3]);

    getU("u_roughnessGrass", 1, &u._u_miscParams5[0]);
    getU("u_forestTint", 1, &u._u_miscParams5[1]);
    getU("u_camY", 1, &u._u_miscParams5[2]);

    allocator_->updateUniformBuffer(res.terrainUbo, &u, sizeof(u));
}

void PassTerrain::render(VkCommandBuffer cmd, MeshNode* terrainNode,
                         VkDescriptorSet cameraSet, VkDescriptorSet lightingSet,
                         uint32_t viewportWidth, uint32_t viewportHeight,
                         VkBuffer vertexBuffer, VkBuffer indexBuffer, uint32_t indexCount) {
    if (!terrainNode || indexCount == 0) return;

    bool cubicHeight = false;
    bool cubicSurface = false;
    if (terrainNode->customShader()) {
        const auto& vs = terrainNode->customShader()->vertexChunk;
        const auto& fs = terrainNode->customShader()->fragmentChunk;
        if (vs.find("cmCubicTapLevel") != std::string::npos || vs.find("cmHeightCubic") != std::string::npos) {
            cubicHeight = true;
        }
        if (fs.find("cmCubicTap") != std::string::npos) {
            cubicSurface = true;
        }
    }

    VkPipeline pipeline = getOrCreatePipeline(cubicHeight, cubicSurface);
    if (!pipeline) return;

    NodeTerrainResources& res = getOrCreateNodeResources(terrainNode);
    syncTextures(terrainNode, res);
    syncUniforms(terrainNode, res);

    VkViewport vp{};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = static_cast<float>(viewportWidth);
    vp.height = static_cast<float>(viewportHeight);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {viewportWidth, viewportHeight};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    std::array<VkDescriptorSet, 3> descSets = {
        cameraSet, lightingSet, res.terrainDescSet
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, static_cast<uint32_t>(descSets.size()), descSets.data(),
                            0, nullptr);

    MeshPushConstants push{};
    std::memcpy(push.model, terrainNode->worldMatrix().data, sizeof(push.model));
    std::memcpy(push.baseColor, terrainNode->color(), sizeof(push.baseColor));
    std::memcpy(push.emissive, terrainNode->emissiveColor(), sizeof(push.emissive));
    push.emissive[3] = terrainNode->emissive();
    push.pbrParams[0] = terrainNode->metallic();
    push.pbrParams[1] = terrainNode->roughness();
    push.pbrParams[2] = terrainNode->alphaCutoff();
    push.pbrParams[3] = terrainNode->shadeMap() ? 32.0f : 0.0f;

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}

} // namespace bro::scene::vk
