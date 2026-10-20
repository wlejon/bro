#pragma once
// Far cards (docs/scene-world-architecture.md §2): a far world cell's crowns
// drawn once into a card and the card drawn in their place, frame after
// frame, until the view has moved enough that it would show.
//
// Which cells. A forest node's world cells (ForestNode::cells) in view
// whose every tree draws as a crown alone (forest::crownsAlone, planForest's
// far cells) and whose box spans at most kMaxTile pixels on screen.
//
// The capture. A cell's crowns are drawn through the camera itself, its
// projection cropped to a square round the cell's rectangle on screen at
// one texel a pixel: the same eye, lighting, shadows, fog and sample
// pattern (4x, as the frame's), so a card fresh from its capture is the
// pixels the crowns would have drawn. Up to kMaxCaptures a frame go into
// one multisampled scratch target, each in a square of its own; a compute
// pass (far_card_resolve.comp) resolves each into its tile of the atlas,
// 8 bytes a texel (far_card_layout.h): the covered samples' mean colour,
// how many were covered, and the nearest depth across the card's own range.
// The capture is centred on a pixel corner so a fresh card's texels are the
// screen's pixels.
//
// The card. A grid of quads over its tile (FAR_CARD_GRID_TEXELS texels a
// side each; the cards draw grouped by tile size, a draw a grid), each grid
// point stood at the nearest depth the tile holds round it and taken back
// through the capture's inverse view-projection to where it lay (relative
// to the eye now), then through the camera: the card bends over its crowns'
// relief and keeps their parallax. Each fragment takes its nearest texel
// (a filter would blur the crowns' pixel-sized leaves; the texel step as the
// eye moves is lost in their own shimmer), writes its depth reprojected the
// same way, and keeps as many of its samples as the crowns covered
// (dithered as forestSampleMask's cover is).
//
// Which cells get room. Cells with no card are weighed a quarter a frame
// (the rectangle on screen is the cost); a card standing in is checked every
// frame. New cards go in by trees a texel, the most first; a full atlas
// gives back the card longest unused, else the standing card that saves the
// least, if less than the new one would.
//
// When a card is captured again: the node's data moved, the light or fog
// changed, its size on screen is off the captured size by a quarter, or the
// eye has moved round it far enough that the relief inside the cell would
// shift by kParallaxPixels. A stale card keeps standing in until its new
// capture, the stalest first within the frame's budget; a cell with no card
// yet draws its crowns, as does a cell grown past kMaxTile. The light is
// captured, not relit: a moving sun recaptures
// (docs/scene-world-architecture.md §2 notes the departure).
//
// The trees a card stands for are left out of the camera's plan
// (SceneFrame::forestCellSkip); their shadow casters stay. Needs
// BRO_FAR_CARDS=1 for now; under BRO_WORLD_CELLS=0 the GPU planner cannot
// leave a cell out, and there are no cards.

#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_target_format.h"
#include "scene/vulkan/scene_view.h"
#include "scene/vulkan/shaders/far_card_layout.h"
#include "scene/world_cells.h"

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <vulkan/vulkan.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace bro::scene {
class ForestNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

/// BRO_FAR_CARDS=1 (read once).
bool farCardsEnabled();

class FarCards {
public:
    static constexpr uint32_t kAtlasSize = 4096;   // texels a side (8 bytes each: 128 MB)
    static constexpr uint32_t kPage = 256;         // a page of the atlas: one tile size's slots
    static constexpr uint32_t kPages = (kAtlasSize / kPage) * (kAtlasSize / kPage);
    static constexpr uint32_t kMinTile = 16, kMaxTile = 256;
    static constexpr uint32_t kScratch = 1024;     // the captures' multisampled target a side
    static constexpr uint32_t kMaxCaptures = 16;   // a frame
    static constexpr uint32_t kCaptureSamples = 4; // the scratch target's
    static constexpr float kParallaxPixels = 1.5f;

    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);
    void releaseNodes(std::span<const uint32_t> ids);

    /// This frame's cards among `forests`' cells and the captures they need,
    /// and frame.forestCellSkip for them. Before the forests are planned.
    void select(SceneFrame& frame, const std::vector<ForestNode*>& forests);

    /// The captures chosen (PassFarCardCapture): drawn into the scratch
    /// target, resolved into the atlas.
    bool capturing() const { return !captures_.empty(); }
    void capture(SceneFrame& frame);
    /// The cards (PassFarCardDraw), in the open HDR scope.
    bool drawing() const { return !records_.empty(); }
    void draw(SceneFrame& frame);

    /// What a capture's lighting set binds in place of the frame's far
    /// depth: cleared to the far plane.
    VkImageView clearFarDepth() const { return clearFar_.view; }

private:
    struct Tile {
        uint16_t page = 0xffff, slot = 0, size = 0;
        bool valid() const { return page != 0xffff; }
    };
    struct Page {
        uint32_t size = 0;   // its tiles' side (0: free)
        std::bitset<(kPage / kMinTile) * (kPage / kMinTile)> used;
        uint32_t count = 0;
    };
    struct Card {
        uint32_t node = 0, cell = 0;
        uint64_t generation = 0;   // the node's data
        Tile tile;
        bool captured = false;
        bromath::Vec3 eye;              // the capture's (absolute)
        bromath::Mat4 invRelViewProj;   // its clip -> position relative to its eye
        float pixels = 0.0f;            // the cell's extent on screen then
        float across = 0.0f;            // the cell's bounding sphere's diameter on screen then
        float fallbackDepth = 0.0f;     // the cell's centre in its clip depth
        float depthRange[2] = {};       // the capture depths the atlas's 24 bits span
        float value = 0.0f;             // trees a texel: what the card saves for the room it takes
        uint32_t samples = 0;           // a pixel's at capture
        float light[16] = {};           // the light and fog then (lightKey)
        uint64_t used = 0;              // the frame it last stood in
        uint64_t capturedAt = 0;        // the frame of its capture
    };
    struct Capture {
        uint64_t key = 0;
        ForestNode* node = nullptr;
        uint32_t cell = 0;
        SceneView view;
        uint32_t x = 0, y = 0;   // in the scratch target
    };
    struct NodeCells {
        uint64_t generation = ~uint64_t{0};
        bromath::Mat4 world;
        std::vector<bromath::AABB3> bounds;   // world, each cell's
        std::vector<float> extent;            // forest::CellExtent a cell (4 floats)
        std::vector<uint8_t> carded;          // a cell's card exists (cards_ holds it)
    };

    bool ensureImages(SceneGpu& gpu);
    Tile allocTile(uint32_t size);
    void freeTile(Tile& t);
    uint32_t tileX(const Tile& t) const;
    uint32_t tileY(const Tile& t) const;
    void dropCard(uint64_t key);
    void lightKey(const SceneFrame& frame, float out[16]) const;
    /// The card's record (FAR_CARD_RECORD_VEC4S vec4s) for the eye at `eye`.
    void putRecord(const Card& c, const bromath::Vec3& eye, float* out) const;

    SceneVkDevice* device_ = nullptr;
    bool ready_ = false;
    uint64_t frame_ = 0;
    std::unordered_map<uint64_t, Card> cards_;   // by node id << 32 | cell
    std::unordered_map<uint32_t, NodeCells> nodes_;
    std::array<Page, kPages> pages_{};
    std::vector<Capture> captures_;
    std::vector<float> records_;   // this frame's cards', by tile size
    struct DrawGroup {
        uint32_t vertices, first, count;   // a grid size's draw: its records [first, first + count)
    };
    std::vector<DrawGroup> groups_;

    // The atlas (far_card_layout.h's texels), the scratch target, and the
    // far depth for captures.
    SceneVkImage atlas_;
    SceneVkImage scratchColor_, scratchDepth_;
    SceneVkImage clearFar_;
    bool clearFarReady_ = false;

    // The resolve (compute) and the card draw.
    VkDescriptorSetLayout resolveSetLayout_ = VK_NULL_HANDLE, drawSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout_ = VK_NULL_HANDLE, drawLayout_ = VK_NULL_HANDLE;
    VkPipeline resolvePipeline_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE, fsIndirect_ = VK_NULL_HANDLE;
    PipelineVariants drawPipelines_;
};

}  // namespace bro::scene::vk
