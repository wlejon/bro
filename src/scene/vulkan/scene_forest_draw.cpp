// A forest node's draws, as both of the drawer's planners make them
// (scene_forest_draw.h).

#include "scene/vulkan/scene_forest_draw.h"

#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_descriptors.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk::forest {

// The wood draws indexed, so the vertex stage runs once a corner, not once a
// triangle's corner (scene_forest_tube.glsl's vertex numbering): bark of
// `sides` sides is barkRows + 1 rings of sides + 1 vertices a segment, each
// side two triangles between each two; a twig (sides 0) `rows` + 1 rows of its two sides
// a piece (four corners for the plain twigs' one row). Unindexed, a bark
// side's ring points were each worked out three times, a twig's ends (the
// shoot model, posed) three times over.
const bromesh::MeshData& woodIndices(int sides, uint32_t rows) {
    static bromesh::MeshData twigs, fineTwigs, bark[3], curved[3];
    bromesh::MeshData& m = sides == 0 ? (rows > 1 ? fineTwigs : twigs)
                                      : (rows > 1 ? curved : bark)[sides == kFarSides ? 0 : sides == kNearSides ? 1 : 2];
    if (!m.indices.empty()) return m;
    if (sides == 0) {
        const uint32_t per = 2 * (rows + 1);
        for (uint32_t p = 0; p < kTwigPiecesPerEntry; ++p)
            for (uint32_t r = 0; r < rows; ++r)
                for (uint32_t c : {0u, 1u, 2u, 2u, 1u, 3u}) m.indices.push_back(per * p + 2 * r + c);
        m.positions.assign(3 * per * kTwigPiecesPerEntry, 0.0f);
        return m;
    }
    const uint32_t ring = static_cast<uint32_t>(sides) + 1, bRows = rows;
    for (uint32_t k = 0; k < kSegmentsPerEntry; ++k)
        for (uint32_t r = 0; r < bRows; ++r) {
            const uint32_t b = (k * (bRows + 1) + r) * ring, t = b + ring;
            for (uint32_t s = 0; s < static_cast<uint32_t>(sides); ++s)
                for (uint32_t v : {b + s, b + s + 1, t + s, t + s, b + s + 1, t + s + 1}) m.indices.push_back(v);
        }
    m.positions.assign(3 * (bRows + 1) * ring * kSegmentsPerEntry, 0.0f);
    return m;
}

bool drawBase(SceneFrame& frame, SceneDefaults& defaults, VkDevice device, ForestNode& node, VkDescriptorSet set,
              DrawBase& out) {
    SceneGpuResources& res = frame.gpu.resources;
    out.leafMesh = res.mesh(node.id(), kLeafSlot, node.geometryGeneration(), node.leafBatch());
    out.cardMesh = res.mesh(node.id(), kCardSlot, node.cardGeneration(), node.cardBatch());
    out.clusterMesh = res.mesh(node.id(), kClusterSlot, node.geometryGeneration(), node.clusterBatch());
    if (!out.leafMesh || !out.cardMesh || !out.clusterMesh) return false;

    // The draws' shared state: the leaves' material, the forest flag.
    const SceneView& view = frame.view;
    MeshDraw& base = out.base;
    base = MeshDraw{};
    base.nodeId = node.id();
    base.vertexSet = set;
    std::memcpy(base.push.model, view.relative(node.worldMatrix()).data, sizeof(base.push.model));
    std::memcpy(base.push.baseColor, node.color(), sizeof(base.push.baseColor));
    base.push.baseColor[3] = 1.0f;
    base.push.pbrParams[0] = node.metallic();
    base.push.pbrParams[1] = node.roughness();
    base.push.pbrParams[2] = node.alphaCutoff();
    base.push.extra[0] = node.nearClipDist();
    base.push.extra[1] = node.subsurface();
    base.push.extra[2] = 0.0f;
    base.push.extra[3] = 1.0f + 256.0f;   // no atlas
    uint32_t flags = mesh_flags::kForest | mesh_flags::vertexColor(node.vertexColorMode());
    // In a 4x target a hand-over splits each pixel's samples, a blend in
    // quarters rather than a stipple of whole pixels.
    const uint32_t msaa = frame.gpu.targets.samples() == VK_SAMPLE_COUNT_4_BIT ? mesh_flags::kForestMsaa : 0u;
    flags |= msaa;
    if (frame.ssr) flags |= mesh_flags::kReflectance;
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    out.leafFlags = flags | mesh_flags::kTwoSided;
    if (node.mesh().hasTangents()) out.leafFlags |= mesh_flags::kHasTangents;
    out.barkFlags = mesh_flags::kForest | msaa | (flags & (mesh_flags::kReflectance | mesh_flags::kReceivesShadow));
    out.spriteFlags = out.barkFlags | mesh_flags::kForestSprite;
    // The leaves' maps (an albedo with alpha is the usual leaf); sprites and
    // bark take the plain colour.
    out.leafMaterial = VK_NULL_HANDLE;
    const SceneVkImage* albedo = res.texture(node.id(), TextureSlot::BaseColor, node.baseColorTexture());
    const SceneVkImage* normal = res.texture(node.id(), TextureSlot::Normal, node.normalTexture());
    if (albedo || normal) {
        out.leafMaterial = frame.gpu.device.frameSet(defaults.materialLayout);
        SceneVkDescriptorWriter w;
        w.writeImage(0, albedo ? albedo->view : defaults.white.view, albedo ? albedo->sampler : defaults.sampler);
        w.writeImage(1, normal ? normal->view : defaults.flatNormal.view, normal ? normal->sampler : defaults.sampler);
        w.writeImage(2, defaults.white.view, defaults.sampler);
        w.writeImage(3, defaults.black.view, defaults.sampler);
        w.writeImage(4, defaults.white.view, defaults.sampler);
        w.updateSet(device, out.leafMaterial);
        if (albedo) out.leafFlags |= mesh_flags::kAlbedoMap;
        if (normal) out.leafFlags |= mesh_flags::kNormalMap;
    }
    return true;
}

MeshDraw groupDraw(SceneFrame& frame, ForestNode& node, const DrawBase& b, int k, bool shadow, int slot, bool posed,
                   uint32_t first, uint32_t n) {
    SceneGpuResources& res = frame.gpu.resources;
    MeshDraw d = b.base;
    d.shadowOnly = shadow;
    d.castsShadow = shadow;
    // Across every forest node: cores, leaves and bark (near, solid)
    // first, then the rest of the scene, then the far tiers, so the
    // clumps and crowns behind near trees and ground are rejected
    // before they shade. Seen from over the canopy the crowns are
    // what hides the rest (the floor, the leaves under them), so
    // there they keep their place before it.
    // (Ranked as before the curved bark and fine twigs had groups of
    // their own, each with its plain one: the clumps and crowns
    // under kOrderMid, so near them the static meshes and grass
    // behind fail the depth test, not shade.)
    const int rank = k == Bark20Curved ? Bark20 - 2 : k == Bark10Curved ? Bark10 - 2
                   : k <= LeafCard ? k : k <= Twigs ? k - 2 : k - 3;
    d.order = b.farLast && (k == Clumps || k == Crowns) ? static_cast<uint8_t>(MeshDraw::kOrderFar + rank)
                                                        : static_cast<uint8_t>(rank);
    if (shadow) d.shadowTileMask = 1u << slot;
    // The timed passes' name for it: the group and the node's leaf form.
    static constexpr const char* kLabel[kGroups][3] = {
        {"forest:cores:broad", "forest:cores:needle", "forest:cores:scale"},
        {"forest:leafNear:broad", "forest:leafNear:needle", "forest:leafNear:scale"},
        {"forest:leafCard:broad", "forest:leafCard:needle", "forest:leafCard:scale"},
        {"forest:bark20c:broad", "forest:bark20c:needle", "forest:bark20c:scale"},
        {"forest:bark10c:broad", "forest:bark10c:needle", "forest:bark10c:scale"},
        {"forest:bark20:broad", "forest:bark20:needle", "forest:bark20:scale"},
        {"forest:bark10:broad", "forest:bark10:needle", "forest:bark10:scale"},
        {"forest:bark5:broad", "forest:bark5:needle", "forest:bark5:scale"},
        {"forest:twigs:broad", "forest:twigs:needle", "forest:twigs:scale"},
        {"forest:twigsFine:broad", "forest:twigsFine:needle", "forest:twigsFine:scale"},
        {"forest:blooms:broad", "forest:blooms:needle", "forest:blooms:scale"},
        {"forest:clumps:broad", "forest:clumps:needle", "forest:clumps:scale"},
        {"forest:crowns:broad", "forest:crowns:needle", "forest:crowns:scale"},
        {"forest:runs:broad", "forest:runs:needle", "forest:runs:scale"}};
    static constexpr const char* kShadowLabel[kGroups] = {
        "shadow:forest:cores", "shadow:forest:leafNear", "shadow:forest:leafCard", "shadow:forest:bark20c",
        "shadow:forest:bark10c", "shadow:forest:bark20", "shadow:forest:bark10",
        "shadow:forest:bark5", "shadow:forest:twigs", "shadow:forest:twigsFine", "shadow:forest:blooms", "shadow:forest:clumps",
        "shadow:forest:crowns", "shadow:forest:runs"};
    d.timerLabel = kLabel[k][std::clamp(static_cast<int>(node.look().leafForm), 0, 2)];
    if (shadow) d.shadowTimerLabel = kShadowLabel[k];
    switch (k) {
    case LeafNear:
    case LeafCard: {
        // In colour, past the card widening, each kept leaf is a
        // cluster of leaves (Lod::clusters); a caster stays a card.
        const bool cluster = k == LeafCard && !shadow && b.clusters;
        const GpuMesh* m = k == LeafNear ? b.leafMesh : cluster ? b.clusterMesh : b.cardMesh;
        d.kind = MeshKind::ForestLeaf;
        d.vertices = m->vertices.buffer;
        d.indices = m->indices.buffer;
        d.indexCount = m->indexCount;
        d.instanceCount = n;
        d.firstInstance = first;
        d.twoSided = true;
        d.materialSet = b.leafMaterial;
        // A needle or scale-leaf node's leaf is its spray's card, its
        // needles cut out by mesh.frag's SCENE_FOREST_SPRAY build.
        const bool spray = !cluster && node.look().leafForm != 0;
        // A broad form's own card near: its outline cut per pixel,
        // rounded and pointed (Look::leafCut; untextured: a
        // texture's alpha is its outline).
        const bool cut = k == LeafNear && node.look().leafForm == 0 && node.look().leafCut &&
                         (b.leafFlags & mesh_flags::kAlbedoMap) == 0u;
        d.push.pbrParams[3] = static_cast<float>(b.leafFlags | (cluster ? mesh_flags::kForestCluster : 0u) |
                                                 (spray ? mesh_flags::kForestSpray : 0u) |
                                                 (cut ? mesh_flags::kForestLeafCut : 0u) |
                                                 (!posed ? mesh_flags::kForestLeafInline : 0u));
        break;
    }
    case Bark20Curved:
    case Bark10Curved:
    case Bark20:
    case Bark10:
    case Bark5: {
        const int sides = k == Bark20 || k == Bark20Curved ? kNearestSides
                          : k == Bark10 || k == Bark10Curved ? kNearSides : kFarSides;
        const bool curved = k == Bark20Curved || k == Bark10Curved;
        const uint32_t rows = curved ? barkRows(sides) : 1u;
        d.kind = MeshKind::ForestTube;
        d.indexCount = kSegmentsPerEntry * static_cast<uint32_t>(sides) * 6u * rows;
        if (const GpuMesh* ix = res.mesh(node.id(), (curved ? kTubeCurveSlot : kTubeSlot) + sides, 1,
                                         woodIndices(sides, rows)))
            d.indices = ix->indices.buffer;
        d.instanceCount = n;
        d.firstInstance = first;
        const float* bc = node.barkColor();
        d.push.baseColor[0] = bc[0];
        d.push.baseColor[1] = bc[1];
        d.push.baseColor[2] = bc[2];
        d.push.baseColor[3] = 1.0f;
        d.push.pbrParams[1] = node.barkRoughness();
        d.push.extra[1] = 0.0f;
        // The width (px) the rings' sides morph by (forestTubeMorph),
        // the tree's choice of sides by its trunk's (treeLod):
        // negative, never the twigs' 3.
        d.push.extra[2] = -(shadow ? b.shadowLod : b.lod).barkSidesPixels;
        d.push.pbrParams[3] = static_cast<float>(b.barkFlags);
        break;
    }
    case Twigs:
    case TwigsFine: {
        // Ribbons facing the eye (mesh_forest_tube.vert's twig
        // build, push.extra.z 2 + its rows a piece) in the bark's
        // colour.
        const uint32_t rows = k == TwigsFine ? kTwigFineRows : 1u;
        d.kind = MeshKind::ForestTube;
        d.indexCount = kTwigPiecesPerEntry * kVertsPerTwig * rows;
        // (Slot kTubeSlot + 1, which no bark's sides take, for the fine pattern.)
        if (const GpuMesh* ix = res.mesh(node.id(), kTubeSlot + (rows > 1 ? 1 : 0), 1, woodIndices(0, rows)))
            d.indices = ix->indices.buffer;
        d.instanceCount = n;
        d.firstInstance = first;
        d.twoSided = true;
        const float* bc = node.barkColor();
        d.push.baseColor[0] = bc[0];
        d.push.baseColor[1] = bc[1];
        d.push.baseColor[2] = bc[2];
        d.push.baseColor[3] = 1.0f;
        d.push.pbrParams[1] = node.barkRoughness();
        d.push.extra[1] = 0.0f;
        d.push.extra[2] = static_cast<float>(FOREST_TWIG_ROWS_BIAS + rows);
        d.push.pbrParams[3] = static_cast<float>(b.barkFlags);
        break;
    }
    case Cores:
    case Clumps:
        d.kind = MeshKind::ForestSprite;
        d.push.extra[2] = k == Cores ? FOREST_SPRITE_CORES : FOREST_SPRITE_CLUMPS;
        d.indexCount = kClumpsPerEntry * kVertsPerClump;
        d.instanceCount = n;
        d.firstInstance = first;
        d.twoSided = true;   // no culling; lit from its front
        d.push.pbrParams[3] = static_cast<float>(b.spriteFlags);
        break;
    case Runs:
        d.kind = MeshKind::ForestSprite;
        d.push.extra[2] = FOREST_SPRITE_RUNS;
        d.indexCount = kRunsPerEntry * FOREST_CARD_VERTICES;
        d.instanceCount = n;
        d.firstInstance = first;
        d.twoSided = true;
        d.push.pbrParams[3] = static_cast<float>(b.spriteFlags);
        break;
    case Blooms:
        // A card a bloom (mesh_forest_bloom.vert), shaped and lit by
        // mesh.frag's SCENE_FOREST_BLOOM build: white times the
        // instance colour, which is the bloom's own; petals matte.
        d.kind = MeshKind::ForestSprite;
        d.indexCount = kBloomsPerEntry * kVertsPerBloom;
        d.instanceCount = n;
        d.firstInstance = first;
        d.twoSided = true;
        d.push.baseColor[0] = d.push.baseColor[1] = d.push.baseColor[2] = d.push.baseColor[3] = 1.0f;
        d.push.pbrParams[0] = 0.0f;
        d.push.pbrParams[1] = 0.8f;
        d.push.pbrParams[2] = 0.0f;
        d.push.extra[1] = 0.0f;
        d.push.pbrParams[3] = static_cast<float>(b.barkFlags | mesh_flags::kTwoSided | mesh_flags::kForestBloom);
        break;
    case Crowns:
        d.kind = MeshKind::ForestSprite;
        d.push.extra[2] = FOREST_SPRITE_CROWNS;
        d.indexCount = n * FOREST_CARD_VERTICES;
        // A caster is two discs through the crown (the second, a spray
        // node's, shadow_forest_sprite.vert culls for a broad one's).
        d.instanceCount = shadow ? 2u : 1u;
        d.firstVertex = first * FOREST_CARD_VERTICES;
        d.twoSided = true;
        d.push.pbrParams[3] = static_cast<float>(b.spriteFlags);
        break;
    default: break;
    }
    // The near sphere (Lod::leavesRange): the camera's leaves and
    // clumps cut each its pixels to its side of it, across the band
    // (mesh.frag's MESH_FOREST_REVEAL); the sphere rides in the
    // emissive's xy (its intensity 0, so it adds no light). A
    // caster keeps each leaf or clump by its own distance as the
    // camera keeps pixels, its side in the emissive's z
    // (PassShadow's params.yzw; shadow_forest_*.vert's forestCasterKept).
    const bool leafSide = k == Cores || k == LeafNear || k == LeafCard || k == Twigs || k == TwigsFine;
    if (b.lod.leavesRange > 0.0f && (leafSide || k == Clumps || k == Runs)) {
        if (!shadow)
            d.push.pbrParams[3] =
                static_cast<float>(static_cast<uint32_t>(d.push.pbrParams[3]) | mesh_flags::kForestReveal);
        d.push.emissive[0] = b.lod.leavesRange;
        d.push.emissive[1] = b.lod.leavesBand;
        d.push.emissive[2] = shadow ? (k == Clumps || k == Runs ? -1.0f : 1.0f) : 0.0f;
        d.push.emissive[3] = 0.0f;
    }
    return d;
}

}  // namespace bro::scene::vk::forest
