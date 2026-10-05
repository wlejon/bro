#include "scene/vulkan/scene_vk_bridge.h"

#include "render/vulkan_util.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/pass_billboard.h"
#include "scene/vulkan/pass_decal.h"
#include "scene/vulkan/pass_dof.h"
#include "scene/vulkan/pass_environment.h"
#include "scene/vulkan/pass_gaussian_splat.h"
#include "scene/vulkan/pass_mesh.h"
#include "scene/vulkan/pass_particles.h"
#include "scene/vulkan/pass_postfx.h"
#include "scene/vulkan/pass_reflection_probe.h"
#include "scene/vulkan/pass_shadow.h"
#include "scene/vulkan/pass_snapshot.h"
#include "scene/vulkan/pass_ssao.h"
#include "scene/vulkan/pass_ssr.h"
#include "scene/vulkan/pass_terrain.h"
#include "scene/vulkan/scene_draw_list.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_lighting.h"
#include "util/log.h"

namespace bro::scene::vk {

SceneVkBridge::SceneVkBridge(render::VulkanContext& context)
    : device_(context),
      allocator_(device_),
      resources_(allocator_),
      gpu_{device_, allocator_, defaults_, targets_, resources_, meshes_, environment_} {
    // The frame, in order. A pass draws into the HDR scope or brings its own
    // targets; the graph does every shared transition (scene_frame_graph.h).
    graph_.add(std::make_unique<PassShadow>());
    graph_.add(std::make_unique<PassFrameUniforms>());
    graph_.add(std::make_unique<PassReflectionProbe>());
    graph_.add(std::make_unique<PassEnvironment>());
    graph_.add(std::make_unique<PassOpaque>());
    graph_.add(std::make_unique<PassTerrain>());
    graph_.add(std::make_unique<PassDepthSnapshot>());
    auto& ssao = static_cast<PassSSAO&>(graph_.add(std::make_unique<PassSSAO>()));
    graph_.add(std::make_unique<PassAoApply>(ssao));
    graph_.add(std::make_unique<PassColorSnapshot>());
    graph_.add(std::make_unique<PassSSR>());
    graph_.add(std::make_unique<PassDecal>());
    graph_.add(std::make_unique<PassTranslucent>());
    graph_.add(std::make_unique<PassParticles>());
    graph_.add(std::make_unique<PassBillboard>());
    graph_.add(std::make_unique<PassGaussianSplat>());
    graph_.add(std::make_unique<PassDoF>());
    graph_.add(std::make_unique<PassPostFx>());
    graph_.add(std::make_unique<PassOverlay>());
    graph_.add(std::make_unique<PassTiltShift>());
    graph_.add(std::make_unique<PassFxaa>());
}

SceneVkBridge::~SceneVkBridge() {
    device_.waitIdle();
    allocator_.destroyBuffer(readbackBuffer_);
    graph_.cleanup(gpu_);
    meshes_.cleanup(gpu_);
    environment_.cleanup(device_, allocator_);
    resources_.cleanup();
    targets_.cleanup(allocator_);
    defaults_.cleanup(device_, allocator_);
    device_.shutdown();
}

bool SceneVkBridge::init() {
    if (!device_.init()) {
        LOG_ERROR("SceneVkBridge: Failed initializing the scene device");
        return false;
    }
    if (!defaults_.setup(device_, allocator_) || !targets_.setup(device_, allocator_) || !meshes_.setup(gpu_) ||
        !environment_.setup(device_, allocator_, defaults_) || !graph_.setup(gpu_)) {
        LOG_ERROR("SceneVkBridge: Failed setting up the scene renderer");
        return false;
    }
    ready_ = true;
    return true;
}

bool SceneVkBridge::render3D(SceneGraph& graph, SceneRenderer& renderer, CullStats& stats) {
    if (!ready_) return false;
    const uint32_t width = static_cast<uint32_t>(renderer.targetWidth());
    const uint32_t height = static_cast<uint32_t>(renderer.targetHeight());
    if (width == 0 || height == 0) return false;

    const VkSampleCountFlagBits samples = targets_.supportedSamples(renderer.msaaSamples());
    const bool resized = targets_.ensure(allocator_, width, height, samples);
    if (!targets_.valid()) return false;
    if (resized) graph_.resize(gpu_, width, height);

    ++renderSerial_;
    const bool readbackInline = readSinceRender_;
    readSinceRender_ = false;

    VkCommandBuffer cmd = device_.beginFrame();
    SceneFrame frame(cmd, gpu_, graph, renderer);
    frame.stats = stats;
    frame.view = SceneView::fromCamera(graph, width, height);
    if (renderer.shadowPlan().tileCount > 0 &&
        !targets_.ensureShadowAtlas(allocator_, static_cast<uint32_t>(renderer.shadowPlan().atlasSize))) {
        renderer.invalidateShadowCache();
    }
    environment_.update(gpu_, cmd, renderer);
    frame.lighting = sceneLighting(renderer, environment_);
    writeCameraSet(frame);
    frame.ssao = renderer.ssaoEnabled() && targets_.ensureIndirect(allocator_);
    frame.dof = renderer.depthOfFieldEnabled();
    frame.tilt = renderer.tiltShiftEnabled();
    frame.fxaa = renderer.fxaaEnabled();
    buildDrawLists(frame);

    graph_.run(frame);

    // The result stays on the GPU for the compositor; it is copied out for
    // the CPU only when someone read the previous render's pixels.
    if (readbackInline && ensureReadbackBuffer()) {
        recordReadback(cmd);
    } else {
        SceneFrameGraph::transition(cmd, targets_.ldr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (device_.submitFrame(cmd) && readbackInline && readbackBuffer_.isValid()) {
        readbackRecordedSerial_ = renderSerial_;
        readbackTicket_ = device_.lastFrameTicket();
    }
    stats = frame.stats;
    return frame.drewContent;
}

render::LayerImage SceneVkBridge::outputImage() const {
    const SceneVkImage& ldr = targets_.ldr;
    if (!ldr.isValid() || renderSerial_ == 0) return {};
    render::LayerImage out;
    out.image = ldr.image;
    out.view = ldr.view;
    out.sampler = ldr.sampler;
    out.layout = ldr.currentLayout;
    out.format = ldr.format;
    out.width = ldr.width;
    out.height = ldr.height;
    return out;
}

void SceneVkBridge::releaseNodes(std::span<const uint32_t> ids) {
    if (!ready_ || ids.empty()) return;
    resources_.releaseNodes(ids);
    graph_.releaseNodes(gpu_, ids);
}

bool SceneVkBridge::ensureReadbackBuffer() {
    const VkDeviceSize size = static_cast<VkDeviceSize>(targets_.width()) * targets_.height() * 4;
    if (readbackBuffer_.isValid() && readbackBuffer_.size >= size) return true;
    allocator_.destroyBuffer(readbackBuffer_);
    const auto& memProps = device_.context().memoryProperties();
    constexpr VkMemoryPropertyFlags kCached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags flags = render::findMemoryType(memProps, ~0u, kCached)
        ? kCached
        : VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!allocator_.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, flags, readbackBuffer_) ||
        !readbackBuffer_.mappedData) {
        LOG_ERROR("SceneVkBridge: Failed creating the readback buffer");
        allocator_.destroyBuffer(readbackBuffer_);
        return false;
    }
    return true;
}

// Copy the LDR result into the readback buffer, leaving the image sampleable.
void SceneVkBridge::recordReadback(VkCommandBuffer cmd) {
    SceneVkImage& ldr = targets_.ldr;
    SceneFrameGraph::transition(cmd, ldr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // The previous copy into the buffer (a frame the CPU never read) is done.
    render::cmdBufferBarrier(cmd, readbackBuffer_.buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = ldr.width;
    region.bufferImageHeight = ldr.height;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {ldr.width, ldr.height, 1};
    vkCmdCopyImageToBuffer(cmd, ldr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer_.buffer, 1, &region);
    render::cmdBufferBarrier(cmd, readbackBuffer_.buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    SceneFrameGraph::transition(cmd, ldr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

std::vector<uint8_t> SceneVkBridge::readTonemapPixelsRGBA(int& outW, int& outH) {
    outW = outH = 0;
    const SceneVkImage& ldr = targets_.ldr;
    if (renderSerial_ == 0 || !ldr.isValid()) return {};
    readSinceRender_ = true;

    if (pixelsSerial_ != renderSerial_) {
        if (readbackRecordedSerial_ != renderSerial_) {
            // Not predicted: copy now, in a submission of its own.
            if (!ensureReadbackBuffer()) return {};
            VkCommandBuffer cmd = device_.frames().beginCommands();
            recordReadback(cmd);
            readbackTicket_ = device_.frames().submit(cmd);
            readbackRecordedSerial_ = renderSerial_;
        }
        if (readbackTicket_ == 0 || !device_.context().queue().wait(readbackTicket_)) {
            LOG_ERROR("SceneVkBridge: waiting for the tonemap readback failed");
            return {};
        }
        const size_t size = static_cast<size_t>(ldr.width) * ldr.height * 4;
        const auto* src = static_cast<const uint8_t*>(readbackBuffer_.mappedData);
        pixels_.assign(src, src + size);
        pixelsSerial_ = renderSerial_;
    }
    outW = static_cast<int>(ldr.width);
    outH = static_cast<int>(ldr.height);
    return pixels_;
}

}  // namespace bro::scene::vk
