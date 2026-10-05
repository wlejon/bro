#pragma once

// The GPU copies of node data, keyed by node id (ids are never reused) and
// refreshed by generation: a node bumps a generation when its CPU data
// changes (scene/texture_source.h), and the copy here is rebuilt exactly when
// the generation it was built from is no longer the current one. Nothing is
// hashed and nothing is keyed by address.
//
// Copies of destroyed nodes are dropped by releaseNodes(); the GPU objects go
// through the frame core's deferred destruction, so frames still in flight
// keep what they use.

#include "scene/mesh_node.h"
#include "scene/shade_map.h"
#include "scene/texture_source.h"
#include "scene/vulkan/scene_vk_allocator.h"

#include <bromesh/mesh_data.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <unordered_map>

namespace bro::scene {
class SkinnedMeshNode;
}

namespace bro::scene::vk {

/// Which image of a node a texture copy holds. Unique per node; node types
/// never share a slot they would both use.
enum class TextureSlot : uint32_t {
    BaseColor,
    Normal,
    MetallicRoughness,
    Emissive,
    Image,          // sprite sheet, HTML panel, particle texture
    DecalAlbedo,
    DecalEmission,
    Occlusion,
};

struct GpuMesh {
    SceneVkBuffer vertices;   // PackedVertex (64 bytes)
    SceneVkBuffer indices;    // uint32
    uint32_t indexCount = 0;
    uint64_t generation = 0;
};

class SceneGpuResources {
public:
    /// Bytes per packed vertex (position, normal, uv, colour, tangent).
    static constexpr uint32_t kVertexStride = 64;
    /// Bytes per skin vertex (u16 joints[4], float weights[4]).
    static constexpr uint32_t kSkinStride = 24;

    explicit SceneGpuResources(SceneVkAllocator& allocator) : allocator_(allocator) {}

    /// Geometry `slot` of node `id` (LOD level + 1, or 0 for the base mesh).
    const GpuMesh* mesh(uint32_t id, uint32_t slot, uint64_t generation, const bromesh::MeshData& data);

    /// A node's RGBA8 image as a mipmapped, repeating texture; null when empty.
    const SceneVkImage* texture(uint32_t id, TextureSlot slot, const NodeTexture& tex);

    /// The skinned node's joint/weight vertex stream.
    VkBuffer skinAttributes(const SkinnedMeshNode& node, size_t vertexCount);

    /// A custom-shader sampler slot (2D, or a 2D array when `tex.layers` > 0).
    /// Staged sub-rect and slice writes are applied, then cleared.
    const SceneVkImage* userTexture(uint32_t id, MeshNode::UserTexture& tex);
    /// Drop the copies of node `id`'s sampler slots that `live` no longer has.
    void pruneUserTextures(uint32_t id, const std::vector<MeshNode::UserTexture>& live);

    /// The tile shade map (one per scene).
    const SceneVkImage* shadeMap(const ShadeMapBinding& binding);

    /// Drop every copy held for these nodes.
    void releaseNodes(std::span<const uint32_t> ids);
    void cleanup();

private:
    struct Texture {
        SceneVkImage image;
        uint64_t generation = 0;
    };
    struct UserTexture {
        SceneVkImage image;
        uint64_t generation = 0;
        uint32_t texelBytes = 0;
        bool expand3 = false;
    };
    struct Skin {
        SceneVkBuffer buffer;
        uint64_t generation = 0;
        size_t vertexCount = 0;
    };

    static uint64_t key(uint32_t id, uint32_t slot) { return (static_cast<uint64_t>(id) << 32) | slot; }
    template <typename Map, typename Destroy>
    static void eraseNode(Map& map, uint32_t id, Destroy&& destroy);

    bool createUserTexture(MeshNode::UserTexture& tex, UserTexture& out);

    SceneVkAllocator& allocator_;
    std::map<uint64_t, GpuMesh> meshes_;
    std::map<uint64_t, Texture> textures_;
    std::unordered_map<uint32_t, Skin> skins_;
    std::unordered_map<uint32_t, std::unordered_map<std::string, UserTexture>> userTextures_;
    Texture shadeMap_;
};

}  // namespace bro::scene::vk
