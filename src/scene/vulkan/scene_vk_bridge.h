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
#include "scene/vulkan/pass_billboard.h"
#include "scene/vulkan/pass_particles.h"
#include "scene/vulkan/pass_decal.h"
#include "scene/vulkan/pass_reflection_probe.h"
#include "scene/vulkan/pass_terrain.h"
#include "scene/vulkan/pass_color_lut.h"
#include "scene/vulkan/pass_ssao.h"
#include "scene/vulkan/pass_ssr.h"
#include "scene/vulkan/pass_dof.h"
#include "scene/vulkan/pass_gaussian_splat.h"
#include "scene/vulkan/scene_vk_custom_shader.h"
#include "scene/mesh_node.h"
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
class DecalNode;
class Particles3DNode;
class HtmlNode;
class SpriteNode;
class ShapeNode;
class GaussianSplatNode;
struct CustomShaderState;
struct CullStats;
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

    VkImage ldrImage() const { return ldrPresentationImage_.image; }
    VkImageLayout currentLayout() const { return ldrPresentationImage_.currentLayout; }
    uint32_t width() const { return currentWidth_; }
    uint32_t height() const { return currentHeight_; }

    /// The last render's tonemapped result as top-down RGBA. Waits for that
    /// render (its own ticket, never the device); the copy is recorded into
    /// the render itself once a caller has asked in the previous frame, so a
    /// compositor reading every frame pays no extra submission.
    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH);

private:
    friend class PassReflectionProbe;
    bool ensureTargets(uint32_t width, uint32_t height, VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT);

    // Per-frame camera / lighting (scene_vk_bridge_frame.cpp)
    SceneCameraUniforms buildCameraUniforms(SceneGraph& graph, SceneRenderer& renderer,
                                            uint32_t width, uint32_t height) const;
    SceneLightingUniforms buildLightingUniforms(SceneGraph& graph, SceneRenderer& renderer) const;
    void finishLightingUniforms(SceneGraph& graph, SceneLightingUniforms& lightUniforms);
    void writeFrameSets(const SceneCameraUniforms& cam, const SceneLightingUniforms& light);

    // Readback (scene_vk_bridge_frame.cpp)
    bool ensureReadbackBuffer();
    void recordReadback(VkCommandBuffer cmd);

    struct CachedMeshBuffer {
        SceneVkBuffer vertexBuffer;
        SceneVkBuffer indexBuffer;
        uint32_t indexCount = 0;
        size_t vertexCount = 0;
        uint64_t meshHash = 0;
    };
    CachedMeshBuffer& uploadMesh(const bromesh::MeshData& mesh, const void* key);

    // Per-node GPU data. Skin attributes live in a device-local buffer; the
    // instance matrices and bone palette are rewritten every render into the
    // frame's upload memory, with descriptor sets from the frame.
    struct NodeDynamicBuffers {
        SceneVkBuffer skinAttribBuffer;
        size_t skinAttribCapacity = 0;

        render::UploadSlice instances;
        VkDescriptorSet boneSet = VK_NULL_HANDLE;
        VkDescriptorSet boneSetShadow = VK_NULL_HANDLE;
    };
    NodeDynamicBuffers& getDynamicBuffers(const void* key);

    struct CachedTexture {
        SceneVkImage image;
        int width = 0;
        int height = 0;
        uint64_t hash = 0;
        bool owned = true;
        // A material set for this frame (sets never change once in use).
        VkDescriptorSet frameSet = VK_NULL_HANDLE;
        uint64_t frameSerial = 0;
    };
    /// Upload (when changed) and return a material set for this frame.
    VkDescriptorSet uploadTexture(const void* key, int width, int height, const uint8_t* rgba);
    VkDescriptorSet materialSetFor(CachedTexture& tex, VkImageView view, VkSampler sampler);

    void prepareDynamicBuffers(SceneGraph& graph);
    void renderShadowPass(VkCommandBuffer cmd, SceneGraph& graph, const SceneLightingUniforms& lightUniforms, CullStats& stats);
    void renderDecalsPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer, CullStats& stats, bool& hasDrawnMeshes);
    void renderParticlesPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer, CullStats& stats, bool& hasDrawnMeshes);
    void renderBillboardsPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer, CullStats& stats, bool& hasDrawnMeshes);

    render::VulkanContext& context_;
    SceneVkDevice device_;
    SceneVkAllocator allocator_;

    PassShadow passShadow_;
    PassEnvironment passEnv_;
    PassMesh passMesh_;
    PassPostFx passPostFx_;
    PassBillboard passBillboard_;
    PassParticles passParticles_;
    PassDecal passDecal_;
    PassReflectionProbe passReflectionProbe_;
    PassTerrain passTerrain_;
    PassColorLut passColorLut_;
    PassSSAO passSSAO_;
    PassSSR passSSR_;
    PassDoF passDoF_;
    PassGaussianSplat passGaussianSplat_;

    SceneVkShadowCascadeTarget shadowTarget_;
    SceneVkRenderTarget hdrTarget_;
    SceneVkImage depthCopyImage_;
    SceneVkImage ssrColorSnapshot_;
    SceneVkImage dofHdrImage_;
    SceneVkImage postLdrImage_;
    SceneVkImage ldrPresentationImage_;

    // CPU readback of ldrPresentationImage_ (see readTonemapPixelsRGBA).
    SceneVkBuffer readbackBuffer_;
    uint64_t renderSerial_ = 0;
    uint64_t readbackRecordedSerial_ = 0;  // render whose submission includes the copy
    uint64_t readbackTicket_ = 0;
    bool readSinceRender_ = false;
    uint64_t pixelsSerial_ = 0;
    std::vector<uint8_t> pixels_;

    SceneVkImage dummyShadeMap_;
    SceneVkImage shadeMapImage_;
    int shadeMapW_ = 0;
    int shadeMapH_ = 0;
    uint64_t shadeMapHash_ = 0;

    VkDescriptorSet uploadExternalSceneTexture(SceneVkBridge* srcBridge, const void* key);

    // This render's camera and lighting sets (frame descriptor arena).
    VkDescriptorSet cameraSet_ = VK_NULL_HANDLE;
    VkDescriptorSet lightingSet_ = VK_NULL_HANDLE;
    // Shade-map binding for this render, set by finishLightingUniforms.
    VkImageView shadeView_ = VK_NULL_HANDLE;
    VkSampler shadeSampler_ = VK_NULL_HANDLE;

    std::unordered_map<const void*, CachedMeshBuffer> meshCache_;
    std::unordered_map<const void*, NodeDynamicBuffers> dynamicBufferCache_;
    std::unordered_map<const void*, CachedTexture> textureCache_;

    struct CustomPipelineEntry {
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::vector<std::string> samplerNames;
        std::vector<std::pair<std::string, uint32_t>> uniformOffsets;
        uint32_t uboSize = 0;
    };
    std::unordered_map<std::string, CustomPipelineEntry> customMeshPipelines_;
    std::unordered_map<std::string, VkPipeline> customShadowPipelines_;

    std::unordered_map<std::string, CachedTexture> userTextureCache_;

    void prepareCustomShaderForNode(const void* key, const CustomShaderState* cs, uint32_t target, bool translucent,
                                    std::vector<MeshNode::UserTexture>& userTextures,
                                    VkPipeline& outPipeline, VkDescriptorSet& outSet);
    void prepareCustomShadowShaderForNode(const void* key, const CustomShaderState* cs, bool isSkinned,
                                          std::vector<MeshNode::UserTexture>& userTextures,
                                          VkPipeline& outPipeline, VkDescriptorSet& outSet);
    void renderGaussianSplatPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                                 CullStats& stats, const float* view, const float* proj,
                                 const float eye[3], uint32_t width, uint32_t height);
    void renderPostProcessing(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                              uint32_t width, uint32_t height);

    uint32_t currentWidth_ = 0;
    uint32_t currentHeight_ = 0;
    VkSampleCountFlagBits currentSampleCount_ = VK_SAMPLE_COUNT_1_BIT;
    bool hasMeshContent_ = false;
    uint32_t finalColorTextureId_ = 1;
};

} // namespace bro::scene::vk
