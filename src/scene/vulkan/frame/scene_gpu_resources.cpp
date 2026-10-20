#include "scene/vulkan/frame/scene_gpu_resources.h"

#include "scene/gpu_upload_stats.h"
#include "scene/nodes/skinned_mesh_node.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

struct alignas(16) PackedVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float color[4];
    float tangent[4];
};
static_assert(sizeof(PackedVertex) == SceneGpuResources::kVertexStride);

std::vector<PackedVertex> packVertices(const bromesh::MeshData& mesh) {
    const size_t vc = mesh.vertexCount();
    std::vector<PackedVertex> out(vc);
    for (size_t i = 0; i < vc; ++i) {
        PackedVertex& v = out[i];
        std::memcpy(v.pos, &mesh.positions[3 * i], sizeof(v.pos));
        if (mesh.hasNormals()) {
            std::memcpy(v.normal, &mesh.normals[3 * i], sizeof(v.normal));
        } else {
            v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
        }
        if (mesh.hasUVs()) {
            std::memcpy(v.uv, &mesh.uvs[2 * i], sizeof(v.uv));
        } else {
            v.uv[0] = v.uv[1] = 0.0f;
        }
        if (mesh.hasColors()) {
            std::memcpy(v.color, &mesh.colors[4 * i], sizeof(v.color));
        } else {
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
        }
        if (mesh.hasTangents()) {
            std::memcpy(v.tangent, &mesh.tangents[4 * i], sizeof(v.tangent));
        } else {
            v.tangent[0] = 1.0f; v.tangent[1] = 0.0f; v.tangent[2] = 0.0f; v.tangent[3] = 1.0f;
        }
    }
    return out;
}

uint32_t mipCount(int w, int h) {
    return static_cast<uint32_t>(std::floor(std::log2(std::max(w, h)))) + 1;
}

}  // namespace

template <typename Map, typename Destroy>
void SceneGpuResources::eraseNode(Map& map, uint32_t id, Destroy&& destroy) {
    auto it = map.lower_bound(key(id, 0));
    while (it != map.end() && (it->first >> 32) == id) {
        destroy(it->second);
        it = map.erase(it);
    }
}

const GpuMesh* SceneGpuResources::mesh(uint32_t id, uint32_t slot, uint64_t generation,
                                       const bromesh::MeshData& data) {
    if (data.empty()) return nullptr;
    GpuMesh& entry = meshes_[key(id, slot)];
    if (entry.generation == generation && entry.vertices.isValid()) return &entry;

    allocator_.destroyBuffer(entry.vertices);
    allocator_.destroyBuffer(entry.indices);
    allocator_.destroyBuffer(entry.vegetation);
    allocator_.destroyBuffer(entry.chunkIndices);
    entry = GpuMesh{};

    const size_t vc = data.vertexCount();
    const std::vector<PackedVertex> vertices = packVertices(data);
    std::vector<uint32_t> sequential;
    const std::vector<uint32_t>* indices = &data.indices;
    if (data.indices.empty()) {
        sequential.resize(vc);
        for (uint32_t i = 0; i < vc; ++i) sequential[i] = i;
        indices = &sequential;
    }
    const size_t vegBytes = data.hasVegetation() ? data.vegetation.size() * sizeof(float) : 0;
    if (!allocator_.createVertexBuffer(vc * sizeof(PackedVertex), vertices.data(), entry.vertices) ||
        !allocator_.createIndexBuffer(indices->size() * sizeof(uint32_t), indices->data(), entry.indices) ||
        (vegBytes && !allocator_.createVertexBuffer(vegBytes, data.vegetation.data(), entry.vegetation))) {
        LOG_ERROR("SceneGpuResources: Failed uploading mesh of node %u (%zu vertices)", id, vc);
        allocator_.destroyBuffer(entry.vertices);
        allocator_.destroyBuffer(entry.indices);
        allocator_.destroyBuffer(entry.vegetation);
        meshes_.erase(key(id, slot));
        return nullptr;
    }
    entry.indexCount = static_cast<uint32_t>(indices->size());
    entry.generation = generation;
    noteMeshUpload(vc * sizeof(PackedVertex) + indices->size() * sizeof(uint32_t) + vegBytes);
    return &entry;
}

const GpuMesh* SceneGpuResources::chunked(uint32_t id, uint32_t slot, uint64_t generation,
                                          const bromesh::MeshData& data) {
    const GpuMesh* m = mesh(id, slot, generation, data);
    if (!m || m->chunksTried) return m;
    GpuMesh& entry = meshes_[key(id, slot)];
    entry.chunksTried = true;
    const size_t vc = data.vertexCount();
    const size_t ic = data.indices.empty() ? vc : data.indices.size();
    const size_t tris = ic / 3;
    if (tris < kChunkMinTriangles || ic % 3 != 0) return &entry;
    const auto index = [&](size_t i) { return data.indices.empty() ? static_cast<uint32_t>(i) : data.indices[i]; };
    const auto vertex = [&](uint32_t v) {
        return bromath::Vec3{data.positions[v * 3], data.positions[v * 3 + 1], data.positions[v * 3 + 2]};
    };

    // A grid over the mesh's two longest extents (a ground's or a wall's
    // plane), as many cells as chunks of about kChunkTriangles, each
    // triangle in the cell of its centroid; the cells in turn, a row's
    // back and forth, so neighbouring cells stay neighbouring runs.
    bromath::AABB3 box = bromath::aempty3();
    for (size_t v = 0; v < vc; ++v) box = bromath::aexpand(box, vertex(static_cast<uint32_t>(v)));
    const bromath::Vec3 ext = bromath::aextent(box);
    const float e[3] = {ext.x, ext.y, ext.z};
    int ax[3] = {0, 1, 2};
    std::sort(ax, ax + 3, [&](int a, int b) { return e[a] > e[b]; });
    const float ea = std::max(e[ax[0]], 1e-6f), eb = std::max(e[ax[1]], 1e-6f);
    const double cells = static_cast<double>(tris) / kChunkTriangles;
    const int na = std::clamp(static_cast<int>(std::lround(std::sqrt(cells * ea / eb))), 1, 256);
    const int nb = std::clamp(static_cast<int>(std::lround(cells / na)), 1, 256);
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    std::vector<uint32_t> cellOf(tris);
    std::vector<uint32_t> start(static_cast<size_t>(na) * nb + 1, 0);
    for (size_t t = 0; t < tris; ++t) {
        const bromath::Vec3 c = (vertex(index(3 * t)) + vertex(index(3 * t + 1)) + vertex(index(3 * t + 2))) * (1.0f / 3.0f);
        const float p[3] = {c.x, c.y, c.z};
        const int a = std::clamp(static_cast<int>((p[ax[0]] - lo[ax[0]]) / ea * na), 0, na - 1);
        int b = std::clamp(static_cast<int>((p[ax[1]] - lo[ax[1]]) / eb * nb), 0, nb - 1);
        if (a & 1) b = nb - 1 - b;
        cellOf[t] = static_cast<uint32_t>(a * nb + b);
        start[cellOf[t] + 1]++;
    }
    for (size_t c = 1; c < start.size(); ++c) start[c] += start[c - 1];
    std::vector<uint32_t> sorted(ic);
    std::vector<uint32_t> at(start.begin(), start.end() - 1);
    for (size_t t = 0; t < tris; ++t) {
        const uint32_t o = 3 * at[cellOf[t]]++;
        for (int k = 0; k < 3; ++k) sorted[o + k] = index(3 * t + k);
    }
    for (size_t c = 0; c + 1 < start.size(); ++c) {
        if (start[c + 1] == start[c]) continue;
        MeshChunk ch;
        ch.firstIndex = 3 * start[c];
        ch.indexCount = 3 * (start[c + 1] - start[c]);
        ch.bounds = bromath::aempty3();
        for (uint32_t i = ch.firstIndex; i < ch.firstIndex + ch.indexCount; ++i)
            ch.bounds = bromath::aexpand(ch.bounds, vertex(sorted[i]));
        entry.chunks.push_back(ch);
    }
    if (entry.chunks.size() < 2 ||
        !allocator_.createIndexBuffer(sorted.size() * sizeof(uint32_t), sorted.data(), entry.chunkIndices)) {
        entry.chunks.clear();
        return &entry;
    }
    noteMeshUpload(sorted.size() * sizeof(uint32_t));
    return &entry;
}

// How a slot's image is stored and sampled (see TextureSlot).
static TextureDesc textureDesc(TextureSlot slot) {
    TextureDesc desc{};
    switch (slot) {
    case TextureSlot::Sprite:
        desc.minFilter = desc.magFilter = VK_FILTER_NEAREST;
        [[fallthrough]];
    case TextureSlot::Html:
    case TextureSlot::Particle:
        desc.generateMipmaps = false;
        desc.enableAnisotropy = false;
        [[fallthrough]];
    case TextureSlot::DecalAlbedo:
    case TextureSlot::DecalEmission:
        desc.addressModeU = desc.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        break;
    default:
        break;
    }
    if (slot == TextureSlot::Particle) desc.format = VK_FORMAT_R8G8B8A8_SRGB;
    return desc;
}

const SceneVkImage* SceneGpuResources::texture(uint32_t id, TextureSlot slot, const NodeTexture& tex) {
    const uint64_t k = key(id, static_cast<uint32_t>(slot));
    if (tex.empty()) {
        auto it = textures_.find(k);
        if (it != textures_.end()) {
            allocator_.destroyImage(it->second.image);
            textures_.erase(it);
        }
        return nullptr;
    }
    Texture& entry = textures_[k];
    if (entry.generation == tex.generation && entry.image.isValid()) return &entry.image;

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(tex.width) * tex.height * 4;
    if (entry.image.isValid() && entry.image.width == static_cast<uint32_t>(tex.width) &&
        entry.image.height == static_cast<uint32_t>(tex.height)) {
        // Same shape: rewrite in place (the upload stream orders it after the
        // frames already submitted that sample the old pixels).
        allocator_.uploadImage(entry.image, tex.rgba.data(), bytes, {}, entry.image.mipLevels > 1);
    } else {
        allocator_.destroyImage(entry.image);
        TextureDesc desc = textureDesc(slot);
        desc.width = static_cast<uint32_t>(tex.width);
        desc.height = static_cast<uint32_t>(tex.height);
        if (!allocator_.createTexture2D(tex.rgba.data(), desc, entry.image)) {
            LOG_ERROR("SceneGpuResources: Failed uploading a %dx%d texture of node %u", tex.width, tex.height, id);
            textures_.erase(k);
            return nullptr;
        }
    }
    entry.generation = tex.generation;
    noteTextureUpload(static_cast<size_t>(bytes));
    return &entry.image;
}

VkBuffer SceneGpuResources::skinAttributes(const SkinnedMeshNode& node, size_t vertexCount) {
    Skin& entry = skins_[node.id()];
    if (entry.buffer.isValid() && entry.generation == node.skinGeneration() && entry.vertexCount == vertexCount)
        return entry.buffer.buffer;

    allocator_.destroyBuffer(entry.buffer);
    std::vector<uint8_t> bytes(vertexCount * kSkinStride, 0);
    const auto& joints = node.skinJoints();
    const auto& weights = node.skinWeights();
    for (size_t v = 0; v < vertexCount; ++v) {
        auto* dstJ = reinterpret_cast<uint16_t*>(bytes.data() + v * kSkinStride);
        auto* dstW = reinterpret_cast<float*>(bytes.data() + v * kSkinStride + 8);
        if (v * 4 + 3 < joints.size()) std::memcpy(dstJ, &joints[v * 4], 4 * sizeof(uint16_t));
        if (v * 4 + 3 < weights.size()) {
            std::memcpy(dstW, &weights[v * 4], 4 * sizeof(float));
        } else {
            dstW[0] = 1.0f;
        }
    }
    if (!allocator_.createVertexBuffer(bytes.size(), bytes.data(), entry.buffer)) {
        LOG_ERROR("SceneGpuResources: Failed uploading the skin of node %u", node.id());
        skins_.erase(node.id());
        return VK_NULL_HANDLE;
    }
    entry.generation = node.skinGeneration();
    entry.vertexCount = vertexCount;
    return entry.buffer.buffer;
}

const SceneVkBuffer* SceneGpuResources::storage(uint32_t id, uint64_t generation, const void* data, size_t bytes) {
    Storage& entry = storages_[id];
    if (entry.buffer.isValid() && entry.generation == generation) return &entry.buffer;
    allocator_.destroyBuffer(entry.buffer);
    const VkDeviceSize size = std::max<VkDeviceSize>(bytes, 16);
    // (Shared: the plants' plan and leaf poses read it on the async compute queue.)
    if (!allocator_.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, entry.buffer, /*shared=*/true) ||
        !allocator_.stageAndUploadBuffer(entry.buffer.buffer, data, bytes)) {
        LOG_ERROR("SceneGpuResources: Failed uploading the storage of node %u (%zu bytes)", id, bytes);
        allocator_.destroyBuffer(entry.buffer);
        storages_.erase(id);
        return nullptr;
    }
    entry.generation = generation;
    return &entry.buffer;
}

bool SceneGpuResources::createUserTexture(MeshNode::UserTexture& tex, UserTexture& out) {
    const uint32_t layers = tex.layers > 0 ? static_cast<uint32_t>(tex.layers) : 1;
    const VkFormat format = tex.channels == 1   ? VK_FORMAT_R32_SFLOAT
                            : tex.channels == 2 ? VK_FORMAT_R32G32_SFLOAT
                                                : VK_FORMAT_R32G32B32A32_SFLOAT;
    out.expand3 = tex.channels == 3;
    out.texelBytes = (tex.channels == 1 ? 1u : tex.channels == 2 ? 2u : 4u) * sizeof(float);
    const uint32_t mips = tex.mipmap ? mipCount(tex.w, tex.h) : 1;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mips > 1) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!allocator_.createImage(static_cast<uint32_t>(tex.w), static_cast<uint32_t>(tex.h), format, usage,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, mips, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, layers, 0,
                                tex.layers > 0 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D)) {
        return false;
    }

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = tex.mipmap ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = tex.repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = (tex.repeat && !tex.clampT) ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                    : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = static_cast<float>(mips);
    if (vkCreateSampler(allocator_.device().device(), &info, nullptr, &out.image.sampler) != VK_SUCCESS) {
        allocator_.destroyImage(out.image);
        return false;
    }
    return true;
}

const SceneVkImage* SceneGpuResources::userTexture(uint32_t id, MeshNode::UserTexture& tex) {
    if (tex.w <= 0 || tex.h <= 0) return nullptr;
    UserTexture& entry = userTextures_[id][tex.name];

    // 3-channel data is widened to RGBA (no RGB32F sampling guarantee).
    auto widen = [&](const float* src, size_t texels, std::vector<float>& storage) -> const float* {
        if (!entry.expand3) return src;
        storage.resize(texels * 4);
        for (size_t p = 0; p < texels; ++p) {
            storage[p * 4 + 0] = src[p * 3 + 0];
            storage[p * 4 + 1] = src[p * 3 + 1];
            storage[p * 4 + 2] = src[p * 3 + 2];
            storage[p * 4 + 3] = 1.0f;
        }
        return storage.data();
    };
    std::vector<float> widened;

    if (entry.generation != tex.generation || !entry.image.isValid()) {
        allocator_.destroyImage(entry.image);
        if (!createUserTexture(tex, entry)) {
            LOG_ERROR("SceneGpuResources: Failed allocating shader texture %s (%dx%d) of node %u",
                      tex.name.c_str(), tex.w, tex.h, id);
            userTextures_[id].erase(tex.name);
            return nullptr;
        }
        // The full image already carries every staged write.
        const uint32_t layers = tex.layers > 0 ? static_cast<uint32_t>(tex.layers) : 1;
        const size_t texels = static_cast<size_t>(tex.w) * tex.h * layers;
        if (tex.data.size() >= texels * static_cast<size_t>(tex.channels)) {
            ImageRegion region;
            region.layerCount = layers;
            const float* src = widen(tex.data.data(), texels, widened);
            allocator_.uploadImage(entry.image, src, texels * entry.texelBytes, region, tex.mipmap);
        }
        entry.generation = tex.generation;
        tex.subUpdates.clear();
        tex.sliceUpdates.clear();
        noteTextureUpload(texels * entry.texelBytes);
        return &entry.image;
    }

    for (const auto& sub : tex.subUpdates) {
        if (sub.w <= 0 || sub.h <= 0 || sub.data.empty()) continue;
        ImageRegion region;
        region.x = sub.x;
        region.y = sub.y;
        region.width = static_cast<uint32_t>(sub.w);
        region.height = static_cast<uint32_t>(sub.h);
        const size_t texels = static_cast<size_t>(sub.w) * sub.h;
        allocator_.uploadImage(entry.image, widen(sub.data.data(), texels, widened), texels * entry.texelBytes,
                               region, tex.mipmap);
    }
    tex.subUpdates.clear();
    for (const auto& slice : tex.sliceUpdates) {
        if (slice.layer < 0 || slice.layer >= tex.layers || slice.data.empty()) continue;
        ImageRegion region;
        region.layer = static_cast<uint32_t>(slice.layer);
        const size_t texels = static_cast<size_t>(tex.w) * tex.h;
        allocator_.uploadImage(entry.image, widen(slice.data.data(), texels, widened), texels * entry.texelBytes,
                               region, tex.mipmap);
    }
    tex.sliceUpdates.clear();
    return &entry.image;
}

void SceneGpuResources::pruneUserTextures(uint32_t id, const std::vector<MeshNode::UserTexture>& live) {
    auto node = userTextures_.find(id);
    if (node == userTextures_.end()) return;
    for (auto it = node->second.begin(); it != node->second.end();) {
        const bool present = std::any_of(live.begin(), live.end(),
                                         [&](const MeshNode::UserTexture& t) { return t.name == it->first; });
        if (present) {
            ++it;
        } else {
            allocator_.destroyImage(it->second.image);
            it = node->second.erase(it);
        }
    }
    if (node->second.empty()) userTextures_.erase(node);
}

const SceneVkImage* SceneGpuResources::shadeMap(const ShadeMapBinding& binding) {
    if (!binding.pixels || binding.width <= 0 || binding.height <= 0) return nullptr;
    Texture& map = shadeMaps_[binding.pixels];
    if (map.image.isValid() && map.generation == binding.generation &&
        map.image.width == static_cast<uint32_t>(binding.width) &&
        map.image.height == static_cast<uint32_t>(binding.height)) {
        return &map.image;
    }
    allocator_.destroyImage(map.image);
    TextureDesc desc{};
    desc.width = static_cast<uint32_t>(binding.width);
    desc.height = static_cast<uint32_t>(binding.height);
    desc.format = VK_FORMAT_R8_UNORM;
    desc.magFilter = VK_FILTER_NEAREST;
    desc.minFilter = VK_FILTER_NEAREST;
    desc.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    desc.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    desc.generateMipmaps = false;
    if (!allocator_.createTexture2D(binding.pixels, desc, map.image)) {
        LOG_ERROR("SceneGpuResources: Failed uploading the %dx%d shade map", binding.width, binding.height);
        shadeMaps_.erase(binding.pixels);
        return nullptr;
    }
    map.generation = binding.generation;
    return &map.image;
}

void SceneGpuResources::retainShadeMaps(const std::vector<const uint8_t*>& live) {
    for (auto it = shadeMaps_.begin(); it != shadeMaps_.end();) {
        if (std::find(live.begin(), live.end(), it->first) != live.end()) {
            ++it;
        } else {
            allocator_.destroyImage(it->second.image);
            it = shadeMaps_.erase(it);
        }
    }
}

void SceneGpuResources::releaseNodes(std::span<const uint32_t> ids) {
    for (uint32_t id : ids) {
        eraseNode(meshes_, id, [&](GpuMesh& m) {
            allocator_.destroyBuffer(m.vertices);
            allocator_.destroyBuffer(m.indices);
            allocator_.destroyBuffer(m.vegetation);
            allocator_.destroyBuffer(m.chunkIndices);
        });
        eraseNode(textures_, id, [&](Texture& t) { allocator_.destroyImage(t.image); });
        if (auto it = skins_.find(id); it != skins_.end()) {
            allocator_.destroyBuffer(it->second.buffer);
            skins_.erase(it);
        }
        if (auto it = storages_.find(id); it != storages_.end()) {
            allocator_.destroyBuffer(it->second.buffer);
            storages_.erase(it);
        }
        if (auto it = userTextures_.find(id); it != userTextures_.end()) {
            for (auto& [name, t] : it->second) allocator_.destroyImage(t.image);
            userTextures_.erase(it);
        }
    }
}

void SceneGpuResources::cleanup() {
    for (auto& [k, m] : meshes_) {
        allocator_.destroyBuffer(m.vertices);
        allocator_.destroyBuffer(m.indices);
        allocator_.destroyBuffer(m.vegetation);
        allocator_.destroyBuffer(m.chunkIndices);
    }
    meshes_.clear();
    for (auto& [k, t] : textures_) allocator_.destroyImage(t.image);
    textures_.clear();
    for (auto& [k, s] : skins_) allocator_.destroyBuffer(s.buffer);
    skins_.clear();
    for (auto& [k, s] : storages_) allocator_.destroyBuffer(s.buffer);
    storages_.clear();
    for (auto& [id, slots] : userTextures_) {
        for (auto& [name, t] : slots) allocator_.destroyImage(t.image);
    }
    userTextures_.clear();
    for (auto& [pixels, t] : shadeMaps_) allocator_.destroyImage(t.image);
    shadeMaps_.clear();
}

}  // namespace bro::scene::vk
