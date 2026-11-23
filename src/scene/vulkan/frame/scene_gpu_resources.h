#pragma once

// The GPU copies of node data, keyed by node id (ids are never reused) and
// refreshed by generation: a node bumps a generation when its CPU data
// changes (scene/texture_source.h), and the copy here is rebuilt exactly when
// the generation it was built from is no longer the current one. Nothing is
// hashed and nothing is keyed by address.
//
// Shared geometry (scene/nodes/shared_geometry.h) is the exception: one copy
// keyed by the geometry's own generation, for every node that draws it.
//
// Copies of destroyed nodes are dropped by releaseNodes(); the GPU objects go
// through the frame core's deferred destruction, so frames still in flight
// keep what they use.

#include "scene/nodes/mesh_node.h"
#include "scene/world/shade_map.h"
#include "scene/texture_source.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"

#include <bromath/aabb.h>
#include <bromesh/mesh_data.h>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene {
class SkinnedMeshNode;
}

namespace bro::scene::vk {

/// Which image of a node a texture copy holds. Unique per node; node types
/// never share a slot they would both use. The slot also picks how the image
/// is stored and sampled:
///   - material maps: mipmapped, repeating;
///   - Sprite: nearest, no mips, clamped (pixel-exact sheets);
///   - Html: linear, no mips, clamped (a panel drawn at its own resolution);
///   - Particle: sRGB storage, so the sampler returns linear texels for the
///     linear HDR pass, linear, no mips, clamped;
///   - decal maps: mipmapped, clamped (the projection box is the image).
enum class TextureSlot : uint32_t {
    BaseColor,
    Normal,
    MetallicRoughness,
    Emissive,
    Sprite,
    Html,
    Particle,
    DecalAlbedo,
    DecalEmission,
    Occlusion,
};

/// A run of a big mesh's triangles that lie together (SceneGpuResources::chunked):
/// its range in GpuMesh::chunkIndices and its bounds in the mesh's own space.
struct MeshChunk {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    bromath::AABB3 bounds;
};

struct GpuMesh {
    SceneVkBuffer vertices;   // PackedVertex (64 bytes)
    SceneVkBuffer indices;    // uint32
    // A big triangle mesh's indices again, its triangles sorted into chunks
    // by where they lie, so a pass draws the chunks its view reaches
    // (SceneMeshDrawer::bindGeometryAndDraw's ChunkCull); empty for a small
    // one. Built once a node asks (chunked), not for a line mesh.
    SceneVkBuffer chunkIndices;
    std::vector<MeshChunk> chunks;
    bool chunksTried = false;
    SceneVkBuffer vegetation; // MeshData::vegetation (64 bytes a vertex), when the mesh has it
    uint32_t indexCount = 0;
    uint64_t generation = 0;
};

/// An instanced node's rows held on the GPU (SceneGpuResources::instances):
/// 16 floats a row, and each row's own index (0..count-1).
struct GpuInstances {
    SceneVkBuffer rows;
    SceneVkBuffer ids;
    size_t count = 0;
    uint64_t generation = 0;     // of the rows held (0: none)
    uint64_t seen = 0;           // the generation last asked for
    uint32_t stableRenders = 0;  // renders it has been asked for unchanged
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
    /// The same, its triangles chunked (GpuMesh::chunks) when it has
    /// kChunkMinTriangles or more: for a triangle list whose draws may be cut
    /// to their view (a static mesh drawn as triangles).
    const GpuMesh* chunked(uint32_t id, uint32_t slot, uint64_t generation, const bromesh::MeshData& data);
    static constexpr uint32_t kChunkMinTriangles = 32768;
    static constexpr uint32_t kChunkTriangles = 4096;   // a chunk's, about
    /// The one GPU copy of shared geometry every node drawing it draws
    /// (keyed by its own generation, never a node's), `chunked` as chunked()
    /// is; kept while anything holds the geometry (pruneShared).
    const GpuMesh* sharedMesh(const std::shared_ptr<const SharedGeometry>& geometry, bool chunked = false);
    /// Drop the copies of shared geometry nothing holds any more.
    void pruneShared();

    /// Node `id`'s instance rows (`count` of them, 16 floats each) held on
    /// the GPU once they have stayed at `generation` for kInstancesStable
    /// renders, uploaded once and drawn from there until they change; null
    /// while they are changing (the draw copies them into the frame's upload
    /// memory instead, as rows that move every frame should be).
    const GpuInstances* instances(uint32_t id, uint64_t generation, const float* rows, size_t count);
    static constexpr uint32_t kInstancesStable = 3;

    /// A node's RGBA8 image as a mipmapped, repeating texture; null when empty.
    const SceneVkImage* texture(uint32_t id, TextureSlot slot, const NodeTexture& tex);

    /// The skinned node's joint/weight vertex stream.
    VkBuffer skinAttributes(const SkinnedMeshNode& node, size_t vertexCount);

    /// A custom-shader sampler slot (2D, or a 2D array when `tex.layers` > 0).
    /// Staged sub-rect and slice writes are applied, then cleared; a slot
    /// backed by a GPU buffer is copied from it once a render.
    const SceneVkImage* userTexture(uint32_t id, MeshNode::UserTexture& tex);
    /// A render begins: buffer-backed slots copy again on their next use.
    void beginRender() { ++render_; }
    /// Drop the copies of node `id`'s sampler slots that `live` no longer has.
    void pruneUserTextures(uint32_t id, const std::vector<MeshNode::UserTexture>& live);

    /// The GPU copy of a tile shade map, keyed by its CPU map (every node a
    /// TileWorld draws shares one), re-uploaded when its generation moves.
    const SceneVkImage* shadeMap(const ShadeMapBinding& binding);
    /// Drop the copies of shade maps not in `live` (this frame's).
    void retainShadeMaps(const std::vector<const uint8_t*>& live);

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
        MeshNode::UserTexture::Encoding encoding = MeshNode::UserTexture::Encoding::Float32;
        uint64_t copiedRender = 0;   // the render (render_) a buffer slot was last copied in
        uint64_t copiedLog = 0;      // the source's layer log count copied up to (0: none yet)
    };
    struct Skin {
        SceneVkBuffer buffer;
        uint64_t generation = 0;
        size_t vertexCount = 0;
    };

    struct Shared {
        GpuMesh mesh;
        std::weak_ptr<const SharedGeometry> owner;
    };
    /// `entry` rebuilt from `data` (false, and empty, when an upload fails).
    bool upload(GpuMesh& entry, uint64_t generation, const bromesh::MeshData& data);
    /// `entry`'s triangles sorted into chunks (GpuMesh::chunks), when it is big enough.
    void buildChunks(GpuMesh& entry, const bromesh::MeshData& data);
    void destroy(GpuMesh& m);

    static uint64_t key(uint32_t id, uint32_t slot) { return (static_cast<uint64_t>(id) << 32) | slot; }
    template <typename Map, typename Destroy>
    static void eraseNode(Map& map, uint32_t id, Destroy&& destroy);

    bool createUserTexture(MeshNode::UserTexture& tex, UserTexture& out);

    SceneVkAllocator& allocator_;
    uint64_t render_ = 1;
    std::map<uint64_t, GpuMesh> meshes_;
    std::unordered_map<uint64_t, Shared> shared_;   // by SharedGeometry::generation
    std::map<uint64_t, Texture> textures_;
    std::unordered_map<uint32_t, Skin> skins_;
    std::unordered_map<uint32_t, GpuInstances> instances_;
    std::unordered_map<uint32_t, std::unordered_map<std::string, UserTexture>> userTextures_;
    std::unordered_map<const uint8_t*, Texture> shadeMaps_;
};

}  // namespace bro::scene::vk
