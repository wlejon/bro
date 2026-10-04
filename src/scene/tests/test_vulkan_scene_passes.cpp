#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/pass_mesh.h"
#include "scene/vulkan/pass_shadow.h"
#include "scene/vulkan/pass_environment.h"
#include "scene/vulkan/pass_postfx.h"
#include "scene/vulkan/pass_reflection_probe.h"
#include "render/vulkan_context.h"
#include "util/log.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace bro;
using namespace bro::scene::vk;

// Interleaved vertex: pos(3), normal(3), uv(2), color(4), tangent(4) = 16 floats = 64 bytes
struct TestVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float color[4];
    float tangent[4];
};

static void setIdentity(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void makeLookAt(const float* eye, const float* target, const float* up, float* out) {
    float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    float fLen = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    f[0] /= fLen; f[1] /= fLen; f[2] /= fLen;

    float s[3] = {
        f[1] * up[2] - f[2] * up[1],
        f[2] * up[0] - f[0] * up[2],
        f[0] * up[1] - f[1] * up[0]
    };
    float sLen = std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
    s[0] /= sLen; s[1] /= sLen; s[2] /= sLen;

    float u[3] = {
        s[1] * f[2] - s[2] * f[1],
        s[2] * f[0] - s[0] * f[2],
        s[0] * f[1] - s[1] * f[0]
    };

    setIdentity(out);
    out[0] = s[0];  out[4] = s[1];  out[8]  = s[2];  out[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    out[1] = u[0];  out[5] = u[1];  out[9]  = u[2];  out[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    out[2] = -f[0]; out[6] = -f[1]; out[10] = -f[2]; out[14] =  (f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
    out[3] = 0.0f;  out[7] = 0.0f;  out[11] = 0.0f;  out[15] = 1.0f;
}

// Reversed-Z infinite perspective projection matrix
static void makeReversedZPerspective(float fovYRad, float aspect, float zNear, float* out) {
    std::memset(out, 0, sizeof(float) * 16);
    float f = 1.0f / std::tan(fovYRad * 0.5f);
    out[0] = f / aspect;
    out[5] = -f; // Invert Y for Vulkan
    out[10] = 0.0f;
    out[11] = -1.0f;
    out[14] = zNear;
    out[15] = 0.0f;
}

int main() {
    std::cout << "=== Running Vulkan Chunk 3: 3D Scene Passes & SPIR-V Pipeline Tests ===" << std::endl;

    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    cfg.enableDynamicRendering = true;

    render::VulkanContext context(cfg);
    if (!context.init()) {
        std::cerr << "Failed to initialize headless Vulkan context!" << std::endl;
        return 1;
    }

    SceneVkDevice device(context);
    if (!device.init()) {
        std::cerr << "Failed to initialize SceneVkDevice!" << std::endl;
        return 1;
    }

    SceneVkAllocator allocator(device);

    // -------------------------------------------------------------------------
    // Test 1: SceneVkShaderCompiler & SPIR-V Pipeline
    // -------------------------------------------------------------------------
    std::cout << "[Test 1] SceneVkShaderCompiler (SPIR-V Bytecode & glslc Pipeline)... " << std::flush;
    {
        // 1. Verify built-in shader bytecode
        const auto& meshVs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::MeshVert);
        const auto& meshFs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::MeshFrag);
        const auto& shadowVs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::ShadowVert);
        const auto& envFs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::EnvironmentFrag);
        const auto& tonemapFs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::TonemapFrag);

        assert(!meshVs.empty() && meshVs[0] == 0x07230203);
        assert(!meshFs.empty() && meshFs[0] == 0x07230203);
        assert(!shadowVs.empty() && shadowVs[0] == 0x07230203);
        assert(!envFs.empty() && envFs[0] == 0x07230203);
        assert(!tonemapFs.empty() && tonemapFs[0] == 0x07230203);

        // 2. Test runtime GLSL compilation via glslc
        if (SceneVkShaderCompiler::hasGlslc()) {
            std::string testGlsl =
                "#version 450\n"
                "layout(location = 0) in vec3 pos;\n"
                "layout(location = 0) out vec3 outPos;\n"
                "void main() {\n"
                "    gl_Position = vec4(pos, 1.0);\n"
                "    outPos = pos;\n"
                "}\n";
            auto compiled = SceneVkShaderCompiler::compileGlsl(testGlsl, VK_SHADER_STAGE_VERTEX_BIT);
            assert(!compiled.empty());
            assert(compiled[0] == 0x07230203);

            // Test shader module creation and deletion
            VkShaderModule mod = SceneVkShaderCompiler::createModule(device.device(), compiled);
            assert(mod != VK_NULL_HANDLE);
            SceneVkShaderCompiler::destroyModule(device.device(), mod);
        }

        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 2: Pass Initialization (Shadow, Environment, Mesh, PostFx)
    // -------------------------------------------------------------------------
    std::cout << "[Test 2] Initializing Scene Passes (Shadow, Env, Mesh, PostFx)... " << std::flush;
    const uint32_t kWidth = 256;
    const uint32_t kHeight = 256;

    PassShadow passShadow;
    bool shadowOk = passShadow.init(device);
    assert(shadowOk);

    PassEnvironment passEnv;
    bool envOk = passEnv.init(device, allocator);
    assert(envOk);

    PassMesh passMesh;
    bool meshOk = passMesh.init(device, allocator);
    assert(meshOk);

    PassPostFx passPostFx;
    bool postfxOk = passPostFx.init(device, allocator, kWidth, kHeight);
    assert(postfxOk);

    PassReflectionProbe passProbe;
    bool probeOk = passProbe.init(device, allocator);
    assert(probeOk);

    std::cout << "PASSED" << std::endl;

    // -------------------------------------------------------------------------
    // Test 3: Pass Chain Execution (Shadow -> Env -> Mesh -> PostFx)
    // -------------------------------------------------------------------------
    std::cout << "[Test 3] Executing Complete Pass Chain & Offscreen Rendering... " << std::flush;

    // 1. Targets setup
    SceneVkShadowCascadeTarget shadowTarget;
    bool shadowTargetOk = shadowTarget.init(allocator, 256, 4, VK_FORMAT_D32_SFLOAT);
    assert(shadowTargetOk);

    SceneVkRenderTargetDesc hdrDesc{};
    hdrDesc.width = kWidth;
    hdrDesc.height = kHeight;
    hdrDesc.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    hdrDesc.depthFormat = VK_FORMAT_D32_SFLOAT;
    hdrDesc.hasColor = true;
    hdrDesc.hasDepth = true;

    SceneVkRenderTarget hdrTarget;
    bool hdrOk = hdrTarget.init(allocator, hdrDesc);
    assert(hdrOk);

    // LDR presentation target image (e.g. final swapchain / surface equivalent)
    SceneVkImage ldrPresentationImage;
    VkImageUsageFlags ldrUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bool ldrOk = allocator.createImage(kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM, ldrUsage,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ldrPresentationImage);
    assert(ldrOk);

    // 2. Geometry setup: A front-facing quad in [-0.5, 0.5]
    TestVertex quadVertices[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.8f, 0.2f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.8f, 0.2f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.8f, 0.2f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.8f, 0.2f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}
    };
    uint32_t quadIndices[6] = {0, 1, 2, 2, 3, 0};

    SceneVkBuffer vertexBuffer, indexBuffer;
    allocator.createVertexBuffer(sizeof(quadVertices), quadVertices, vertexBuffer);
    allocator.createIndexBuffer(sizeof(quadIndices), quadIndices, indexBuffer);

    // 3. Instance data setup: 4 instances arranged horizontally
    float instanceData[4 * 16];
    for (int i = 0; i < 4; ++i) {
        float* row = &instanceData[i * 16];
        setIdentity(row);
        row[3]  = -0.75f + static_cast<float>(i) * 0.5f; // X offset
        row[7]  = 0.0f;                                  // Y offset
        row[11] = 0.0f;                                  // Z offset
        // Instance color tint
        row[12] = 0.5f + 0.1f * i;
        row[13] = 1.0f - 0.2f * i;
        row[14] = 0.2f * i;
        row[15] = 1.0f;
    }
    SceneVkBuffer instanceBuffer;
    allocator.createVertexBuffer(sizeof(instanceData), instanceData, instanceBuffer);

    // 4. Skinned data setup: joints & weights + bone palette
    struct SkinVertex {
        uint16_t joints[4];
        float weights[4];
    };
    SkinVertex skinVerts[4] = {
        {{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}}
    };
    SceneVkBuffer skinAttribBuffer;
    allocator.createVertexBuffer(sizeof(skinVerts), skinVerts, skinAttribBuffer);

    // Bone palette UBO (256 mat4s)
    std::vector<float> boneMatrices(256 * 16, 0.0f);
    for (size_t b = 0; b < 256; ++b) {
        setIdentity(&boneMatrices[b * 16]);
    }
    SceneVkBuffer boneUbo;
    allocator.createUniformBuffer(boneMatrices.size() * sizeof(float), boneUbo);
    allocator.updateUniformBuffer(boneUbo, boneMatrices.data(), boneMatrices.size() * sizeof(float));

    SceneVkDescriptorPool boneDescPool;
    boneDescPool.init(device.device(), 2);
    VkDescriptorSet boneSet = boneDescPool.allocate(passMesh.bonePaletteLayout());
    SceneVkDescriptorWriter boneWriter;
    boneWriter.writeBuffer(0, boneUbo.buffer, boneMatrices.size() * sizeof(float));
    boneWriter.updateSet(device.device(), boneSet);

    // 5. Camera & Lighting Uniforms
    float eye[3] = {0.0f, 0.0f, 2.5f};
    float target[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};

    SceneCameraUniforms camUniforms{};
    makeLookAt(eye, target, up, camUniforms.view);
    makeReversedZPerspective(1.047f, 1.0f, 0.1f, camUniforms.proj); // 60 deg fov
    // ViewProj = Proj * View
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            camUniforms.viewProj[c * 4 + r] =
                camUniforms.proj[0 * 4 + r] * camUniforms.view[c * 4 + 0] +
                camUniforms.proj[1 * 4 + r] * camUniforms.view[c * 4 + 1] +
                camUniforms.proj[2 * 4 + r] * camUniforms.view[c * 4 + 2] +
                camUniforms.proj[3 * 4 + r] * camUniforms.view[c * 4 + 3];
        }
    }
    camUniforms.eyePos[0] = eye[0];
    camUniforms.eyePos[1] = eye[1];
    camUniforms.eyePos[2] = eye[2];
    camUniforms.eyePos[3] = 0.0f;
    camUniforms.viewport[0] = static_cast<float>(kWidth);
    camUniforms.viewport[1] = static_cast<float>(kHeight);
    camUniforms.viewport[2] = 0.1f;
    camUniforms.viewport[3] = 1000.0f;

    SceneVkBuffer cameraUbo;
    allocator.createUniformBuffer(sizeof(camUniforms), cameraUbo);
    allocator.updateUniformBuffer(cameraUbo, &camUniforms, sizeof(camUniforms));

    SceneLightingUniforms lightUniforms{};
    lightUniforms.sunDirection[0] = 0.577f;
    lightUniforms.sunDirection[1] = -0.577f;
    lightUniforms.sunDirection[2] = -0.577f;
    lightUniforms.sunDirection[3] = 1.0f; // enabled
    lightUniforms.sunColor[0] = 1.0f;
    lightUniforms.sunColor[1] = 0.95f;
    lightUniforms.sunColor[2] = 0.9f;
    lightUniforms.sunColor[3] = 3.0f; // intensity
    lightUniforms.ambientColor[0] = 0.1f;
    lightUniforms.ambientColor[1] = 0.15f;
    lightUniforms.ambientColor[2] = 0.25f;
    lightUniforms.ambientColor[3] = 1.0f;
    lightUniforms.numLights[0] = 1.0f;
    lightUniforms.numLights[2] = 1.0f; // shadow enabled

    // Light VP for shadow cascade 0
    float lightEye[3] = {2.0f, 3.0f, 2.0f};
    float lightView[16], lightProj[16];
    makeLookAt(lightEye, target, up, lightView);
    setIdentity(lightProj);
    // Orthographic projection for directional light
    lightProj[0] = 0.5f; lightProj[5] = -0.5f; lightProj[10] = 0.2f; lightProj[15] = 1.0f;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            lightUniforms.shadowCascadeProj[c * 4 + r] =
                lightProj[0 * 4 + r] * lightView[c * 4 + 0] +
                lightProj[1 * 4 + r] * lightView[c * 4 + 1] +
                lightProj[2 * 4 + r] * lightView[c * 4 + 2] +
                lightProj[3 * 4 + r] * lightView[c * 4 + 3];
        }
    }

    SceneVkBuffer lightUbo;
    allocator.createUniformBuffer(sizeof(lightUniforms), lightUbo);
    allocator.updateUniformBuffer(lightUbo, &lightUniforms, sizeof(lightUniforms));

    SceneVkDescriptorPool mainDescPool;
    mainDescPool.init(device.device(), 8);

    VkDescriptorSet camSet = mainDescPool.allocate(passMesh.cameraLayout());
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, cameraUbo.buffer, sizeof(camUniforms));
    camWriter.updateSet(device.device(), camSet);

    VkDescriptorSet lightSet = mainDescPool.allocate(passMesh.lightingLayout());
    SceneVkDescriptorWriter lightWriter;
    lightWriter.writeBuffer(0, lightUbo.buffer, sizeof(lightUniforms));
    lightWriter.writeImage(1, shadowTarget.arrayView(), shadowTarget.shadowSampler());
    lightWriter.writeImage(2, passProbe.dummyCubemapView(), passProbe.activeCubemapSampler());
    lightWriter.writeImage(3, passMesh.dummyBlackView(), passMesh.defaultSampler());
    lightWriter.updateSet(device.device(), lightSet);

    // -------------------------------------------------------------------------
    // Record and Execute Pass Chain
    // -------------------------------------------------------------------------
    VkCommandBuffer cmd = device.beginFrame();

    // 1. PassShadow: Render shadow map cascade
    passShadow.beginCascade(cmd, device, shadowTarget, 0, lightUniforms.shadowCascadeProj);
    ShadowCaster caster{};
    caster.vertexBuffer = vertexBuffer.buffer;
    caster.indexBuffer = indexBuffer.buffer;
    caster.indexCount = 6;
    setIdentity(caster.modelMatrix);
    passShadow.drawStatic(cmd, caster);
    passShadow.endCascade(cmd, device, shadowTarget);
    shadowTarget.transitionToShaderRead(cmd, allocator);

    // 2. PassEnvironment: Draw atmosphere/sky background into HDR target
    hdrTarget.beginRendering(cmd, device,
                            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                            {{0.0f, 0.0f, 0.0f, 1.0f}},
                            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                            0.0f); // Reversed-Z clear depth is 0.0f

    EnvironmentParams envParams{};
    // Inverse view projection (reconstruct forward ray into screen)
    setIdentity(envParams.invViewProj);
    envParams.invViewProj[10] = -1.0f;
    envParams.sunDirection[0] = lightUniforms.sunDirection[0];
    envParams.sunDirection[1] = lightUniforms.sunDirection[1];
    envParams.sunDirection[2] = lightUniforms.sunDirection[2];
    passEnv.render(cmd, kWidth, kHeight, envParams);

    // 3. PassMesh: Render static, instanced, and skinned meshes into HDR target
    passMesh.begin(cmd, camSet, lightSet, kWidth, kHeight);

    // Static mesh draw
    MeshDrawCall staticDraw{};
    staticDraw.vertexBuffer = vertexBuffer.buffer;
    staticDraw.indexBuffer = indexBuffer.buffer;
    staticDraw.indexCount = 6;
    setIdentity(staticDraw.modelMatrix);
    staticDraw.baseColor[0] = 0.9f;
    staticDraw.baseColor[1] = 0.7f;
    staticDraw.baseColor[2] = 0.2f; // Gold metallic quad
    staticDraw.metallic = 0.8f;
    staticDraw.roughness = 0.2f;
    passMesh.drawStatic(cmd, staticDraw);

    // Instanced mesh draw
    InstancedMeshDrawCall instDraw{};
    instDraw.vertexBuffer = vertexBuffer.buffer;
    instDraw.indexBuffer = indexBuffer.buffer;
    instDraw.indexCount = 6;
    instDraw.instanceBuffer = instanceBuffer.buffer;
    instDraw.instanceCount = 4;
    setIdentity(instDraw.modelMatrix);
    passMesh.drawInstanced(cmd, instDraw);

    // Skinned mesh draw
    SkinnedMeshDrawCall skinDraw{};
    skinDraw.vertexBuffer = vertexBuffer.buffer;
    skinDraw.indexBuffer = indexBuffer.buffer;
    skinDraw.indexCount = 6;
    skinDraw.skinAttribBuffer = skinAttribBuffer.buffer;
    skinDraw.bonePaletteSet = boneSet;
    setIdentity(skinDraw.modelMatrix);
    skinDraw.modelMatrix[12] = 0.5f; // shift right
    passMesh.drawSkinned(cmd, skinDraw);

    hdrTarget.endRendering(cmd, device);
    hdrTarget.transitionColorToShaderRead(cmd, allocator);

    // 4. PassPostFx: Tonemap + Bloom + FXAA into LDR presentation target
    PostFxParams postFxParams{};
    postFxParams.exposure = 1.0f;
    postFxParams.gamma = 2.2f;
    postFxParams.tonemapMode = TonemapMode::ACES;
    postFxParams.enableBloom = true;
    postFxParams.bloomIntensity = 0.1f;
    postFxParams.enableFxaa = true;

    // Transition presentation target from UNDEFINED to COLOR_ATTACHMENT_OPTIMAL
    allocator.transitionImageLayout(cmd, ldrPresentationImage.image, VK_FORMAT_R8G8B8A8_UNORM,
                                   ldrPresentationImage.currentLayout,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ldrPresentationImage.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    passPostFx.render(cmd, device, allocator,
                      hdrTarget.colorImage(),
                      ldrPresentationImage.view,
                      VK_FORMAT_R8G8B8A8_UNORM,
                      kWidth, kHeight, postFxParams);

    // Transition presentation target to TRANSFER_SRC_OPTIMAL for readback
    allocator.transitionImageLayout(cmd, ldrPresentationImage.image, VK_FORMAT_R8G8B8A8_UNORM,
                                   ldrPresentationImage.currentLayout,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ldrPresentationImage.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // Allocate readback staging buffer
    VkDeviceSize readbackSize = kWidth * kHeight * 4;
    SceneVkBuffer readbackBuffer;
    allocator.createBuffer(readbackSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           readbackBuffer);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = kWidth;
    copyRegion.bufferImageHeight = kHeight;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {kWidth, kHeight, 1};

    vkCmdCopyImageToBuffer(cmd, ldrPresentationImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer.buffer, 1, &copyRegion);

    device.endFrame();
    assert(device.submitFrame());
    device.waitIdle();

    // -------------------------------------------------------------------------
    // Verification: Inspect pixel output
    // -------------------------------------------------------------------------
    const uint8_t* pixels = nullptr;
    if (readbackBuffer.mappedData) {
        pixels = static_cast<const uint8_t*>(readbackBuffer.mappedData);
    } else {
        void* mapped = nullptr;
        vkMapMemory(device.device(), readbackBuffer.memory, readbackBuffer.offset, readbackSize, 0, &mapped);
        assert(mapped != nullptr);
        pixels = static_cast<const uint8_t*>(mapped);
    }

    // Sample center pixel (where quad geometry is rendered)
    uint32_t centerIndex = (kHeight / 2 * kWidth + kWidth / 2) * 4;
    uint8_t rCenter = pixels[centerIndex + 0];
    uint8_t gCenter = pixels[centerIndex + 1];
    uint8_t bCenter = pixels[centerIndex + 2];
    uint8_t aCenter = pixels[centerIndex + 3];

    // Sample sky pixel (top-left corner)
    uint8_t rSky0 = pixels[0], gSky0 = pixels[1], bSky0 = pixels[2], aSky0 = pixels[3];
    uint32_t p10Idx = (10 * kWidth + 10) * 4;
    uint8_t r10 = pixels[p10Idx], g10 = pixels[p10Idx+1], b10 = pixels[p10Idx+2], a10 = pixels[p10Idx+3];
    uint32_t p64Idx = (64 * kWidth + 64) * 4;
    uint8_t r64 = pixels[p64Idx], g64 = pixels[p64Idx+1], b64 = pixels[p64Idx+2], a64 = pixels[p64Idx+3];

    std::cout << "\n  -> Shaded Quad Center pixel RGBA: ("
              << (int)rCenter << ", " << (int)gCenter << ", " << (int)bCenter << ", " << (int)aCenter << ")" << std::endl;
    std::cout << "  -> Pixel (0,0) RGBA: ("
              << (int)rSky0 << ", " << (int)gSky0 << ", " << (int)bSky0 << ", " << (int)aSky0 << ")" << std::endl;
    std::cout << "  -> Pixel (10,10) RGBA: ("
              << (int)r10 << ", " << (int)g10 << ", " << (int)b10 << ", " << (int)a10 << ")" << std::endl;
    std::cout << "  -> Pixel (64,64) RGBA: ("
              << (int)r64 << ", " << (int)g64 << ", " << (int)b64 << ", " << (int)a64 << ")" << std::endl;

    uint8_t rSky = r64, gSky = g64, bSky = b64, aSky = a64;

    // Shaded gold quad pixel should have non-zero warm color (R > 0, G > 0) and A = 255
    assert(rCenter > 0);
    assert(gCenter > 0);
    assert(aCenter == 255);

    // Sky background pixel should also be non-zero (atmosphere gradient rendered) and A = 255
    assert(rSky > 0 || gSky > 0 || bSky > 0);
    assert(aSky == 255);

    if (!readbackBuffer.mappedData) {
        vkUnmapMemory(device.device(), readbackBuffer.memory);
    }
    allocator.destroyBuffer(readbackBuffer);

    std::cout << "  -> Pixel verification PASSED!" << std::endl;

    // Cleanup resources
    allocator.destroyImage(ldrPresentationImage);
    hdrTarget.cleanup(allocator);
    shadowTarget.cleanup(allocator);

    allocator.destroyBuffer(vertexBuffer);
    allocator.destroyBuffer(indexBuffer);
    allocator.destroyBuffer(instanceBuffer);
    allocator.destroyBuffer(skinAttribBuffer);
    allocator.destroyBuffer(boneUbo);
    allocator.destroyBuffer(cameraUbo);
    allocator.destroyBuffer(lightUbo);

    boneDescPool.destroy();
    mainDescPool.destroy();

    passPostFx.cleanup(device, allocator);
    passMesh.cleanup(device, allocator);
    passEnv.cleanup(device, allocator);
    passShadow.cleanup(device);
    passProbe.cleanup(device, allocator);

    device.shutdown();

    std::cout << "=== All Vulkan Chunk 3 Scene Pass Tests Passed Successfully! ===" << std::endl;
    return 0;
}
