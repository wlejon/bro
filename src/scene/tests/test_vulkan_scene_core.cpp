// bro_vulkan_scene_test: the scene's Vulkan building blocks — device, allocator,
// pipeline builder, descriptors, render targets — drawing and reading back.
//
// assert() is the check here, so it must survive a Release build: NDEBUG is
// undefined before any header can pull in <cassert>. Run under
// BRO_VK_VALIDATION=1 (tests/run_tests.sh does) and any validation error
// fails the run too.
#undef NDEBUG

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_frame_graph.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_target_format.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "util/log.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace bro;
using namespace bro::scene::vk;

// Vertex SPIR-V compiled from GLSL (pos: vec3, color: vec4)
static const uint32_t kTestVertexSpv[] = {
    0x07230203,0x00010000,0x000d000b,0x0000001f,
    0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0009000f,0x00000000,0x00000004,0x6e69616d,
    0x00000000,0x0000000d,0x00000012,0x0000001b,
    0x0000001d,0x00030003,0x00000002,0x000001c2,
    0x000a0004,0x475f4c47,0x4c474f4f,0x70635f45,
    0x74735f70,0x5f656c79,0x656e696c,0x7269645f,
    0x69746365,0x00006576,0x00080004,0x475f4c47,
    0x4c474f4f,0x6e695f45,0x64756c63,0x69645f65,
    0x74636572,0x00657669,0x00040005,0x00000004,
    0x6e69616d,0x00000000,0x00060005,0x0000000b,
    0x505f6c67,0x65567265,0x78657472,0x00000000,
    0x00060006,0x0000000b,0x00000000,0x505f6c67,
    0x7469736f,0x006e6f69,0x00070006,0x0000000b,
    0x00000001,0x505f6c67,0x746e696f,0x657a6953,
    0x00000000,0x00070006,0x0000000b,0x00000002,
    0x435f6c67,0x4470696c,0x61747369,0x0065636e,
    0x00070006,0x0000000b,0x00000003,0x435f6c67,
    0x446c6c75,0x61747369,0x0065636e,0x00030005,
    0x0000000d,0x00000000,0x00050005,0x00000012,
    0x6f506e69,0x69746973,0x00006e6f,0x00050005,
    0x0000001b,0x67617266,0x6f6c6f43,0x00000072,
    0x00040005,0x0000001d,0x6f436e69,0x00726f6c,
    0x00030047,0x0000000b,0x00000002,0x00050048,
    0x0000000b,0x00000000,0x0000000b,0x00000000,
    0x00050048,0x0000000b,0x00000001,0x0000000b,
    0x00000001,0x00050048,0x0000000b,0x00000002,
    0x0000000b,0x00000003,0x00050048,0x0000000b,
    0x00000003,0x0000000b,0x00000004,0x00040047,
    0x00000012,0x0000001e,0x00000000,0x00040047,
    0x0000001b,0x0000001e,0x00000000,0x00040047,
    0x0000001d,0x0000001e,0x00000001,0x00020013,
    0x00000002,0x00030021,0x00000003,0x00000002,
    0x00030016,0x00000006,0x00000020,0x00040017,
    0x00000007,0x00000006,0x00000004,0x00040015,
    0x00000008,0x00000020,0x00000000,0x0004002b,
    0x00000008,0x00000009,0x00000001,0x0004001c,
    0x0000000a,0x00000006,0x00000009,0x0006001e,
    0x0000000b,0x00000007,0x00000006,0x0000000a,
    0x0000000a,0x00040020,0x0000000c,0x00000003,
    0x0000000b,0x0004003b,0x0000000c,0x0000000d,
    0x00000003,0x00040015,0x0000000e,0x00000020,
    0x00000001,0x0004002b,0x0000000e,0x0000000f,
    0x00000000,0x00040017,0x00000010,0x00000006,
    0x00000003,0x00040020,0x00000011,0x00000001,
    0x00000010,0x0004003b,0x00000011,0x00000012,
    0x00000001,0x0004002b,0x00000006,0x00000014,
    0x3f800000,0x00040020,0x00000019,0x00000003,
    0x00000007,0x0004003b,0x00000019,0x0000001b,
    0x00000003,0x00040020,0x0000001c,0x00000001,
    0x00000007,0x0004003b,0x0000001c,0x0000001d,
    0x00000001,0x00050036,0x00000002,0x00000004,
    0x00000000,0x00000003,0x000200f8,0x00000005,
    0x0004003d,0x00000010,0x00000013,0x00000012,
    0x00050051,0x00000006,0x00000015,0x00000013,
    0x00000000,0x00050051,0x00000006,0x00000016,
    0x00000013,0x00000001,0x00050051,0x00000006,
    0x00000017,0x00000013,0x00000002,0x00070050,
    0x00000007,0x00000018,0x00000015,0x00000016,
    0x00000017,0x00000014,0x00050041,0x00000019,
    0x0000001a,0x0000000d,0x0000000f,0x0003003e,
    0x0000001a,0x00000018,0x0004003d,0x00000007,
    0x0000001e,0x0000001d,0x0003003e,0x0000001b,
    0x0000001e,0x000100fd,0x00010038
};

// Fragment SPIR-V compiled from GLSL (inColor -> outColor)
static const uint32_t kTestFragmentSpv[] = {
    0x07230203,0x00010000,0x000d000b,0x0000000d,
    0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,
    0x00000000,0x00000009,0x0000000b,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,
    0x000001c2,0x000a0004,0x475f4c47,0x4c474f4f,
    0x70635f45,0x74735f70,0x5f656c79,0x656e696c,
    0x7269645f,0x69746365,0x00006576,0x00080004,
    0x475f4c47,0x4c474f4f,0x6e695f45,0x64756c63,
    0x69645f65,0x74636572,0x00657669,0x00040005,
    0x00000004,0x6e69616d,0x00000000,0x00050005,
    0x00000009,0x4374756f,0x726f6c6f,0x00000000,
    0x00050005,0x0000000b,0x67617266,0x6f6c6f43,
    0x00000072,0x00040047,0x00000009,0x0000001e,
    0x00000000,0x00040047,0x0000000b,0x0000001e,
    0x00000000,0x00020013,0x00000002,0x00030021,
    0x00000003,0x00000002,0x00030016,0x00000006,
    0x00000020,0x00040017,0x00000007,0x00000006,
    0x00000004,0x00040020,0x00000008,0x00000003,
    0x00000007,0x0004003b,0x00000008,0x00000009,
    0x00000003,0x00040020,0x0000000a,0x00000001,
    0x00000007,0x0004003b,0x0000000a,0x0000000b,
    0x00000001,0x00050036,0x00000002,0x00000004,
    0x00000000,0x00000003,0x000200f8,0x00000005,
    0x0004003d,0x00000007,0x0000000c,0x0000000b,
    0x0003003e,0x00000009,0x0000000c,0x000100fd,
    0x00010038
};

struct TestVertex {
    float pos[3];
    float color[4];
};

int main() {
    std::cout << "=== bro_vulkan_scene_test: scene Vulkan core ===" << std::endl;

    // Initialize Headless VulkanContext
    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    cfg.enableDynamicRendering = true;

    render::VulkanContext context(cfg);
    bool ctxOk = context.init();
    if (!ctxOk) {
        std::cerr << "FAILED: Could not initialize VulkanContext" << std::endl;
        return 1;
    }

    // 1. Test SceneVkDevice
    std::cout << "[Test 1] SceneVkDevice Initialization & Command Sync... " << std::flush;
    SceneVkDevice device(context);
    bool devOk = device.init();
    assert(devOk);
    assert(device.isInitialized());
    assert(device.device() == context.device());

    // Upload stream: a command buffer from the frame, submitted on flush.
    VkCommandBuffer uploadCmd = device.uploadCommands();
    assert(uploadCmd != VK_NULL_HANDLE);
    assert(uploadCmd == device.uploadCommands());
    device.flushUploads();

    // Frame submission returns through the queue's tickets.
    VkCommandBuffer cmd1 = device.beginFrame();
    assert(cmd1 != VK_NULL_HANDLE);
    bool subOk = device.submitFrame(cmd1);
    assert(subOk);
    assert(device.lastFrameTicket() != 0);
    context.queue().wait(device.lastFrameTicket());
    assert(context.queue().isComplete(device.lastFrameTicket()));
    std::cout << "PASSED" << std::endl;

    // 2. Test SceneVkAllocator (Buffers, Uniforms, Images, Mipmaps)
    std::cout << "[Test 2] SceneVkAllocator Buffers, Transfers & Mipmapped Texture... " << std::flush;
    SceneVkAllocator allocator(device);

    // Vertex Buffer
    std::vector<TestVertex> testVerts = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{ 0.0f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    };
    SceneVkBuffer vertexBuffer;
    bool vbOk = allocator.createVertexBuffer(testVerts.size() * sizeof(TestVertex), testVerts.data(), vertexBuffer);
    assert(vbOk);
    assert(vertexBuffer.isValid());

    // Index Buffer
    std::vector<uint16_t> testIndices = {0, 1, 2};
    SceneVkBuffer indexBuffer;
    bool ibOk = allocator.createIndexBuffer(testIndices.size() * sizeof(uint16_t), testIndices.data(), indexBuffer);
    assert(ibOk);
    assert(indexBuffer.isValid());

    // Per-frame uniforms live in the frame's upload memory.
    SceneCameraUniforms camData{};
    camData.viewport[0] = 1280.0f;
    camData.viewport[1] = 720.0f;
    VkDescriptorBufferInfo cameraUbo = device.frameUniform(&camData, sizeof(SceneCameraUniforms));
    assert(cameraUbo.buffer != VK_NULL_HANDLE);
    assert(cameraUbo.range == sizeof(SceneCameraUniforms));

    // Mipmapped Texture (32x32 RGBA)
    const uint32_t texW = 32, texH = 32;
    std::vector<uint32_t> testPixels(texW * texH, 0xFF00FF00); // Green
    TextureDesc texDesc;
    texDesc.width = texW;
    texDesc.height = texH;
    texDesc.generateMipmaps = true;

    SceneVkImage texture;
    bool texOk = allocator.createTexture2D(testPixels.data(), texDesc, texture);
    assert(texOk);
    assert(texture.isValid());
    assert(texture.mipLevels == 6); // log2(32) + 1 = 6
    assert(texture.sampler != VK_NULL_HANDLE);
    assert(texture.currentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Test pooled chunk sub-allocation: create 100 buffers and verify they are sub-allocated in blocks
    std::vector<SceneVkBuffer> pooledBuffers(100);
    for (int i = 0; i < 100; ++i) {
        bool ok = allocator.createBuffer(1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         pooledBuffers[i]);
        assert(ok);
        assert(pooledBuffers[i].isValid());
        assert(pooledBuffers[i].allocId != 0);
    }
    SceneVkAllocatorStats statsAfter100 = allocator.stats();
    assert(statsAfter100.activeAllocationCount >= 100);
    // 100 allocations of 1KB must be pooled into 1-2 blocks, NOT 100 separate VkDeviceMemory handles!
    assert(statsAfter100.activeBlockCount < 5);

    for (int i = 0; i < 100; ++i) {
        allocator.destroyBuffer(pooledBuffers[i]);
    }

    std::cout << "PASSED (Mip levels: " << texture.mipLevels << ", Block sub-allocation verified: 100 buffers in "
              << statsAfter100.activeBlockCount << " chunk(s))" << std::endl;

    // 3. Test Descriptors
    std::cout << "[Test 3] SceneVkDescriptors (Layouts, Pools, Frame Sets & Updates)... " << std::flush;
    VkDescriptorSetLayout camLayout = SceneVkDescriptorLayoutBuilder::createCameraLayout(device.device());
    assert(camLayout != VK_NULL_HANDLE);

    VkDescriptorSetLayout matLayout = SceneVkDescriptorLayoutBuilder::createMaterialLayout(device.device(), 1);
    assert(matLayout != VK_NULL_HANDLE);

    SceneVkDescriptorPool descPool;
    bool poolOk = descPool.init(device.device(), 64);
    assert(poolOk);

    VkDescriptorSet camSet = descPool.allocate(camLayout);
    assert(camSet != VK_NULL_HANDLE);

    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, cameraUbo.buffer, cameraUbo.range, cameraUbo.offset);
    writer.updateSet(device.device(), camSet);

    VkDescriptorSet matSet = descPool.allocate(matLayout);
    assert(matSet != VK_NULL_HANDLE);

    writer.clear();
    writer.writeImage(0, texture.view, texture.sampler);
    writer.updateSet(device.device(), matSet);

    // Per-frame sets from the frame's descriptor arena
    VkDescriptorSet frameSet = device.frameSet(camLayout);
    assert(frameSet != VK_NULL_HANDLE);

    std::cout << "PASSED" << std::endl;

    // 4. Test Graphics Pipeline Builder with Vulkan 1.3 Dynamic Rendering
    std::cout << "[Test 4] SceneVkPipelineBuilder (SPIR-V & Dynamic Rendering)... " << std::flush;
    VkShaderModule vsModule = SceneVkShaderModule::create(device.device(), kTestVertexSpv, sizeof(kTestVertexSpv));
    VkShaderModule fsModule = SceneVkShaderModule::create(device.device(), kTestFragmentSpv, sizeof(kTestFragmentSpv));
    assert(vsModule != VK_NULL_HANDLE);
    assert(fsModule != VK_NULL_HANDLE);

    // Pipeline Layout
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &pipelineLayout);
    assert(pipelineLayout != VK_NULL_HANDLE);

    // Vertex input description
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(TestVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrDescs(2);
    // Location 0: pos (vec3)
    attrDescs[0].location = 0;
    attrDescs[0].binding = 0;
    attrDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDescs[0].offset = offsetof(TestVertex, pos);
    // Location 1: color (vec4)
    attrDescs[1].location = 1;
    attrDescs[1].binding = 0;
    attrDescs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrDescs[1].offset = offsetof(TestVertex, color);

    SceneVkPipelineBuilder pipelineBuilder;
    pipelineBuilder.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsModule)
                   .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fsModule)
                   .setVertexInput({bindingDesc}, attrDescs)
                   .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                   .setCullMode(VK_CULL_MODE_NONE)
                   .disableBlending(1)
                   .enableDepthTest(true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                   .setTarget({{VK_FORMAT_R8G8B8A8_UNORM}, 1, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT});

    VkPipeline testPipeline = pipelineBuilder.build(device.device(), pipelineLayout);
    assert(testPipeline != VK_NULL_HANDLE);

    std::cout << "PASSED" << std::endl;

    // 5. Dynamic rendering into allocator images, layouts tracked by the frame graph
    std::cout << "[Test 5] Offscreen Dynamic Rendering & Pixel Check... " << std::flush;
    const uint32_t renderW = 64, renderH = 64;
    SceneVkImage color, depth;
    bool rtOk = allocator.createImage(renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, color) &&
                allocator.createImage(renderW, renderH, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depth, 1, VK_SAMPLE_COUNT_1_BIT,
                                      VK_IMAGE_ASPECT_DEPTH_BIT);
    assert(rtOk);

    // Record dynamic rendering pass
    VkCommandBuffer cmd = device.beginFrame();
    SceneFrameGraph::transition(cmd, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    SceneFrameGraph::transition(cmd, depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = color.view;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea = {{0, 0}, {renderW, renderH}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAtt;
    renderInfo.pDepthAttachment = &depthAtt;
    device.cmdBeginRendering(cmd, &renderInfo);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(renderW), static_cast<float>(renderH), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {renderW, renderH}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, testPipeline);

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.buffer, offsets);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    device.cmdEndRendering(cmd);

    // Transition color image to TRANSFER_SRC_OPTIMAL to read back pixels
    SceneFrameGraph::transition(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Staging buffer for readback
    VkDeviceSize readbackSize = renderW * renderH * 4;
    SceneVkBuffer readbackBuffer;
    allocator.createBuffer(readbackSize,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           readbackBuffer);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {renderW, renderH, 1};

    vkCmdCopyImageToBuffer(cmd, color.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer.buffer, 1, &copyRegion);

    assert(device.submitFrame(cmd));
    device.waitIdle();

    // Verify readback pixels
    const uint8_t* pixels = nullptr;
    if (readbackBuffer.mappedData) {
        pixels = static_cast<const uint8_t*>(readbackBuffer.mappedData);
    } else {
        void* mappedPixels = nullptr;
        vkMapMemory(device.device(), readbackBuffer.memory, readbackBuffer.offset, readbackSize, 0, &mappedPixels);
        assert(mappedPixels != nullptr);
        pixels = static_cast<const uint8_t*>(mappedPixels);
    }

    // Center pixel (32, 32) is inside the triangle -> Should be GREEN (R=0, G=255, B=0, A=255)
    size_t centerIdx = (32 * renderW + 32) * 4;
    uint8_t rCenter = pixels[centerIdx + 0];
    uint8_t gCenter = pixels[centerIdx + 1];
    uint8_t bCenter = pixels[centerIdx + 2];
    uint8_t aCenter = pixels[centerIdx + 3];
    assert(gCenter == 255);
    assert(rCenter == 0);
    assert(bCenter == 0);
    assert(aCenter == 255);

    // Top-left pixel (0, 0) is background -> Should be RED (R=255, G=0, B=0, A=255)
    uint8_t rBg = pixels[0];
    uint8_t gBg = pixels[1];
    uint8_t bBg = pixels[2];
    uint8_t aBg = pixels[3];
    assert(rBg == 255);
    assert(gBg == 0);
    assert(bBg == 0);
    assert(aBg == 255);

    if (!readbackBuffer.mappedData) {
        vkUnmapMemory(device.device(), readbackBuffer.memory);
    }
    allocator.destroyBuffer(readbackBuffer);
    std::cout << "PASSED (Triangle & background pixels verified)" << std::endl;

    // 6. The frame targets: sizes, sample-count clamp, the shadow atlas
    std::cout << "[Test 6] SceneTargets (frame images, MSAA clamp, shadow atlas)... " << std::flush;
    SceneTargets targets;
    assert(targets.setup(device, allocator));
    assert(targets.shadowAtlas.isValid() && targets.shadowAtlas.width == 1);
    assert(targets.ensureShadowAtlas(allocator, 512) && targets.shadowAtlas.width == 512);
    assert(targets.ensureShadowAtlas(allocator, targets.maxShadowAtlas() + 1) &&
           targets.shadowAtlas.width == targets.maxShadowAtlas());
    assert(targets.ensureShadowAtlas(allocator, 256) && targets.shadowAtlas.height == 256);
    assert(device.shadowCompareSampler() != VK_NULL_HANDLE);
    const VkSampleCountFlagBits samples = targets.supportedSamples(4);
    assert(samples >= VK_SAMPLE_COUNT_1_BIT && samples <= VK_SAMPLE_COUNT_4_BIT);
    assert(targets.supportedSamples(0) == VK_SAMPLE_COUNT_1_BIT);
    assert(targets.ensure(allocator, 32, 16, samples));
    assert(targets.valid() && targets.width() == 32 && targets.height() == 16);
    assert(targets.hdr.width == 32 && targets.ldr.height == 16);
    assert(targets.msaa() == (samples != VK_SAMPLE_COUNT_1_BIT));
    assert(!targets.ensure(allocator, 32, 16, samples));   // unchanged: nothing recreated
    assert(targets.ensureIndirect(allocator) && targets.indirect.isValid());
    const TargetFormat hdr = targets.hdrTarget(true);
    assert(hdr.colorCount == 2 && hdr.samples == samples && hdr.depth == SceneTargets::kDepthFormat);
    assert(targets.hdrTarget(false).key() != hdr.key());
    targets.cleanup(allocator);
    std::cout << "PASSED" << std::endl;

    // Clean up
    allocator.destroyImage(color);
    allocator.destroyImage(depth);
    vkDestroyPipeline(device.device(), testPipeline, nullptr);
    vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
    SceneVkShaderModule::destroy(device.device(), vsModule);
    SceneVkShaderModule::destroy(device.device(), fsModule);
    descPool.destroy();
    vkDestroyDescriptorSetLayout(device.device(), camLayout, nullptr);
    vkDestroyDescriptorSetLayout(device.device(), matLayout, nullptr);
    allocator.destroyImage(texture);
    allocator.destroyBuffer(vertexBuffer);
    allocator.destroyBuffer(indexBuffer);
    device.shutdown();

    if (const uint32_t errors = render::vulkanValidationErrorCount()) {
        std::cerr << "FAILED: " << errors << " Vulkan validation error(s)" << std::endl;
        return 1;
    }
    std::cout << "=== bro_vulkan_scene_test: all checks passed ===" << std::endl;
    return 0;
}
