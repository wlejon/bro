#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/pass_mesh.h"
#include "scene/reflection_probe_node.h"

#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>

namespace bro::scene {
class SceneGraph;
class SceneRenderer;
}

namespace bro::scene::vk {

class SceneVkBridge;

/// Reflection probe capturing pass that renders the 6 cubemap faces of active probes.
class PassReflectionProbe {
public:
    PassReflectionProbe() = default;
    ~PassReflectionProbe();

    PassReflectionProbe(const PassReflectionProbe&) = delete;
    PassReflectionProbe& operator=(const PassReflectionProbe&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    void updateProbes(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                      PassMesh& passMesh, SceneVkAllocator& allocator, SceneVkDevice& device,
                      SceneVkBridge& bridge);

    bool hasActiveProbe() const { return activeProbe_ != nullptr; }
    const ReflectionProbeNode* activeProbe() const { return activeProbe_; }

    VkImageView activeCubemapView() const;
    VkSampler activeCubemapSampler() const { return cubemapSampler_; }
    VkImageView dummyCubemapView() const { return dummyCubemap_.view; }

private:
    struct ProbeGpuData {
        SceneVkImage cubemap;
        std::vector<VkImageView> faceViews; // 6 individual face views
        SceneVkImage depthImage;
        int resolution = 0;
        uint32_t mipLevels = 1;
    };

    bool ensureProbeGpu(ReflectionProbeNode* probe, SceneVkAllocator& allocator, SceneVkDevice& device);
    void renderFace(VkCommandBuffer cmd, ReflectionProbeNode* probe, int face,
                    SceneGraph& graph, SceneRenderer& renderer, PassMesh& passMesh,
                    SceneVkAllocator& allocator, SceneVkDevice& device,
                    SceneVkBridge& bridge);

    SceneVkImage dummyCubemap_;
    VkSampler cubemapSampler_ = VK_NULL_HANDLE;

    std::unordered_map<const ReflectionProbeNode*, ProbeGpuData> probeCache_;
    const ReflectionProbeNode* activeProbe_ = nullptr;

    SceneVkBuffer faceCameraUbos_[6];
    SceneVkDescriptorPool faceDescPool_;
    VkDescriptorSet faceCameraSets_[6] = {};
    VkDescriptorSetLayout faceCameraLayout_ = VK_NULL_HANDLE;

    SceneVkBuffer faceLightingUbo_;
    VkDescriptorSet faceLightingSet_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout faceLightingLayout_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
