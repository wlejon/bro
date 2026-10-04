#pragma once

#include "render/vulkan_context.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target.h"
#include "scene/vulkan/pass_mesh.h"
#include "scene/vulkan/pass_shadow.h"
#include "scene/vulkan/pass_environment.h"
#include "scene/vulkan/pass_postfx.h"
#include <bromesh/mesh_data.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::scene {
class SceneGraph;
class SceneRenderer;
class MeshNode;
class InstancedMeshNode;
class SkinnedMeshNode;
class LightNode;
}

namespace bro::scene::vk {

class SceneVkBridge {
public:
    explicit SceneVkBridge(render::VulkanContext& context);
    ~SceneVkBridge();

    SceneVkBridge(const SceneVkBridge&) = delete;
    SceneVkBridge& operator=(const SceneVkBridge&) = delete;

    bool init();

    void render3D(SceneGraph& graph, SceneRenderer& renderer);

    bool hasMeshContent() const { return hasMeshContent_; }
    uint32_t finalColorTextureId() const { return finalColorTextureId_; }

    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH);

private:
    bool ensureTargets(uint32_t width, uint32_t height);

    struct CachedMeshBuffer {
        SceneVkBuffer vertexBuffer;
        SceneVkBuffer indexBuffer;
        uint32_t indexCount = 0;
        size_t vertexCount = 0;
        uint64_t meshHash = 0;
    };
    CachedMeshBuffer& uploadMesh(const bromesh::MeshData& mesh, const void* key);

    struct NodeDynamicBuffers {
        SceneVkBuffer instanceBuffer;
        size_t instanceCapacity = 0;

        SceneVkBuffer skinAttribBuffer;
        size_t skinAttribCapacity = 0;

        SceneVkBuffer boneUbo;
        VkDescriptorSet boneSet = VK_NULL_HANDLE;
        VkDescriptorSet boneSetShadow = VK_NULL_HANDLE;
    };
    NodeDynamicBuffers& getDynamicBuffers(const void* key);

    struct CachedTexture {
        SceneVkImage image;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
    };
    VkDescriptorSet uploadTexture(const void* key, int width, int height, const uint8_t* rgba);

    render::VulkanContext& context_;
    SceneVkDevice device_;
    SceneVkAllocator allocator_;

    PassShadow passShadow_;
    PassEnvironment passEnv_;
    PassMesh passMesh_;
    PassPostFx passPostFx_;

    SceneVkShadowCascadeTarget shadowTarget_;
    SceneVkRenderTarget hdrTarget_;
    SceneVkImage ldrPresentationImage_;
    SceneVkBuffer readbackBuffer_;

    SceneVkBuffer cameraUbo_;
    SceneVkBuffer lightingUbo_;
    SceneVkDescriptorPool mainDescPool_;
    VkDescriptorSet cameraSet_ = VK_NULL_HANDLE;
    VkDescriptorSet lightingSet_ = VK_NULL_HANDLE;

    SceneVkDescriptorPool dynamicDescPool_;

    std::unordered_map<const void*, CachedMeshBuffer> meshCache_;
    std::unordered_map<const void*, NodeDynamicBuffers> dynamicBufferCache_;
    std::unordered_map<const void*, CachedTexture> textureCache_;

    uint32_t currentWidth_ = 0;
    uint32_t currentHeight_ = 0;
    bool hasMeshContent_ = false;
    uint32_t finalColorTextureId_ = 1;
};

} // namespace bro::scene::vk
