#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/light_node.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

struct alignas(16) PackedVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float color[4];
    float tangent[4];
};

SceneVkBridge::SceneVkBridge(render::VulkanContext& context)
    : context_(context), device_(context), allocator_(device_) {}

SceneVkBridge::~SceneVkBridge() {
    device_.waitIdle();

    for (auto& [k, buf] : meshCache_) {
        allocator_.destroyBuffer(buf.vertexBuffer);
        allocator_.destroyBuffer(buf.indexBuffer);
    }
    meshCache_.clear();

    for (auto& [k, dyn] : dynamicBufferCache_) {
        allocator_.destroyBuffer(dyn.instanceBuffer);
        allocator_.destroyBuffer(dyn.skinAttribBuffer);
        allocator_.destroyBuffer(dyn.boneUbo);
    }
    dynamicBufferCache_.clear();

    for (auto& [k, tex] : textureCache_) {
        allocator_.destroyImage(tex.image);
    }
    textureCache_.clear();

    dynamicDescPool_.destroy();
    mainDescPool_.destroy();

    allocator_.destroyBuffer(cameraUbo_);
    allocator_.destroyBuffer(lightingUbo_);
    allocator_.destroyBuffer(readbackBuffer_);
    allocator_.destroyImage(ldrPresentationImage_);

    hdrTarget_.cleanup(allocator_);
    shadowTarget_.cleanup(allocator_);

    passPostFx_.cleanup(device_, allocator_);
    passMesh_.cleanup(device_, allocator_);
    passEnv_.cleanup(device_, allocator_);
    passShadow_.cleanup(device_);
}

bool SceneVkBridge::init() {
    if (!device_.init()) {
        LOG_ERROR("SceneVkBridge: Failed initializing SceneVkDevice");
        return false;
    }

    if (!passShadow_.init(device_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassShadow");
        return false;
    }

    if (!passEnv_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassEnvironment");
        return false;
    }

    if (!passMesh_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassMesh");
        return false;
    }

    if (!allocator_.createUniformBuffer(sizeof(SceneCameraUniforms), cameraUbo_) ||
        !allocator_.createUniformBuffer(sizeof(SceneLightingUniforms), lightingUbo_)) {
        LOG_ERROR("SceneVkBridge: Failed allocating camera/lighting uniform buffers");
        return false;
    }

    if (!mainDescPool_.init(device_.device(), 16) ||
        !dynamicDescPool_.init(device_.device(), 128)) {
        LOG_ERROR("SceneVkBridge: Failed creating descriptor pools");
        return false;
    }

    cameraSet_ = mainDescPool_.allocate(passMesh_.cameraLayout());
    lightingSet_ = mainDescPool_.allocate(passMesh_.lightingLayout());

    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, cameraUbo_.buffer, sizeof(SceneCameraUniforms));
    camWriter.updateSet(device_.device(), cameraSet_);

    return true;
}

bool SceneVkBridge::ensureTargets(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;

    if (!shadowTarget_.isValid()) {
        if (!shadowTarget_.init(allocator_, 1024, 4, VK_FORMAT_D32_SFLOAT)) {
            LOG_ERROR("SceneVkBridge: Failed initializing shadow target");
            return false;
        }
        SceneVkDescriptorWriter lightWriter;
        lightWriter.writeBuffer(0, lightingUbo_.buffer, sizeof(SceneLightingUniforms));
        lightWriter.writeImage(1, shadowTarget_.arrayView(), shadowTarget_.shadowSampler());
        lightWriter.updateSet(device_.device(), lightingSet_);
    }

    if (currentWidth_ == width && currentHeight_ == height && hdrTarget_.isValid()) {
        return true;
    }

    currentWidth_ = width;
    currentHeight_ = height;

    hdrTarget_.cleanup(allocator_);
    SceneVkRenderTargetDesc hdrDesc{};
    hdrDesc.width = width;
    hdrDesc.height = height;
    hdrDesc.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    hdrDesc.depthFormat = VK_FORMAT_D32_SFLOAT;
    hdrDesc.hasColor = true;
    hdrDesc.hasDepth = true;
    if (!hdrTarget_.init(allocator_, hdrDesc)) {
        LOG_ERROR("SceneVkBridge: Failed creating HDR render target (%ux%u)", width, height);
        return false;
    }

    allocator_.destroyImage(ldrPresentationImage_);
    VkImageUsageFlags ldrUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!allocator_.createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM, ldrUsage,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ldrPresentationImage_)) {
        LOG_ERROR("SceneVkBridge: Failed creating LDR presentation image (%ux%u)", width, height);
        return false;
    }

    passPostFx_.cleanup(device_, allocator_);
    if (!passPostFx_.init(device_, allocator_, width, height)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassPostFx");
        return false;
    }

    allocator_.destroyBuffer(readbackBuffer_);
    if (!allocator_.createBuffer(width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                readbackBuffer_)) {
        LOG_ERROR("SceneVkBridge: Failed creating readback buffer");
        return false;
    }

    return true;
}

static uint64_t computeMeshHash(const bromesh::MeshData& mesh) {
    uint64_t h = 14695981039346656037ULL;
    h ^= mesh.positions.size();
    h *= 1099511628211ULL;
    h ^= mesh.indices.size();
    h *= 1099511628211ULL;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(mesh.positions.data());
    size_t byteCount = mesh.positions.size() * sizeof(float);
    if (byteCount <= 256) {
        for (size_t i = 0; i < byteCount; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    } else {
        for (size_t i = 0; i < 128; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        for (size_t i = byteCount - 128; i < byteCount; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

SceneVkBridge::CachedMeshBuffer& SceneVkBridge::uploadMesh(const bromesh::MeshData& mesh, const void* key) {
    auto& entry = meshCache_[key];
    size_t vc = mesh.vertexCount();
    size_t ic = !mesh.indices.empty() ? mesh.indices.size() : vc;
    uint64_t currentHash = computeMeshHash(mesh);

    if (entry.vertexCount == vc && entry.indexCount == ic && entry.meshHash == currentHash && entry.vertexBuffer.isValid()) {
        return entry;
    }

    allocator_.destroyBuffer(entry.vertexBuffer);
    allocator_.destroyBuffer(entry.indexBuffer);
    entry.meshHash = currentHash;

    std::vector<PackedVertex> vertices(vc);
    for (size_t i = 0; i < vc; ++i) {
        PackedVertex& v = vertices[i];
        v.pos[0] = mesh.positions[3 * i + 0];
        v.pos[1] = mesh.positions[3 * i + 1];
        v.pos[2] = mesh.positions[3 * i + 2];

        if (mesh.hasNormals()) {
            v.normal[0] = mesh.normals[3 * i + 0];
            v.normal[1] = mesh.normals[3 * i + 1];
            v.normal[2] = mesh.normals[3 * i + 2];
        } else {
            v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
        }

        if (mesh.hasUVs()) {
            v.uv[0] = mesh.uvs[2 * i + 0];
            v.uv[1] = mesh.uvs[2 * i + 1];
        } else {
            v.uv[0] = 0.0f; v.uv[1] = 0.0f;
        }

        if (mesh.hasColors()) {
            v.color[0] = mesh.colors[4 * i + 0];
            v.color[1] = mesh.colors[4 * i + 1];
            v.color[2] = mesh.colors[4 * i + 2];
            v.color[3] = mesh.colors[4 * i + 3];
        } else {
            v.color[0] = 1.0f; v.color[1] = 1.0f; v.color[2] = 1.0f; v.color[3] = 1.0f;
        }

        if (mesh.hasTangents()) {
            v.tangent[0] = mesh.tangents[4 * i + 0];
            v.tangent[1] = mesh.tangents[4 * i + 1];
            v.tangent[2] = mesh.tangents[4 * i + 2];
            v.tangent[3] = mesh.tangents[4 * i + 3];
        } else {
            v.tangent[0] = 1.0f; v.tangent[1] = 0.0f; v.tangent[2] = 0.0f; v.tangent[3] = 1.0f;
        }
    }

    allocator_.createVertexBuffer(vc * sizeof(PackedVertex), vertices.data(), entry.vertexBuffer);

    if (!mesh.indices.empty()) {
        allocator_.createIndexBuffer(mesh.indices.size() * sizeof(uint32_t), mesh.indices.data(), entry.indexBuffer);
    } else {
        std::vector<uint32_t> seq(vc);
        for (uint32_t i = 0; i < vc; ++i) seq[i] = i;
        allocator_.createIndexBuffer(seq.size() * sizeof(uint32_t), seq.data(), entry.indexBuffer);
    }

    entry.vertexCount = vc;
    entry.indexCount = static_cast<uint32_t>(ic);
    return entry;
}

SceneVkBridge::NodeDynamicBuffers& SceneVkBridge::getDynamicBuffers(const void* key) {
    return dynamicBufferCache_[key];
}

VkDescriptorSet SceneVkBridge::uploadTexture(const void* key, int width, int height, const uint8_t* rgba) {
    auto& tex = textureCache_[key];
    if (tex.image.isValid() && tex.width == width && tex.height == height) {
        return tex.descSet;
    }

    if (tex.image.isValid()) {
        allocator_.destroyImage(tex.image);
    }

    TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.generateMipmaps = true;
    if (!allocator_.createTexture2D(rgba, desc, tex.image)) {
        return VK_NULL_HANDLE;
    }
    tex.width = width;
    tex.height = height;

    tex.descSet = dynamicDescPool_.allocate(passMesh_.materialLayout());
    if (!tex.descSet) return VK_NULL_HANDLE;

    SceneVkDescriptorWriter writer;
    writer.writeImage(0, tex.image.view, tex.image.sampler);
    writer.writeImage(1, passMesh_.dummyNormalView(), passMesh_.defaultSampler());
    writer.writeImage(2, passMesh_.dummyWhiteView(), passMesh_.defaultSampler());
    writer.writeImage(3, passMesh_.dummyBlackView(), passMesh_.defaultSampler());
    writer.updateSet(device_.device(), tex.descSet);

    return tex.descSet;
}

void SceneVkBridge::render3D(SceneGraph& graph, SceneRenderer& renderer) {
    uint32_t width = static_cast<uint32_t>(graph.canvasWidth());
    uint32_t height = static_cast<uint32_t>(graph.canvasHeight());
    if (width == 0 || height == 0) return;

    if (!ensureTargets(width, height)) return;

    renderer.setCullingActive(renderer.frustumCullingEnabled());

    // 1. Camera UBO setup
    SceneCameraUniforms camUniforms{};
    std::memcpy(camUniforms.view, graph.viewMatrix().data, sizeof(camUniforms.view));

    bromath::Mat4 proj = graph.projectionMatrix();
    proj.at(1, 0) *= -1.0f;
    proj.at(1, 1) *= -1.0f;
    proj.at(1, 2) *= -1.0f;
    proj.at(1, 3) *= -1.0f;
    std::memcpy(camUniforms.proj, proj.data, sizeof(camUniforms.proj));

    bromath::Mat4 vp = bromath::mmul(proj, graph.viewMatrix());
    std::memcpy(camUniforms.viewProj, vp.data, sizeof(camUniforms.viewProj));

    bromath::Mat4 invView = bromath::minverse(graph.viewMatrix());
    std::memcpy(camUniforms.invView, invView.data, sizeof(camUniforms.invView));

    bromath::Mat4 invProj = bromath::minverse(proj);
    std::memcpy(camUniforms.invProj, invProj.data, sizeof(camUniforms.invProj));

    camUniforms.eyePos[0] = graph.cameraEye().x;
    camUniforms.eyePos[1] = graph.cameraEye().y;
    camUniforms.eyePos[2] = graph.cameraEye().z;
    camUniforms.eyePos[3] = 0.0f;

    camUniforms.viewport[0] = static_cast<float>(width);
    camUniforms.viewport[1] = static_cast<float>(height);
    camUniforms.viewport[2] = graph.cameraNearZ_;
    camUniforms.viewport[3] = graph.cameraFarZ_;

    camUniforms.fogParams[0] = renderer.fogStart();
    camUniforms.fogParams[1] = renderer.fogEnd();
    camUniforms.fogParams[2] = renderer.fogDensity();
    camUniforms.fogParams[3] = renderer.fogStartDist();

    camUniforms.fogColor[0] = renderer.fogColor()[0];
    camUniforms.fogColor[1] = renderer.fogColor()[1];
    camUniforms.fogColor[2] = renderer.fogColor()[2];
    camUniforms.fogColor[3] = renderer.fogHeightFalloff();

    allocator_.updateUniformBuffer(cameraUbo_, &camUniforms, sizeof(camUniforms));

    // 2. Lighting UBO setup
    SceneLightingUniforms lightUniforms{};
    LightNode* sunLight = nullptr;
    std::vector<LightNode*> pointLights;
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::Light) continue;
        auto* l = static_cast<LightNode*>(node.get());
        if (l->kind() == LightNode::Kind::Directional) {
            if (!sunLight || l->castsShadow()) {
                sunLight = l;
            }
        } else if (l->kind() == LightNode::Kind::Point) {
            if (pointLights.size() < 16) {
                pointLights.push_back(l);
            }
        }
    }

    if (sunLight) {
        bromath::Vec3 dir = bromath::vnorm(sunLight->direction());
        lightUniforms.sunDirection[0] = dir.x;
        lightUniforms.sunDirection[1] = dir.y;
        lightUniforms.sunDirection[2] = dir.z;
        lightUniforms.sunDirection[3] = 1.0f;

        const auto& col = sunLight->color();
        lightUniforms.sunColor[0] = col.x;
        lightUniforms.sunColor[1] = col.y;
        lightUniforms.sunColor[2] = col.z;
        lightUniforms.sunColor[3] = sunLight->intensity();
        lightUniforms.numLights[0] = 1.0f;
        if (sunLight->castsShadow()) {
            lightUniforms.numLights[2] = 1.0f;
        }
    } else if (pointLights.empty()) {
        lightUniforms.sunDirection[0] = -0.3f;
        lightUniforms.sunDirection[1] = -1.0f;
        lightUniforms.sunDirection[2] = -0.5f;
        float dLen = std::sqrt(0.09f + 1.0f + 0.25f);
        lightUniforms.sunDirection[0] /= dLen;
        lightUniforms.sunDirection[1] /= dLen;
        lightUniforms.sunDirection[2] /= dLen;
        lightUniforms.sunDirection[3] = 1.0f;

        lightUniforms.sunColor[0] = 1.0f;
        lightUniforms.sunColor[1] = 0.98f;
        lightUniforms.sunColor[2] = 0.95f;
        lightUniforms.sunColor[3] = 3.0f;
        lightUniforms.numLights[0] = 1.0f;
    } else {
        lightUniforms.numLights[0] = 0.0f;
    }

    lightUniforms.numLights[1] = static_cast<float>(pointLights.size());
    for (size_t i = 0; i < pointLights.size(); ++i) {
        LightNode* pl = pointLights[i];
        const auto& M = pl->worldMatrix();
        lightUniforms.pointLights[i].position[0] = M.at(0, 3);
        lightUniforms.pointLights[i].position[1] = M.at(1, 3);
        lightUniforms.pointLights[i].position[2] = M.at(2, 3);
        lightUniforms.pointLights[i].position[3] = pl->range();

        const auto& c = pl->color();
        lightUniforms.pointLights[i].color[0] = c.x;
        lightUniforms.pointLights[i].color[1] = c.y;
        lightUniforms.pointLights[i].color[2] = c.z;
        lightUniforms.pointLights[i].color[3] = pl->intensity();
    }

    const float* amb = renderer.effectiveAmbient();
    lightUniforms.ambientColor[0] = amb[0];
    lightUniforms.ambientColor[1] = amb[1];
    lightUniforms.ambientColor[2] = amb[2];
    lightUniforms.ambientColor[3] = 1.0f;

    bromath::Vec3 lightDir = {lightUniforms.sunDirection[0], lightUniforms.sunDirection[1], lightUniforms.sunDirection[2]};
    bromath::Vec3 fwd = {-graph.viewMatrix().at(2, 0),
                         -graph.viewMatrix().at(2, 1),
                         -graph.viewMatrix().at(2, 2)};
    bromath::Vec3 sceneCenter = graph.cameraEye() + fwd * 8.0f;
    bromath::Vec3 lightEye = sceneCenter - lightDir * 25.0f;
    bromath::Vec3 lightTarget = sceneCenter;
    bromath::Vec3 lightUp = (std::abs(lightDir.y) > 0.99f) ? bromath::Vec3{0, 0, 1} : bromath::Vec3{0, 1, 0};
    bromath::Mat4 lightView = bromath::mlookAt(lightEye, lightTarget, lightUp);

    float orthoSize = 12.0f;
    float znear = 1.0f;
    float zfar = 50.0f;
    bromath::Mat4 lightProj = bromath::midentity();
    for (int i = 0; i < 16; ++i) lightProj.data[i] = 0.0f;
    lightProj.at(0, 0) = 1.0f / orthoSize;
    lightProj.at(1, 1) = -1.0f / orthoSize; // Vulkan Y-flip
    lightProj.at(2, 2) = -1.0f / (zfar - znear);
    lightProj.at(2, 3) = -znear / (zfar - znear);
    lightProj.at(3, 3) = 1.0f;

    bromath::Mat4 lightVP = bromath::mmul(lightProj, lightView);
    std::memcpy(lightUniforms.shadowCascadeProj, lightVP.data, sizeof(lightUniforms.shadowCascadeProj));

    allocator_.updateUniformBuffer(lightingUbo_, &lightUniforms, sizeof(lightUniforms));

    // 3. Command Recording
    VkCommandBuffer cmd = device_.beginFrame();

    CullStats stats{};
    bool hasDrawnMeshes = false;

    // Prepare dynamic buffers before any pass (used by both shadow & mesh passes)
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible()) continue;
        if (node->type() == SceneNode::Type::Mesh) {
            auto* mn = static_cast<MeshNode*>(node.get());
            if (mn->asSkinnedMesh()) {
                auto* sm = mn->asSkinnedMesh();
                if (!sm->currentMesh().empty()) {
                    uploadMesh(sm->currentMesh(), sm);
                    auto& dynBuf = getDynamicBuffers(sm);
                    size_t skinVc = sm->currentMesh().vertexCount();
                    size_t skinByteSize = skinVc * 24;
                    if (dynBuf.skinAttribCapacity < skinByteSize) {
                        allocator_.destroyBuffer(dynBuf.skinAttribBuffer);
                        std::vector<uint8_t> skinBytes(skinByteSize, 0);
                        const auto& j = sm->skinJoints();
                        const auto& w = sm->skinWeights();
                        for (size_t vi = 0; vi < skinVc; ++vi) {
                            uint16_t* dstJ = reinterpret_cast<uint16_t*>(skinBytes.data() + vi * 24);
                            float* dstW = reinterpret_cast<float*>(skinBytes.data() + vi * 24 + 8);
                            if (vi * 4 + 3 < j.size()) {
                                dstJ[0] = j[vi * 4 + 0]; dstJ[1] = j[vi * 4 + 1];
                                dstJ[2] = j[vi * 4 + 2]; dstJ[3] = j[vi * 4 + 3];
                            }
                            if (vi * 4 + 3 < w.size()) {
                                dstW[0] = w[vi * 4 + 0]; dstW[1] = w[vi * 4 + 1];
                                dstW[2] = w[vi * 4 + 2]; dstW[3] = w[vi * 4 + 3];
                            } else {
                                dstW[0] = 1.0f;
                            }
                        }
                        allocator_.createVertexBuffer(skinByteSize, skinBytes.data(), dynBuf.skinAttribBuffer);
                        dynBuf.skinAttribCapacity = skinByteSize;
                    }

                    if (!dynBuf.boneUbo.isValid()) {
                        allocator_.createUniformBuffer(256 * 16 * sizeof(float), dynBuf.boneUbo);
                        dynBuf.boneSet = dynamicDescPool_.allocate(passMesh_.bonePaletteLayout());
                        SceneVkDescriptorWriter writer;
                        writer.writeBuffer(0, dynBuf.boneUbo.buffer, 256 * 16 * sizeof(float));
                        writer.updateSet(device_.device(), dynBuf.boneSet);

                        dynBuf.boneSetShadow = dynamicDescPool_.allocate(passShadow_.bonePaletteLayout());
                        SceneVkDescriptorWriter writerShadow;
                        writerShadow.writeBuffer(0, dynBuf.boneUbo.buffer, 256 * 16 * sizeof(float));
                        writerShadow.updateSet(device_.device(), dynBuf.boneSetShadow);
                    }
                    std::vector<float> boneData(256 * 16, 0.0f);
                    for (int b = 0; b < 256; ++b) {
                        boneData[b * 16 + 0] = 1.0f;
                        boneData[b * 16 + 5] = 1.0f;
                        boneData[b * 16 + 10] = 1.0f;
                        boneData[b * 16 + 15] = 1.0f;
                    }
                    const auto& pal = sm->skinPalette();
                    if (!pal.empty()) {
                        size_t copyCount = std::min(pal.size(), boneData.size());
                        std::memcpy(boneData.data(), pal.data(), copyCount * sizeof(float));
                    }
                    allocator_.updateUniformBuffer(dynBuf.boneUbo, boneData.data(), boneData.size() * sizeof(float));
                }
            } else {
                if (!mn->currentMesh().empty()) {
                    uploadMesh(mn->currentMesh(), mn);
                }
            }
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<InstancedMeshNode*>(node.get());
            if (!im->mesh().empty() && im->instanceCount() > 0) {
                uploadMesh(im->mesh(), im);
                auto& dynBuf = getDynamicBuffers(im);
                size_t instByteSize = im->instanceCount() * 16 * sizeof(float);
                if (dynBuf.instanceCapacity < instByteSize) {
                    allocator_.destroyBuffer(dynBuf.instanceBuffer);
                    allocator_.createVertexBuffer(instByteSize, im->instanceData().data(), dynBuf.instanceBuffer);
                    dynBuf.instanceCapacity = instByteSize;
                } else {
                    allocator_.stageAndUploadBuffer(dynBuf.instanceBuffer.buffer, im->instanceData().data(), instByteSize);
                }
            }
        }
    }

    // Shadow pass
    if (lightUniforms.numLights[2] > 0.5f) {
        passShadow_.beginCascade(cmd, device_, shadowTarget_, 0, lightUniforms.shadowCascadeProj);
        for (auto& [id, node] : graph.nodes_) {
            if (!node->renderVisible()) continue;
            if (node->type() == SceneNode::Type::Mesh) {
                auto* mn = static_cast<MeshNode*>(node.get());
                if (!mn->castsShadow() || mn->currentMesh().empty()) continue;
                auto& meshBuf = uploadMesh(mn->currentMesh(), mn);
                if (mn->asSkinnedMesh()) {
                    auto* sm = mn->asSkinnedMesh();
                    auto& dynBuf = getDynamicBuffers(sm);
                    SkinnedShadowCaster caster{};
                    caster.vertexBuffer = meshBuf.vertexBuffer.buffer;
                    caster.indexBuffer = meshBuf.indexBuffer.buffer;
                    caster.indexCount = meshBuf.indexCount;
                    caster.skinAttribBuffer = dynBuf.skinAttribBuffer.buffer;
                    caster.bonePaletteSet = dynBuf.boneSetShadow;
                    std::memcpy(caster.modelMatrix, sm->worldMatrix().data, sizeof(caster.modelMatrix));
                    passShadow_.drawSkinned(cmd, caster);
                    stats.shadowDrawn++;
                } else {
                    ShadowCaster caster{};
                    caster.vertexBuffer = meshBuf.vertexBuffer.buffer;
                    caster.indexBuffer = meshBuf.indexBuffer.buffer;
                    caster.indexCount = meshBuf.indexCount;
                    std::memcpy(caster.modelMatrix, mn->worldMatrix().data, sizeof(caster.modelMatrix));
                    passShadow_.drawStatic(cmd, caster);
                    stats.shadowDrawn++;
                }
            } else if (node->type() == SceneNode::Type::InstancedMesh) {
                auto* im = static_cast<InstancedMeshNode*>(node.get());
                if (!im->castsShadow() || im->mesh().empty() || im->instanceCount() == 0) continue;
                auto& meshBuf = uploadMesh(im->mesh(), im);
                auto& dynBuf = getDynamicBuffers(im);
                InstancedShadowCaster caster{};
                caster.vertexBuffer = meshBuf.vertexBuffer.buffer;
                caster.indexBuffer = meshBuf.indexBuffer.buffer;
                caster.indexCount = meshBuf.indexCount;
                caster.instanceBuffer = dynBuf.instanceBuffer.buffer;
                caster.instanceCount = static_cast<uint32_t>(im->instanceCount());
                std::memcpy(caster.modelMatrix, im->worldMatrix().data, sizeof(caster.modelMatrix));
                passShadow_.drawInstanced(cmd, caster);
                stats.shadowDrawn++;
            }
        }
        passShadow_.endCascade(cmd, device_, shadowTarget_);
        shadowTarget_.transitionToShaderRead(cmd, allocator_);
    }

    // HDR mesh pass
    hdrTarget_.beginRendering(cmd, device_,
                             VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                             {{0.0f, 0.0f, 0.0f, 0.0f}},
                             VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                             0.0f);

    if (renderer.atmosphere().enabled || !renderer.envPath().empty()) {
        EnvironmentParams envParams{};
        bromath::Mat4 invVP = bromath::minverse(vp);
        std::memcpy(envParams.invViewProj, invVP.data, sizeof(envParams.invViewProj));
        envParams.sunDirection[0] = lightUniforms.sunDirection[0];
        envParams.sunDirection[1] = lightUniforms.sunDirection[1];
        envParams.sunDirection[2] = lightUniforms.sunDirection[2];
        envParams.sunIntensity = lightUniforms.sunColor[3];
        envParams.skyIntensity = renderer.environmentIntensity();
        passEnv_.render(cmd, width, height, envParams);
    }

    passMesh_.begin(cmd, cameraSet_, lightingSet_, width, height);

    struct TranslucentDraw {
        enum class Type { Static, Instanced, Skinned } type;
        MeshDrawCall drawStatic;
        InstancedMeshDrawCall drawInstanced;
        SkinnedMeshDrawCall drawSkinned;
        float depth = 0.0f;
    };
    std::vector<TranslucentDraw> translucentDraws;
    const bromath::Vec3 viewFwd{-graph.viewMatrix().at(2, 0),
                                -graph.viewMatrix().at(2, 1),
                                -graph.viewMatrix().at(2, 2)};
    auto getDepth = [&](SceneNode* n) {
        bromath::Vec3 c;
        if (auto wbOpt = renderer.nodeWorldBounds(n)) {
            c = (wbOpt->min + wbOpt->max) * 0.5f;
        } else {
            const bromath::Mat4& w = n->worldMatrix();
            c = bromath::Vec3{w.at(0, 3), w.at(1, 3), w.at(2, 3)};
        }
        return bromath::vdot(c - graph.cameraEye(), viewFwd);
    };

    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible()) continue;

        if (node->type() == SceneNode::Type::Mesh) {
            auto* mn = static_cast<MeshNode*>(node.get());
            if (mn->asSkinnedMesh()) {
                auto* sm = mn->asSkinnedMesh();
                if (renderer.cameraCulled(sm)) {
                    stats.meshCulled++;
                    continue;
                }
                stats.meshDrawn++;
                hasDrawnMeshes = true;
                if (!sm->currentMesh().empty()) {
                    auto& meshBuf = uploadMesh(sm->currentMesh(), sm);
                    auto& dynBuf = getDynamicBuffers(sm);

                    SkinnedMeshDrawCall draw{};
                    draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
                    draw.indexBuffer = meshBuf.indexBuffer.buffer;
                    draw.indexCount = meshBuf.indexCount;
                    draw.skinAttribBuffer = dynBuf.skinAttribBuffer.buffer;
                    draw.bonePaletteSet = dynBuf.boneSet;
                    std::memcpy(draw.modelMatrix, sm->worldMatrix().data, sizeof(draw.modelMatrix));
                    std::memcpy(draw.baseColor, sm->color(), sizeof(draw.baseColor));
                    std::memcpy(draw.emissiveColor, sm->emissiveColor(), sizeof(draw.emissiveColor));
                    draw.emissiveIntensity = sm->emissive();
                    draw.metallic = sm->metallic();
                    draw.roughness = sm->roughness();
                    draw.alphaCutoff = sm->alphaCutoff();
                    draw.flags = 0;
                    if (sm->effectiveUnlit()) draw.flags |= 16u;

                    if (sm->color()[3] < 1.0f) {
                        translucentDraws.push_back({TranslucentDraw::Type::Skinned, {}, {}, draw, getDepth(sm)});
                    } else {
                        passMesh_.drawSkinned(cmd, draw);
                    }
                }
            } else {
                if (renderer.cameraCulled(mn)) {
                    stats.meshCulled++;
                    continue;
                }
                stats.meshDrawn++;
                hasDrawnMeshes = true;
                if (!mn->currentMesh().empty()) {
                    auto& meshBuf = uploadMesh(mn->currentMesh(), mn);
                    MeshDrawCall draw{};
                    draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
                    draw.indexBuffer = meshBuf.indexBuffer.buffer;
                    draw.indexCount = meshBuf.indexCount;
                    std::memcpy(draw.modelMatrix, mn->worldMatrix().data, sizeof(draw.modelMatrix));
                    std::memcpy(draw.baseColor, mn->color(), sizeof(draw.baseColor));
                    std::memcpy(draw.emissiveColor, mn->emissiveColor(), sizeof(draw.emissiveColor));
                    draw.emissiveIntensity = mn->emissive();
                    draw.metallic = mn->metallic();
                    draw.roughness = mn->roughness();
                    draw.alphaCutoff = mn->alphaCutoff();
                    draw.flags = 0;
                    if (mn->effectiveUnlit()) draw.flags |= 16u;

                    const auto& pTex = mn->pendingBaseTexture();
                    if (pTex.w > 0 && pTex.h > 0 && !pTex.data.empty()) {
                        draw.materialSet = uploadTexture(mn, pTex.w, pTex.h, pTex.data.data());
                        if (draw.materialSet != VK_NULL_HANDLE) draw.flags |= 1u;
                    }
                    if (mn->color()[3] < 1.0f) {
                        translucentDraws.push_back({TranslucentDraw::Type::Static, draw, {}, {}, getDepth(mn)});
                    } else {
                        passMesh_.drawStatic(cmd, draw);
                    }
                }
            }
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<InstancedMeshNode*>(node.get());
            if (renderer.cameraCulled(im)) {
                stats.instancedCulled++;
                continue;
            }
            stats.instancedDrawn++;
            hasDrawnMeshes = true;
            if (!im->mesh().empty() && im->instanceCount() > 0) {
                auto& meshBuf = uploadMesh(im->mesh(), im);
                auto& dynBuf = getDynamicBuffers(im);

                InstancedMeshDrawCall draw{};
                draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
                draw.indexBuffer = meshBuf.indexBuffer.buffer;
                draw.indexCount = meshBuf.indexCount;
                draw.instanceBuffer = dynBuf.instanceBuffer.buffer;
                draw.instanceCount = static_cast<uint32_t>(im->instanceCount());
                std::memcpy(draw.modelMatrix, im->worldMatrix().data, sizeof(draw.modelMatrix));
                std::memcpy(draw.baseColor, im->color(), sizeof(draw.baseColor));
                std::memcpy(draw.emissiveColor, im->emissiveColor(), sizeof(draw.emissiveColor));
                draw.emissiveIntensity = im->emissive();
                draw.metallic = im->metallic();
                draw.roughness = im->roughness();
                draw.alphaCutoff = im->alphaCutoff();
                draw.flags = 0;
                if (im->effectiveUnlit()) draw.flags |= 16u;

                if (im->color()[3] < 1.0f) {
                    translucentDraws.push_back({TranslucentDraw::Type::Instanced, {}, draw, {}, getDepth(im)});
                } else {
                    passMesh_.drawInstanced(cmd, draw);
                }
            }
        }
    }

    if (!translucentDraws.empty()) {
        std::stable_sort(translucentDraws.begin(), translucentDraws.end(),
                         [](const TranslucentDraw& a, const TranslucentDraw& b) {
                             return a.depth > b.depth;
                         });
        for (const auto& td : translucentDraws) {
            if (td.type == TranslucentDraw::Type::Static) {
                passMesh_.drawStaticTranslucent(cmd, td.drawStatic);
            } else if (td.type == TranslucentDraw::Type::Instanced) {
                passMesh_.drawInstancedTranslucent(cmd, td.drawInstanced);
            } else if (td.type == TranslucentDraw::Type::Skinned) {
                passMesh_.drawSkinnedTranslucent(cmd, td.drawSkinned);
            }
        }
    }

    if (graph.gizmoProvider_) {
        auto gizmoMeshes = graph.gizmoProvider_(&graph);
        for (auto* gm : gizmoMeshes) {
            if (!gm || gm->mesh().empty()) continue;
            auto& meshBuf = uploadMesh(gm->mesh(), gm);
            MeshDrawCall draw{};
            draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
            draw.indexBuffer = meshBuf.indexBuffer.buffer;
            draw.indexCount = meshBuf.indexCount;
            std::memcpy(draw.modelMatrix, gm->worldMatrix().data, sizeof(draw.modelMatrix));
            std::memcpy(draw.baseColor, gm->color(), sizeof(draw.baseColor));
            std::memcpy(draw.emissiveColor, gm->emissiveColor(), sizeof(draw.emissiveColor));
            draw.emissiveIntensity = gm->emissive();
            draw.metallic = 0.0f;
            draw.roughness = 1.0f;
            draw.flags = 16u;
            passMesh_.drawStatic(cmd, draw);
            hasDrawnMeshes = true;
        }
    }

    hdrTarget_.endRendering(cmd, device_);
    hdrTarget_.transitionColorToShaderRead(cmd, allocator_);

    // PostFx Pass
    allocator_.transitionImageLayout(cmd, ldrPresentationImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                   ldrPresentationImage_.currentLayout,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ldrPresentationImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    PostFxParams postFx{};
    postFx.exposure = renderer.exposure();
    postFx.gamma = renderer.gamma();
    postFx.tonemapMode = static_cast<TonemapMode>(renderer.toneMap());
    postFx.enableBloom = renderer.bloomEnabled();
    postFx.bloomIntensity = renderer.bloomIntensity();
    postFx.enableFxaa = renderer.fxaaEnabled();

    passPostFx_.render(cmd, device_, allocator_,
                      hdrTarget_.colorImage(),
                      ldrPresentationImage_.view,
                      VK_FORMAT_R8G8B8A8_UNORM,
                      width, height, postFx);

    allocator_.transitionImageLayout(cmd, ldrPresentationImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                   ldrPresentationImage_.currentLayout,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ldrPresentationImage_.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = width;
    copyRegion.bufferImageHeight = height;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {width, height, 1};

    vkCmdCopyImageToBuffer(cmd, ldrPresentationImage_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer_.buffer, 1, &copyRegion);

    device_.endFrame();
    device_.submitFrame();
    device_.waitIdle();

    hasMeshContent_ = hasDrawnMeshes;
    renderer.setCullStats(stats);
}

std::vector<uint8_t> SceneVkBridge::readTonemapPixelsRGBA(int& outW, int& outH) {
    if (!readbackBuffer_.isValid() || currentWidth_ == 0 || currentHeight_ == 0) {
        outW = outH = 0;
        return {};
    }

    outW = static_cast<int>(currentWidth_);
    outH = static_cast<int>(currentHeight_);
    size_t size = static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4;

    std::vector<uint8_t> result(size);
    void* mapped = nullptr;
    if (vkMapMemory(device_.device(), readbackBuffer_.memory, 0, size, 0, &mapped) == VK_SUCCESS) {
        std::memcpy(result.data(), mapped, size);
        vkUnmapMemory(device_.device(), readbackBuffer_.memory);
    } else {
        outW = outH = 0;
        return {};
    }

    return result;
}

} // namespace bro::scene::vk
