#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/gpu_upload_stats.h"
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
    noteMeshUpload(vc * sizeof(PackedVertex) + ic * sizeof(uint32_t));
    return entry;
}

SceneVkBridge::NodeDynamicBuffers& SceneVkBridge::getDynamicBuffers(const void* key) {
    return dynamicBufferCache_[key];
}

VkDescriptorSet SceneVkBridge::uploadTexture(const void* key, int width, int height, const uint8_t* rgba) {
    auto& tex = textureCache_[key];
    uint64_t hash = 0;
    if (rgba && width > 0 && height > 0) {
        hash = 14695981039346656037ULL;
        hash ^= (static_cast<uint64_t>(width) << 32) | static_cast<uint32_t>(height);
        hash *= 1099511628211ULL;
        size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        size_t samples = std::min<size_t>(byteCount, 256);
        for (size_t i = 0; i < samples; ++i) {
            hash ^= rgba[i];
            hash *= 1099511628211ULL;
        }
    }

    if (tex.image.isValid() && tex.width == width && tex.height == height && tex.hash == hash) {
        return tex.descSet;
    }

    if (tex.image.isValid() && tex.owned) {
        allocator_.destroyImage(tex.image);
    }
    tex.image = {};
    tex.owned = true;

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
    tex.hash = hash;
    noteTextureUpload(static_cast<size_t>(width) * height * 4);

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

VkDescriptorSet SceneVkBridge::uploadExternalSceneTexture(SceneVkBridge* srcBridge, const void* key) {
    if (!srcBridge || srcBridge->ldrPresentationImage_.view == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    auto& tex = textureCache_[key];
    if (tex.descSet != VK_NULL_HANDLE &&
        !tex.owned &&
        tex.image.image == srcBridge->ldrPresentationImage_.image &&
        tex.width == static_cast<int>(srcBridge->currentWidth_) &&
        tex.height == static_cast<int>(srcBridge->currentHeight_)) {
        return tex.descSet;
    }

    if (tex.image.isValid() && tex.owned) {
        allocator_.destroyImage(tex.image);
        tex.image = {};
    }
    tex.owned = false;
    tex.image.image = srcBridge->ldrPresentationImage_.image;
    tex.image.view = srcBridge->ldrPresentationImage_.view;
    tex.image.sampler = srcBridge->ldrPresentationImage_.sampler;
    tex.width = static_cast<int>(srcBridge->currentWidth_);
    tex.height = static_cast<int>(srcBridge->currentHeight_);

    if (tex.descSet == VK_NULL_HANDLE) {
        tex.descSet = dynamicDescPool_.allocate(passMesh_.materialLayout());
    }
    if (!tex.descSet) return VK_NULL_HANDLE;

    SceneVkDescriptorWriter writer;
    writer.writeImage(0, srcBridge->ldrPresentationImage_.view, srcBridge->ldrPresentationImage_.sampler);
    writer.writeImage(1, passMesh_.dummyNormalView(), passMesh_.defaultSampler());
    writer.writeImage(2, passMesh_.dummyWhiteView(), passMesh_.defaultSampler());
    writer.writeImage(3, passMesh_.dummyBlackView(), passMesh_.defaultSampler());
    writer.updateSet(device_.device(), tex.descSet);

    return tex.descSet;
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
    if (readbackBuffer_.mappedData) {
        std::memcpy(result.data(), readbackBuffer_.mappedData, size);
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(device_.device(), readbackBuffer_.memory, readbackBuffer_.offset, size, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(result.data(), mapped, size);
            vkUnmapMemory(device_.device(), readbackBuffer_.memory);
        } else {
            outW = outH = 0;
            return {};
        }
    }

    return result;
}

bool SceneVkBridge::ensureTargets(uint32_t width, uint32_t height, VkSampleCountFlagBits sampleCount) {
    if (width == 0 || height == 0) return false;

    if (!shadowTarget_.isValid()) {
        if (!shadowTarget_.init(allocator_, 1024, 4, VK_FORMAT_D32_SFLOAT)) {
            LOG_ERROR("SceneVkBridge: Failed initializing shadow target");
            return false;
        }
        SceneVkDescriptorWriter lightWriter;
        lightWriter.writeBuffer(0, lightingUbo_.buffer, sizeof(SceneLightingUniforms));
        lightWriter.writeImage(1, shadowTarget_.arrayView(), shadowTarget_.shadowSampler());
        lightWriter.writeImage(2, passReflectionProbe_.dummyCubemapView(), passReflectionProbe_.activeCubemapSampler());
        lightWriter.writeImage(3, dummyShadeMap_.view, dummyShadeMap_.sampler);
        lightWriter.updateSet(device_.device(), lightingSet_);
    }

    if (currentWidth_ == width && currentHeight_ == height && currentSampleCount_ == sampleCount && hdrTarget_.isValid()) {
        return true;
    }

    currentWidth_ = width;
    currentHeight_ = height;

    if (currentSampleCount_ != sampleCount) {
        currentSampleCount_ = sampleCount;
        passMesh_.cleanup(device_, allocator_);
        PassMesh::Config meshCfg{};
        meshCfg.samples = sampleCount;
        passMesh_.init(device_, allocator_, meshCfg);
        passTerrain_.setSampleCount(sampleCount);
    }

    hdrTarget_.cleanup(allocator_);
    SceneVkRenderTargetDesc hdrDesc{};
    hdrDesc.width = width;
    hdrDesc.height = height;
    hdrDesc.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    hdrDesc.depthFormat = VK_FORMAT_D32_SFLOAT;
    hdrDesc.sampleCount = sampleCount;
    hdrDesc.hasColor = true;
    hdrDesc.hasDepth = true;
    if (!hdrTarget_.init(allocator_, hdrDesc)) {
        LOG_ERROR("SceneVkBridge: Failed creating HDR render target (%ux%u)", width, height);
        return false;
    }

    allocator_.destroyImage(depthCopyImage_);
    VkImageUsageFlags depthCopyUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!allocator_.createImage(width, height, VK_FORMAT_D32_SFLOAT, depthCopyUsage,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthCopyImage_,
                               1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        LOG_ERROR("SceneVkBridge: Failed creating depth copy image (%ux%u)", width, height);
        return false;
    }

    VkSamplerCreateInfo sampInfo{};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_NEAREST;
    sampInfo.minFilter = VK_FILTER_NEAREST;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_.device(), &sampInfo, nullptr, &depthCopyImage_.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkBridge: Failed creating sampler for depth copy image");
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

    VkSamplerCreateInfo ldrSampInfo{};
    ldrSampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ldrSampInfo.magFilter = VK_FILTER_LINEAR;
    ldrSampInfo.minFilter = VK_FILTER_LINEAR;
    ldrSampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ldrSampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ldrSampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ldrSampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_.device(), &ldrSampInfo, nullptr, &ldrPresentationImage_.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkBridge: Failed creating sampler for LDR presentation image");
        return false;
    }

    allocator_.destroyImage(ssrColorSnapshot_);
    VkImageUsageFlags ssrUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!allocator_.createImage(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, ssrUsage,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ssrColorSnapshot_)) {
        LOG_ERROR("SceneVkBridge: Failed creating SSR color snapshot image (%ux%u)", width, height);
        return false;
    }
    if (vkCreateSampler(device_.device(), &ldrSampInfo, nullptr, &ssrColorSnapshot_.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkBridge: Failed creating sampler for SSR color snapshot image");
        return false;
    }

    allocator_.destroyImage(dofHdrImage_);
    VkImageUsageFlags dofUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!allocator_.createImage(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, dofUsage,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dofHdrImage_)) {
        LOG_ERROR("SceneVkBridge: Failed creating DoF HDR image (%ux%u)", width, height);
        return false;
    }
    if (vkCreateSampler(device_.device(), &ldrSampInfo, nullptr, &dofHdrImage_.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkBridge: Failed creating sampler for DoF HDR image");
        return false;
    }

    allocator_.destroyImage(postLdrImage_);
    VkImageUsageFlags postLdrUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!allocator_.createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM, postLdrUsage,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, postLdrImage_)) {
        LOG_ERROR("SceneVkBridge: Failed creating Post LDR image (%ux%u)", width, height);
        return false;
    }
    if (vkCreateSampler(device_.device(), &ldrSampInfo, nullptr, &postLdrImage_.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkBridge: Failed creating sampler for Post LDR image");
        return false;
    }

    passSSAO_.resize(device_, allocator_, width, height);
    passDoF_.resize(device_, allocator_, width, height);

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

} // namespace bro::scene::vk
