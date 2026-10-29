// bro_vulkan_scene_test: the scene's Vulkan building blocks — device, allocator,
// pipeline builder, descriptors, render targets — drawing and reading back.
//
// assert() is the check here, so it must survive a Release build: NDEBUG is
// undefined before any header can pull in <cassert>. Run under
// BRO_VK_VALIDATION=1 (tests/run_tests.sh does) and any validation error
// fails the run too.
#undef NDEBUG

#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_frame_graph.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/shaders/shadow/shadow_pages_layout.h"
#include "scene/vulkan/world/scene_world_service.h"
#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "render/vulkan_debug.h"
#include "util/log.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

    // 6. The frame targets: sizes, sample-count clamp
    std::cout << "[Test 6] SceneTargets (frame images, MSAA clamp)... " << std::flush;
    SceneTargets targets;
    assert(targets.setup(device, allocator));
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
    assert(SceneTargets::carriesIndirect(hdr));
    const TargetFormat plain = targets.hdrTarget(false);
    assert(plain.colorCount == 1 && !SceneTargets::carriesIndirect(plain));
    assert(plain.key() != hdr.key());
    targets.cleanup(allocator);
    std::cout << "PASSED" << std::endl;

    // Every built-in shader compiled from the source tree (what
    // BRO_SCENE_SHADER_SOURCE does at run time) is word for word the SPIR-V
    // the build embedded, variants and their DEFINES included, and makes a
    // module the device (and the validation layer) accepts.
    std::cout << "[Test] Built-in shaders from source match the embedded SPIR-V... " << std::flush;
    {
        const std::string dir = SceneVkShaderCompiler::buildShaderSourceDir();
        assert(!dir.empty());
        for (int i = 0; i < static_cast<int>(BuiltinSceneShader::Count); ++i) {
            const auto shader = static_cast<BuiltinSceneShader>(i);
            const auto& embedded = SceneVkShaderCompiler::embeddedSpirv(shader);
            std::string err;
            const auto fromSource = SceneVkShaderCompiler::compileBuiltinFromSource(shader, dir, &err);
            if (fromSource != embedded) {
                std::cerr << "\nFAILED: " << SceneVkShaderCompiler::builtinName(shader) << ": "
                          << (fromSource.empty() ? "did not compile: " + err : "differs from the embedded SPIR-V")
                          << " (" << fromSource.size() << " vs " << embedded.size() << " words)" << std::endl;
                return 1;
            }
            VkShaderModule module = SceneVkShaderCompiler::createModule(device.device(), embedded);
            assert(module != VK_NULL_HANDLE);
            SceneVkShaderCompiler::destroyModule(device.device(), module);
        }
    }
    std::cout << "PASSED (" << static_cast<int>(BuiltinSceneShader::Count) << " shaders)" << std::endl;

    // The world service's compute path: a compute kind's granted jobs record
    // into the frame's compute stream (the async compute queue where the
    // device has one, else the graphics stream at the same point), the
    // frame's graphics waits for them, what they wrote is there, and their
    // span teaches the policy the kind's cost.
    std::cout << "[Test] World service: compute jobs on the compute stream... " << std::flush;
    {
        SceneGpuTimer timer(context);
        const bool timed = timer.init();
        SceneWorldService world;
        const int kind = world.addKind(nullptr, {"test-compute", scene::WorldSchedule::Compute, 0.01f});
        assert(kind == 0);

        constexpr uint32_t kCount = 64;
        SceneVkBuffer out;
        assert(allocator.createBuffer(kCount * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, out,
                                      true));
        assert(out.mappedData);
        const char* src =
            "#version 450\n"
            "layout(local_size_x = 64) in;\n"
            "layout(set = 0, binding = 0) buffer Out { uint v[]; } o;\n"
            "layout(push_constant) uniform P { uint base; } p;\n"
            "void main() { o.v[gl_GlobalInvocationID.x] = p.base + gl_GlobalInvocationID.x; }\n";
        const auto spirv = SceneVkShaderCompiler::compileGlsl(src, VK_SHADER_STAGE_COMPUTE_BIT);
        assert(!spirv.empty());
        VkShaderModule cs = SceneVkShaderCompiler::createModule(device.device(), spirv);
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                             nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &binding;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        assert(vkCreateDescriptorSetLayout(device.device(), &li, nullptr, &setLayout) == VK_SUCCESS);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        assert(vkCreatePipelineLayout(device.device(), &pli, nullptr, &layout) == VK_SUCCESS);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
                    "main", nullptr};
        ci.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        assert(vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) == VK_SUCCESS);

        bool apart = false;
        uint32_t lastBase = 0;
        for (uint32_t f = 1; f <= 6; ++f) {
            context.frames().beginFrame();
            VkCommandBuffer cmd = device.beginFrame();
            timer.begin(cmd);
            // A target no device misses: on a software rasterizer (CI's
            // lavapipe) the frames run far over 2.78 ms, the budget sits at
            // its floor and nothing is granted. The grants are the subject.
            world.beginFrame(&timer, 1000.0f);
            for (uint64_t k = 0; k < 4; ++k) world.policy().offer({k, 0, kind, 10.0f - k, 1.0, false});
            world.policy().schedule();
            assert(!world.policy().granted(kind).empty());
            VkCommandBuffer computeCmd = VK_NULL_HANDLE;
            WorldStreams streams{cmd,
                                 [&] {
                                     computeCmd = device.computeCommands(cmd);
                                     if (computeCmd != cmd) timer.beginCompute(computeCmd);
                                     return computeCmd;
                                 },
                                 &timer};
            const uint32_t base = f * 1000;
            const double jobs = world.run(streams, kind, [&](WorldStream& s) {
                const auto granted = world.policy().granted(kind);
                VkCommandBuffer c = s.cmd();
                apart = s.apart();
                VkDescriptorSet set = device.frameSet(setLayout);
                assert(set != VK_NULL_HANDLE);
                VkDescriptorBufferInfo bi{out.buffer, 0, kCount * sizeof(uint32_t)};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = set;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &bi;
                vkUpdateDescriptorSets(device.device(), 1, &w, 0, nullptr);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                // (Each granted job one dispatch over the buffer, the last's values kept.)
                for (size_t j = 0; j < granted.size(); ++j) {
                    vkCmdPushConstants(c, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &base);
                    vkCmdDispatch(c, 1, 1, 1);
                    render::cmdMemoryBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
                }
                render::cmdMemoryBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                return static_cast<double>(granted.size());
            });
            assert(jobs == 4.0);
            lastBase = base;
            // The frame's graphics waits for its compute from here on.
            cmd = device.joinCompute(cmd);
            timer.end(cmd);
            if (apart) timer.endCompute(computeCmd);
            assert(device.submitFrame(cmd));
            timer.submitted(device.lastFrameTicket());
            context.queue().wait(device.lastFrameTicket());
        }
        assert(apart == device.asyncCompute());
        const auto* v = static_cast<const uint32_t*>(out.mappedData);
        for (uint32_t i = 0; i < kCount; ++i) assert(v[i] == lastBase + i);
        // The span reached the policy: the kind's cost is measured, where
        // the queue the jobs ran on writes timestamps.
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(), &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(), &familyCount, families.data());
        const int family = apart ? context.queueFamilies().computeFamily : context.queueFamilies().graphicsFamily;
        const bool stamps = timed && family >= 0 && families[static_cast<size_t>(family)].timestampValidBits > 0;
        // A device whose stamps read no time across the jobs (the runners'
        // paravirtual Apple GPU) leaves the kind unmeasured, as the policy
        // takes no 0 ms sample: the last render's spans say which it was.
        int spans = 0;
        bool timeless = true;
        {
            std::vector<SceneGpuTimer::Frame> last;
            timer.takeFrames(last);
            for (const SceneGpuTimer::Frame& fr : last)
                for (const SceneGpuTimer::Frame::Span& s : fr.spans) {
                    ++spans;
                    if (s.ms > 0.0) timeless = false;
                }
        }
        const bool readNoTime = stamps && spans > 0 && timeless;
        assert(world.policy().measured(kind) == (stamps && !readNoTime));
        std::cout << "PASSED (" << (apart ? "async compute queue" : "graphics stream") << ", a job "
                  << world.policy().cost(kind) << " ms"
                  << (readNoTime ? " unmeasured: the stamps read no time" : stamps ? " measured" : " unmeasured") << ")"
                  << std::endl;

        vkDestroyPipeline(device.device(), pipeline, nullptr);
        vkDestroyPipelineLayout(device.device(), layout, nullptr);
        vkDestroyDescriptorSetLayout(device.device(), setLayout, nullptr);
        SceneVkShaderCompiler::destroyModule(device.device(), cs);
        allocator.destroyBuffer(out);
    }

    // Shader set B (scene_shader_sets.h): a pipeline made before set B is
    // loaded gets its twin from B's tree; switching sets binds it, with no
    // compile; an identical tree twins nothing.
    std::cout << "[Test] Shader set B: twins, switch, identical tree... " << std::flush;
    {
        namespace fs = std::filesystem;
        const std::string srcDir = SceneVkShaderCompiler::buildShaderSourceDir();
        const fs::path tmp = fs::temp_directory_path() / "bro_shader_set_test";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::copy(srcDir, tmp, fs::copy_options::recursive, ec);
        assert(!ec);
        const auto setSide = [&](const char* from, const char* to) {
            const fs::path f = tmp / "shadow" / "shadow_pages_layout.h";
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            in.close();
            std::string text = ss.str();
            const size_t at = text.find(from);
            assert(at != std::string::npos);
            text.replace(at, std::strlen(from), to);
            std::ofstream(f, std::ios::binary | std::ios::trunc) << text;
        };

        // A run-time compute shader that reads a constant of the tree's.
        const char* src =
            "#version 450\n"
            "#include \"shadow/shadow_pages_layout.h\"\n"
            "layout(local_size_x = 1) in;\n"
            "layout(set = 0, binding = 0) buffer Out { uint v[]; } o;\n"
            "void main() { o.v[0] = uint(SP_LOCAL_ENTRIES); }\n";
        const auto spirv = SceneVkShaderCompiler::compileGlsl(src, VK_SHADER_STAGE_COMPUTE_BIT);
        assert(!spirv.empty());
        VkShaderModule cs = SceneVkShaderCompiler::createModule(device.device(), spirv);
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                             nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &binding;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        assert(vkCreateDescriptorSetLayout(device.device(), &li, nullptr, &setLayout) == VK_SUCCESS);
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        assert(vkCreatePipelineLayout(device.device(), &pli, nullptr, &layout) == VK_SUCCESS);
        VkPipeline pipeline = createComputePipeline(device.device(), cs, layout, "shader set test");
        assert(pipeline != VK_NULL_HANDLE);
        SceneVkShaderCompiler::destroyModule(device.device(), cs);   // (the twin is made from the recipe)

        SceneVkBuffer out;
        assert(allocator.createBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, out,
                                      true));
        const auto run = [&]() -> uint32_t {
            context.frames().beginFrame();
            VkCommandBuffer cmd = device.beginFrame();
            VkDescriptorSet set = device.frameSet(setLayout);
            VkDescriptorBufferInfo bi{out.buffer, 0, sizeof(uint32_t)};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(device.device(), 1, &w, 0, nullptr);
            bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, 1, 1, 1);
            render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            assert(device.submitFrame(cmd));
            context.queue().wait(device.lastFrameTicket());
            return *static_cast<const uint32_t*>(out.mappedData);
        };
        assert(run() == SP_LOCAL_ENTRIES);
        assert(!useShaderSet(1));   // (no set B yet)

        // The identical tree: nothing differs, nothing twinned.
        ShaderSetInfo info;
        std::string err;
        assert(loadShaderSetB(tmp.string(), &info, &err));
        assert(info.differing == 0 && info.twinned == 0 && info.failed.empty());
        assert(useShaderSet(1) && run() == SP_LOCAL_ENTRIES);
        assert(useShaderSet(0));

        // The constant changed in B's tree: the pipeline twinned, the switch
        // binds it, back to A binds A's.
        setSide("#define SP_LOCAL_ENTRIES        85", "#define SP_LOCAL_ENTRIES        77");
        assert(loadShaderSetB(tmp.string(), &info, &err));
        assert(info.twinned >= 1 && info.differing >= 1 && info.failed.empty());
        assert(run() == SP_LOCAL_ENTRIES);
        assert(useShaderSet(1));
        assert(run() == 77u);
        assert(useShaderSet(0) && run() == SP_LOCAL_ENTRIES);
        assert(!loadShaderSetB((tmp / "none").string(), &info, &err) && !err.empty());

        const int twinned = shaderSetInfo().twinned;
        destroyScenePipeline(device.device(), pipeline, nullptr);   // (and its twin)
        assert(shaderSetInfo().twinned == twinned - 1);
        vkDestroyPipelineLayout(device.device(), layout, nullptr);
        vkDestroyDescriptorSetLayout(device.device(), setLayout, nullptr);
        allocator.destroyBuffer(out);
        fs::remove_all(tmp, ec);
    }
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
