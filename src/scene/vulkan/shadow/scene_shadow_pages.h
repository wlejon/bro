#pragma once
// The cached shadow pages' GPU state (docs/scene-world-architecture.md §8;
// the paging is world/shadow_pages.h's, CPU-side in SceneRenderer):
//
//   pool      the physical pages, ShadowPlan::poolColumns x poolRows pages
//             of SP_STRIDE texels (a page and its border) in one D32 image,
//             sampled through the lighting layout's compare sampler.
//   scratch   the depth target the frame's views (rects of pages) are drawn
//             into: a redrawn page is copied from it into the pool
//             (pass_shadow_copy.h), an overlay sampled in it in place (the
//             lighting set's binding 15).
//   table     the frame's page table (ShadowPlan::table) in the frame's
//             upload memory, the lighting set's binding 14.
//   requests  the marking pass's bits (pass_shadow_mark.h), a bit a page
//             table entry, cleared and set each frame, then copied into a
//             host-visible ring and read once the frame's ticket completes
//             (takeRequests, before the next plan): a frame or two late.
//
// It is the world service's producer of shadow pages: it offers the pages
// the paging wants (ShadowPages::wants) as jobs of its two kinds (pages the
// plants draw into, pages they do not: their costs are a hundred times
// apart) and hands the grants back to the paging, which draws them the next
// frame (the plants' GPU plan must know the frame's views before the
// frame's graph runs); the shadow pass stamps each kind's span around the
// pages of it that it redraws, with their count, so a kind's cost is one of
// its pages' measured cost.

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/world/scene_world_service.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene {
class SceneRenderer;
class ShadowPages;
struct ShadowPlan;
}  // namespace bro::scene

namespace bro::scene::vk {

struct SceneGpu;
struct SceneFrame;
class SceneDefaults;
class SceneVkDevice;

class SceneShadowPages final : public WorldProducer {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// The frame begins with `renderer`'s shadow plan: the pool and scratch
    /// at its sizes, the page table uploaded. False (nothing shadowed this
    /// frame) when the plan has no maps or the images cannot be had (the
    /// renderer's pages are then all redrawn once they can).
    bool beginFrame(SceneGpu& gpu, SceneRenderer& renderer);
    /// The pool and scratch hold this frame's plan.
    bool ready() const { return ready_; }
    /// The pool's view (1x1 until a light casts a shadow; it never shrinks,
    /// so the pages survive frames without shadows).
    VkImageView poolView() const { return pool.view; }
    /// The scratch target's view: the frame's overlays, sampled in place.
    VkImageView scratchView() const { return scratch.view; }
    /// The frame's page table (a single zero entry when there is none).
    VkDescriptorBufferInfo table() const { return table_; }
    /// The world service's kinds of shadow page jobs: a page the plants draw
    /// into, and one only what stands still draws into.
    int kind() const { return kind_; }
    int stillKind() const { return stillKind_; }

    /// The requests' buffer this frame (cleared, then marked).
    VkBuffer requestBuffer() const { return requests_.buffer; }
    VkDeviceSize requestBytes() const { return requestBytes_; }
    /// The frame's requests are copied out after the marking (recorded into
    /// `cmd`, after the marking's writes): they name plan `serial`.
    void recordReadback(VkCommandBuffer cmd, uint64_t serial);
    /// The frame that recorded the readback was submitted as `ticket`.
    void submitted(uint64_t ticket);
    /// Every readback the GPU has finished, handed to `pages` (oldest first).
    void takeRequests(SceneVkDevice& device, ShadowPages& pages);

    // WorldProducer: the paging's wants, and its grants back.
    void offer(SceneFrame& frame, WorldService& service) override;
    double record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) override;

    SceneVkImage pool;
    SceneVkImage scratch;

private:
    static constexpr int kRing = 4;
    static constexpr VkImageUsageFlags kPoolUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    static constexpr VkImageUsageFlags kScratchUsage =
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    struct Readback {
        SceneVkBuffer buffer;
        uint64_t serial = 0;
        uint64_t ticket = 0;
        bool recorded = false;   // copied into by a frame not yet submitted
        bool pending = false;    // submitted, not read
    };
    bool ensureImage(SceneGpu& gpu, SceneVkImage& image, uint32_t side, VkImageUsageFlags usage, const char* what);
    bool ensureRequests(SceneGpu& gpu, VkDeviceSize bytes);

    SceneVkAllocator* allocator_ = nullptr;
    int kind_ = -1;
    int stillKind_ = -1;
    bool ready_ = false;
    VkDescriptorBufferInfo table_{};
    SceneVkBuffer requests_;
    VkDeviceSize requestBytes_ = 0;
    Readback ring_[kRing];
    int next_ = 0;
};

}  // namespace bro::scene::vk
