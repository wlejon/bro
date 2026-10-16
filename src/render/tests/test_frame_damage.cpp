// bro_frame_damage_test: the damage between two frames from their pixels
// (render/frame_damage.h). The tile grid to rects; then frames that differ
// in known places, compared on the CPU (diffFrames) and on the GPU
// (GpuFrameDiff), which must agree with each other and with the tiles the
// changes touch. Run with BRO_VK_VALIDATION=1 to also fail on any
// validation error.

#include "render/frame_damage.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "render/vulkan_util.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace bro;
using render::DamageRect;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << "  FAIL: " #cond " (line " << __LINE__ << ")" << std::endl; \
            ++gFailures;                                                             \
        }                                                                            \
    } while (0)

std::string str(const std::vector<DamageRect>& rs) {
    std::string s = "[";
    for (const DamageRect& r : rs) {
        s += " " + std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.width) + "x" +
             std::to_string(r.height);
    }
    return s + " ]";
}

std::vector<DamageRect> sorted(std::vector<DamageRect> rs) {
    std::sort(rs.begin(), rs.end(), [](const DamageRect& a, const DamageRect& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    return rs;
}

bool same(const std::vector<DamageRect>& a, const std::vector<DamageRect>& b) {
    const auto sa = sorted(a), sb = sorted(b);
    if (sa.size() != sb.size()) return false;
    for (size_t i = 0; i < sa.size(); ++i) {
        if (sa[i].x != sb[i].x || sa[i].y != sb[i].y || sa[i].width != sb[i].width || sa[i].height != sb[i].height)
            return false;
    }
    return true;
}

#define CHECK_RECTS(got, ...)                                                              \
    do {                                                                                   \
        const std::vector<DamageRect> want_ = __VA_ARGS__;                                 \
        if (!same(got, want_)) {                                                           \
            std::cerr << "  FAIL: rects " << str(got) << " want " << str(want_) << " (line " \
                      << __LINE__ << ")" << std::endl;                                     \
            ++gFailures;                                                                   \
        }                                                                                  \
    } while (0)

void testTileRects() {
    std::cout << "[tiles] runs per row, extended down, clipped, capped" << std::endl;
    std::vector<DamageRect> out;
    // 4 x 3 tiles over 60 x 40 px (the last column and row are partial).
    // clang-format off
    const uint32_t none[12] = {0};
    render::tileFlagsToRects(none, 4, 3, 60, 40, out);
    CHECK(out.empty());
    const uint32_t block[12] = {0, 1, 1, 0,
                                0, 1, 1, 0,
                                0, 0, 0, 0};
    render::tileFlagsToRects(block, 4, 3, 60, 40, out);
    CHECK_RECTS(out, {{16, 0, 32, 32}});
    const uint32_t ell[12] = {1, 0, 0, 1,
                              1, 1, 0, 1,
                              0, 0, 0, 1};
    render::tileFlagsToRects(ell, 4, 3, 60, 40, out);
    CHECK_RECTS(out, {{0, 0, 16, 16}, {48, 0, 12, 40}, {0, 16, 32, 16}});
    const uint32_t all[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    render::tileFlagsToRects(all, 4, 3, 60, 40, out);
    CHECK_RECTS(out, {{0, 0, 60, 40}});
    // clang-format on
    // A checkerboard is more rects than kMaxDamageRects: its bounding box.
    std::vector<uint32_t> checker(40 * 40);
    for (uint32_t y = 0; y < 40; ++y)
        for (uint32_t x = 0; x < 40; ++x) checker[y * 40 + x] = (x + y) % 2;
    render::tileFlagsToRects(checker.data(), 40, 40, 640, 640, out);
    CHECK_RECTS(out, {{0, 0, 640, 640}});
}

// A frame and its changes, as BGRA rows.
struct Pixels {
    uint32_t w, h;
    std::vector<uint8_t> px;
    Pixels(uint32_t w_, uint32_t h_, uint32_t seed) : w(w_), h(h_), px(size_t(w_) * h_ * 4) {
        std::mt19937 rng(seed);
        for (auto& b : px) b = static_cast<uint8_t>(rng());
    }
    uint8_t* at(uint32_t x, uint32_t y) { return px.data() + (size_t(y) * w + x) * 4; }
    // Flips one bit of a channel over a rect (`channel` 3 is alpha).
    void touch(uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, int channel = 1) {
        for (uint32_t j = y; j < y + rh; ++j)
            for (uint32_t i = x; i < x + rw; ++i) at(i, j)[channel] ^= 1;
    }
};

struct GpuImage {
    render::VulkanContext& ctx;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint64_t id = 0;
    VkImageView view = VK_NULL_HANDLE;
    GpuImage(render::VulkanContext& c, uint32_t w, uint32_t h) : ctx(c) {
        ctx.createImage(w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, mem, off, id);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = render::colorRange();
        vkCreateImageView(ctx.device(), &vi, nullptr, &view);
    }
    ~GpuImage() {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(ctx.device(), view, nullptr);
        if (image != VK_NULL_HANDLE) ctx.destroyImage(image, id);
    }
    // Records the upload of `p`, leaving the image in GENERAL.
    void upload(VkCommandBuffer cmd, const Pixels& p) {
        render::UploadSlice s = ctx.frames().allocUpload(p.px.size());
        std::memcpy(s.mapped, p.px.data(), p.px.size());
        render::cmdTransitionImage(cmd, image, render::colorRange(), VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = s.offset;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {p.w, p.h, 1};
        vkCmdCopyBufferToImage(cmd, s.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        render::ImageBarrier b;
        b.image = image;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        b.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        render::cmdImageBarrier(cmd, b);
    }
};

// The damage from `older` to `newer` both ways; checks they agree.
std::vector<DamageRect> diffBoth(render::VulkanContext& ctx, render::GpuFrameDiff& gpu, const Pixels& older,
                                 const Pixels& newer) {
    std::vector<DamageRect> cpu, onGpu;
    render::diffFrames(older.px.data(), newer.px.data(), older.w, older.h, size_t(older.w) * 4, cpu);
    auto& frames = ctx.frames();
    frames.beginFrame();
    GpuImage a(ctx, older.w, older.h), b(ctx, newer.w, newer.h);
    VkCommandBuffer cmd = frames.beginCommands();
    a.upload(cmd, older);
    b.upload(cmd, newer);
    const bool recorded = gpu.record(cmd, b.image, b.view, a.image, a.view, VK_IMAGE_LAYOUT_GENERAL, newer.w, newer.h);
    CHECK(recorded);
    ctx.queue().wait(frames.submit(cmd));
    CHECK(gpu.result(onGpu) == recorded);
    if (!same(cpu, onGpu)) {
        std::cerr << "  FAIL: CPU " << str(cpu) << " and GPU " << str(onGpu) << " differ" << std::endl;
        ++gFailures;
    }
    return onGpu;
}

void testDiff(render::VulkanContext& ctx) {
    std::cout << "[diff] CPU and GPU, against the tiles each change touches" << std::endl;
    render::GpuFrameDiff gpu(ctx);
    CHECK(gpu.valid());
    if (!gpu.valid()) return;
    std::vector<DamageRect> out;
    CHECK(!gpu.result(out));  // nothing recorded yet

    const Pixels base(1920, 1080, 7);
    {
        Pixels same(base);
        CHECK(diffBoth(ctx, gpu, base, same).empty());
    }
    {
        // A line of text rewritten: 10 x 3 px inside one tile row.
        Pixels p(base);
        p.touch(100, 37, 10, 3);
        CHECK_RECTS(diffBoth(ctx, gpu, base, p), {{96, 32, 16, 16}});
    }
    {
        // Across tile edges, and a second change far away.
        Pixels p(base);
        p.touch(30, 14, 10, 4);
        p.touch(1900, 1070, 3, 3);
        CHECK_RECTS(diffBoth(ctx, gpu, base, p), {{16, 0, 32, 32}, {1888, 1056, 16, 24}});
    }
    {
        // A one-pixel change in the last, partial tile row (1080 = 67.5 tiles).
        Pixels p(base);
        p.touch(0, 1079, 1, 1, 0);
        CHECK_RECTS(diffBoth(ctx, gpu, base, p), {{0, 1072, 16, 8}});
    }
    {
        // Alpha alone is no change (scanout is XRGB)... on the GPU. The CPU
        // path compares whole pixels; its frames are read back opaque.
        Pixels p(base);
        p.touch(500, 500, 4, 4, 3);
        std::vector<DamageRect> onGpu;
        auto& frames = ctx.frames();
        frames.beginFrame();
        GpuImage a(ctx, base.w, base.h), b(ctx, p.w, p.h);
        VkCommandBuffer cmd = frames.beginCommands();
        a.upload(cmd, base);
        b.upload(cmd, p);
        CHECK(gpu.record(cmd, b.image, b.view, a.image, a.view, VK_IMAGE_LAYOUT_GENERAL, p.w, p.h));
        ctx.queue().wait(frames.submit(cmd));
        CHECK(gpu.result(onGpu) && onGpu.empty());
    }
    {
        // A window opening: a big block, one rect.
        Pixels p(base);
        p.touch(200, 100, 1200, 800);
        CHECK_RECTS(diffBoth(ctx, gpu, base, p), {{192, 96, 1216, 816}});
    }
    {
        // An odd size (a 1366 x 770 screen), change at the corner.
        const Pixels small(1366, 770, 11);
        Pixels p(small);
        p.touch(1365, 769, 1, 1);
        CHECK_RECTS(diffBoth(ctx, gpu, small, p), {{1360, 768, 6, 2}});
    }
    {
        // Random scattered changes: CPU and GPU agree (diffBoth checks).
        std::mt19937 rng(3);
        for (int round = 0; round < 4; ++round) {
            Pixels p(base);
            const int n = 1 + round * 7;
            for (int i = 0; i < n; ++i) p.touch(rng() % 1900, rng() % 1060, 1 + rng() % 20, 1 + rng() % 20);
            CHECK(!diffBoth(ctx, gpu, base, p).empty());
        }
    }
}

}  // namespace

int main() {
    testTileRects();

    render::VulkanContextConfig cfg;
    cfg.headless = true;
    render::VulkanContext ctx(cfg);
    if (!ctx.init()) {
        std::cerr << "FAILED: headless VulkanContext init" << std::endl;
        return 1;
    }
    std::cout << "Device: " << ctx.deviceProperties().deviceName << std::endl;
    testDiff(ctx);

    const uint32_t validationErrors = render::vulkanValidationErrorCount();
    if (validationErrors > 0) {
        std::cerr << "FAILED: " << validationErrors << " Vulkan validation error(s)" << std::endl;
        return 1;
    }
    if (gFailures > 0) {
        std::cerr << "FAILED: " << gFailures << " check(s)" << std::endl;
        return 1;
    }
    std::cout << "bro_frame_damage_test: all checks passed" << std::endl;
    return 0;
}
