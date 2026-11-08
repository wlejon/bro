// bro_vulkan_test: the shared GPU frame/submission core (VulkanQueue,
// VulkanFrames), the presenter's offscreen composite + readback, Skia's
// Ganesh-Vulkan context on the shared device (SkiaGpu), the render module's
// brokernel stages, the persisted pipeline cache, and — where a display is
// available — the swapchain following its window. Run with
// BRO_VK_VALIDATION=1 to also fail on any validation error.

#include "render/kernel_shader.h"
#include "render/skia_gpu.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "render/vulkan_presenter.h"
#include "render/vulkan_swapchain.h"
#include "render/vulkan_util.h"
#include "platform/window.h"
#include "platform/window_system.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace bro;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "  FAIL: " #cond " (line " << __LINE__ << ")" << std::endl;  \
            ++gFailures;                                                              \
        }                                                                             \
    } while (0)

bool near(int a, int b, int tol = 2) { return std::abs(a - b) <= tol; }

const uint8_t* px(const std::vector<uint8_t>& p, uint32_t w, uint32_t x, uint32_t y) {
    return p.data() + (static_cast<size_t>(y) * w + x) * 4;
}

void testQueueAndFrames(render::VulkanContext& ctx) {
    std::cout << "[queue/frames] tickets, immediate submits, arenas, deferred destruction" << std::endl;
    auto& queue = ctx.queue();
    auto& frames = ctx.frames();

    // An immediate submit waits for its own ticket: the fill is visible after.
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint64_t id = 0;
    void* mapped = nullptr;
    CHECK(ctx.createBuffer(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           buf, mem, off, id, mapped));
    const uint64_t before = queue.lastSubmittedTicket();
    CHECK(queue.submitImmediate([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, buf, 0, 256, 0xA5A5A5A5u); }));
    CHECK(queue.lastSubmittedTicket() == before + 1);
    CHECK(queue.completedTicket() >= before + 1);
    CHECK(mapped && static_cast<uint8_t*>(mapped)[255] == 0xA5);

    // Frame ring: upload slices are aligned, survive the frame, and the arena
    // grows past one chunk.
    frames.beginFrame();
    const uint64_t serial = frames.frameSerial();
    render::UploadSlice a = frames.allocUpload(100, 256);
    render::UploadSlice b = frames.allocUpload(12ull * 1024 * 1024);  // bigger than a chunk
    CHECK(a && b && a.offset % 256 == 0);
    std::memset(b.mapped, 0x5A, b.size);
    CHECK(static_cast<uint8_t*>(b.mapped)[b.size - 1] == 0x5A);

    // Descriptor arena: more sets than one pool holds.
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &binding};
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(ctx.device(), &li, nullptr, &layout);
    bool allSets = true;
    for (int i = 0; i < 600; ++i) allSets = allSets && frames.allocDescriptorSet(layout) != VK_NULL_HANDLE;
    CHECK(allSets);

    // A frame command buffer, and something it uses destroyed through defer().
    VkCommandBuffer cmd = frames.beginCommands();
    vkCmdFillBuffer(cmd, buf, 0, 256, 0x11111111u);
    const uint64_t t = frames.submit(cmd);
    CHECK(t > 0);
    int destroyed = 0;
    frames.defer([&] { ++destroyed; ctx.destroyBuffer(buf, id); });
    frames.beginFrame();  // frame `serial` closes; its slot is not reused yet
    CHECK(destroyed == 0);
    // (A frame that takes nothing of its slot stays open while the next slot
    // is on the GPU; this one takes an upload, so it closes.)
    CHECK(frames.allocUpload(16));
    frames.beginFrame();  // the slot comes round: its submissions are complete
    CHECK(destroyed == 1);
    CHECK(queue.isComplete(t));
    CHECK(frames.frameSerial() == serial + 2);
    vkDestroyDescriptorSetLayout(ctx.device(), layout, nullptr);
}

// Submissions from several threads at once: every ticket is handed out once,
// in order per thread; a ticket can be waited on (from any thread, or by a
// later batch) the moment submit() returns, before the queue's thread has
// submitted it; work submitted after a ticket runs after it; and a batch that
// waits on another queue's ticket is held until that queue submits it.
void testQueueThreads(render::VulkanContext& ctx) {
    std::cout << "[queue] multi-thread submission order and ticket waits" << std::endl;
    auto& queue = ctx.queue();
    VkDevice dev = ctx.device();
    const uint32_t family = static_cast<uint32_t>(ctx.queueFamilies().graphicsFamily);

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint64_t id = 0;
    void* mapped = nullptr;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 150;
    CHECK(ctx.createBuffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           buf, mem, off, id, mapped));
    if (!mapped) return;
    std::memset(mapped, 0, 4096);

    std::vector<std::vector<uint64_t>> tickets(kThreads);
    std::vector<int> badWaits(kThreads, 0);
    std::vector<std::thread> threads;
    const uint64_t first = queue.lastSubmittedTicket();
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
            VkCommandPool pool = VK_NULL_HANDLE;
            vkCreateCommandPool(dev, &poolInfo, nullptr, &pool);
            std::vector<VkCommandBuffer> cmds(kPerThread);
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, static_cast<uint32_t>(kPerThread)};
            vkAllocateCommandBuffers(dev, &ai, cmds.data());
            for (int i = 0; i < kPerThread; ++i) {
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmds[i], &begin);
                // Each thread counts in its own word: a later fill overwrites
                // an earlier one only if it runs after it.
                const uint32_t value = static_cast<uint32_t>(i + 1);
                vkCmdFillBuffer(cmds[i], buf, static_cast<VkDeviceSize>(t) * 256, 4, value);
                vkEndCommandBuffer(cmds[i]);
                render::QueueSubmit batch;
                batch.commandBuffers.push_back(cmds[i]);
                // Every other batch also waits on this thread's previous ticket.
                if (i % 2 == 1)
                    batch.waits.push_back({queue.timeline(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, tickets[t].back()});
                tickets[t].push_back(queue.submit(batch));
                // Waited on at once, every tenth: before the queue's thread
                // may have submitted it.
                if (i % 10 == 0 && !queue.wait(tickets[t].back())) ++badWaits[t];
            }
            if (!queue.wait(tickets[t].back())) ++badWaits[t];
            vkDestroyCommandPool(dev, pool, nullptr);
        });
    }
    for (auto& th : threads) th.join();

    std::vector<uint64_t> all;
    for (int t = 0; t < kThreads; ++t) {
        CHECK(badWaits[t] == 0);
        CHECK(static_cast<int>(tickets[t].size()) == kPerThread);
        bool increasing = true;
        for (size_t i = 1; i < tickets[t].size(); ++i) increasing = increasing && tickets[t][i] > tickets[t][i - 1];
        CHECK(increasing);
        all.insert(all.end(), tickets[t].begin(), tickets[t].end());
        // The last fill of each thread is what its word holds.
        CHECK(static_cast<uint32_t*>(mapped)[t * 64] == static_cast<uint32_t>(kPerThread));
    }
    std::sort(all.begin(), all.end());
    bool contiguous = all.size() == static_cast<size_t>(kThreads * kPerThread) && all.front() == first + 1;
    for (size_t i = 1; contiguous && i < all.size(); ++i) contiguous = all[i] == all[i - 1] + 1;
    CHECK(contiguous);
    CHECK(queue.completedTicket() >= all.back());

    // Across queues: a graphics batch waiting on an upload-queue ticket the
    // moment it is handed out completes only after it.
    if (render::VulkanQueue* upload = ctx.uploadQueue()) {
        for (int i = 0; i < 50; ++i) {
            const uint64_t upTicket = upload->submit(render::QueueSubmit{});
            CHECK(upTicket > 0);
            const uint64_t gTicket = queue.submit(
                render::QueueSubmit{{}, {{upload->timeline(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, upTicket}}, {}});
            CHECK(gTicket > 0 && queue.wait(gTicket));
            CHECK(upload->isComplete(upTicket));
        }
    } else {
        std::cout << "  (no upload queue: the cross-queue case is skipped)" << std::endl;
    }
    ctx.destroyBuffer(buf, id);
}

// An RGBA8 image cleared to `color` and left in SHADER_READ_ONLY_OPTIMAL.
struct TestImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint64_t id = 0;
};
TestImage makeClearedImage(render::VulkanContext& ctx, uint32_t w, uint32_t h, VkClearColorValue color) {
    TestImage t;
    ctx.createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.image, t.mem, t.off, t.id);
    ctx.queue().submitImmediate([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange range = render::colorRange();
        render::cmdTransitionImage(cmd, t.image, range, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        render::cmdTransitionImage(cmd, t.image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    return t;
}

void testPresenterOffscreen(render::VulkanContext& ctx) {
    std::cout << "[presenter] offscreen composite + readback" << std::endl;
    render::VulkanPresenter presenter(ctx);
    CHECK(presenter.init());
    std::vector<uint8_t> out;
    uint32_t w = 0, h = 0;
    CHECK(!presenter.readbackPixels(out, w, h));  // nothing presented yet

    // CPU pixels only, N32 (BGRA): red with a green top-left quarter.
    ctx.frames().beginFrame();
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
    surface->getCanvas()->clear(SK_ColorRED);
    SkPaint green;
    green.setColor(SK_ColorGREEN);
    surface->getCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 32, 32), green);
    CHECK(presenter.presentSurface(surface.get()));
    CHECK(presenter.readbackPixels(out, w, h));
    CHECK(w == 64 && h == 64);
    if (w == 64 && h == 64) {
        const uint8_t* tl = px(out, w, 0, 0);
        const uint8_t* br = px(out, w, 63, 63);
        CHECK(tl[0] == 0 && tl[1] == 255 && tl[2] == 0 && tl[3] == 255);
        CHECK(br[0] == 255 && br[1] == 0 && br[2] == 0 && br[3] == 255);
    }

    // A GPU image (blue, 48x48) under a larger UI layer above (64x64): a
    // half-transparent red premultiplied pixel blends; the rest of the layer
    // is transparent; outside the image is the clear color.
    TestImage blue = makeClearedImage(ctx, 48, 48, {{0.0f, 0.0f, 1.0f, 1.0f}});
    auto above = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
    above->getCanvas()->clear(SK_ColorTRANSPARENT);
    SkPaint halfRed;
    halfRed.setColor(SkColorSetARGB(128, 255, 0, 0));
    halfRed.setBlendMode(SkBlendMode::kSrc);
    above->getCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 8, 8), halfRed);

    // Present twice in one frame: the second must not reuse (and overwrite)
    // the first's in-flight layer texture.
    ctx.frames().beginFrame();
    render::PresentFrame frame;
    frame.images.push_back(
        render::PresentImage::at1to1(blue.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 48, 48));
    frame.images[0].above = render::VulkanPresenter::surfaceLayer(above.get());
    frame.clearColor[0] = 0.0f; frame.clearColor[1] = 1.0f; frame.clearColor[2] = 0.0f; frame.clearColor[3] = 1.0f;
    CHECK(presenter.present(frame));
    CHECK(presenter.present(frame));
    CHECK(presenter.readbackPixels(out, w, h));
    CHECK(w == 64 && h == 64);
    if (w == 64 && h == 64) {
        const uint8_t* blended = px(out, w, 2, 2);  // 50% red over blue
        CHECK(near(blended[0], 128) && blended[1] == 0 && near(blended[2], 127) && blended[3] == 255);
        const uint8_t* image = px(out, w, 40, 40);  // blue image, transparent layer above
        CHECK(image[0] == 0 && image[1] == 0 && image[2] == 255);
        const uint8_t* clear = px(out, w, 60, 60);  // beyond the image: clear color
        CHECK(clear[0] == 0 && clear[1] == 255 && clear[2] == 0);
    }

    // All three layers: a half-transparent (premultiplied) image blends over
    // the opaque white layer below it, and the layer above over both.
    TestImage halfBlue = makeClearedImage(ctx, 48, 48, {{0.0f, 0.0f, 0.5f, 0.5f}});
    auto below = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
    below->getCanvas()->clear(SK_ColorWHITE);
    ctx.frames().beginFrame();
    frame.images[0].image = halfBlue.image;
    frame.below = render::VulkanPresenter::surfaceLayer(below.get());
    CHECK(presenter.present(frame));
    CHECK(presenter.readbackPixels(out, w, h));
    if (w == 64 && h == 64) {
        const uint8_t* mid = px(out, w, 40, 40);  // half blue over white
        CHECK(near(mid[0], 128) && near(mid[1], 128) && mid[2] == 255 && mid[3] == 255);
        const uint8_t* top = px(out, w, 2, 2);    // half red over that
        CHECK(near(top[0], 191) && near(top[1], 64) && near(top[2], 128));
        const uint8_t* outside = px(out, w, 60, 60);  // only the layer below
        CHECK(outside[0] == 255 && outside[1] == 255 && outside[2] == 255);
    }

    // Images placed anywhere, scaled and clipped, several in one frame: the
    // blue image squeezed into a 16x16 square at (8,8) and cut to its left
    // half, then the half-blue one at (40,40), past the frame's edge.
    ctx.frames().beginFrame();
    render::PresentFrame placed;
    placed.below = render::VulkanPresenter::surfaceLayer(below.get());
    render::PresentImage& squeezed = placed.images.emplace_back(
        render::PresentImage::at1to1(blue.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 48, 48));
    squeezed.dstX = squeezed.dstY = 8.0f;
    squeezed.dstW = squeezed.dstH = 16.0f;
    squeezed.clipped = true;
    squeezed.clip = {{8, 8}, {8, 16}};
    render::PresentImage& offset = placed.images.emplace_back(
        render::PresentImage::at1to1(halfBlue.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 48, 48));
    offset.dstX = offset.dstY = 40.0f;
    CHECK(presenter.present(placed));
    CHECK(presenter.readbackPixels(out, w, h));
    CHECK(w == 64 && h == 64);
    if (w == 64 && h == 64) {
        const uint8_t* in = px(out, w, 10, 10);  // the squeezed image, inside its clip
        CHECK(in[0] == 0 && in[1] == 0 && in[2] == 255);
        const uint8_t* cut = px(out, w, 20, 10);  // inside its rectangle, outside the clip
        CHECK(cut[0] == 255 && cut[1] == 255 && cut[2] == 255);
        const uint8_t* corner = px(out, w, 4, 4);  // beside it
        CHECK(corner[0] == 255 && corner[1] == 255 && corner[2] == 255);
        const uint8_t* half = px(out, w, 50, 50);  // the second image, half blue over white
        CHECK(near(half[0], 128) && near(half[1], 128) && half[2] == 255);
        const uint8_t* before = px(out, w, 36, 50);  // left of it
        CHECK(before[0] == 255 && before[1] == 255 && before[2] == 255);
    }
    ctx.queue().waitIdle();
    ctx.destroyImage(halfBlue.image, halfBlue.id);
    ctx.queue().waitIdle();
    ctx.destroyImage(blue.image, blue.id);
}

// The render module's brokernel stages compile, once each.
void testKernelShaders() {
    std::cout << "[kernel] the render stages' brokernel -> SPIR-V" << std::endl;
    const std::pair<render::RenderKernelSource, const char*> stages[] = {
        {render::RenderKernelSource::Present, "present_quad_vert"},
        {render::RenderKernelSource::Present, "present_overlay_frag"},
        {render::RenderKernelSource::FrameDiff, "frame_diff_comp"},
        {render::RenderKernelSource::KernelCanvas, "blit_rgba8"},
        {render::RenderKernelSource::KernelCanvas, "blit_rgba32f"},
    };
    for (const auto& [source, entry] : stages) {
        std::string err;
        const render::KernelShader shader = render::renderKernelShader(source, entry, &err);
        if (!shader) std::cerr << entry << ": " << err << std::endl;
        CHECK(shader.spirv.size() > 5 && shader.spirv[0] == 0x07230203u);
        CHECK(!shader.entry.empty());
        // Kept: the same stage answers the same words.
        CHECK(render::renderKernelShader(source, entry).spirv == shader.spirv);
    }
    std::string err;
    CHECK(!render::renderKernelShader(render::RenderKernelSource::Present, "no_such_entry", &err));
    CHECK(!err.empty());
}

// A real window: the swapchain follows its size, minimizing presents nothing,
// and the vsync preference switches the present mode.
void testSwapchain() {
    platform::WindowConfig wcfg;
    wcfg.title = "bro_vulkan_test";
    wcfg.width = 160;
    wcfg.height = 120;
    wcfg.fitToWorkArea = true;
    std::unique_ptr<platform::Window> windowPtr;
    try {
        windowPtr = platform::createWindow(wcfg);
    } catch (const std::exception& e) {
        std::cout << "[swapchain] SKIPPED (no window: " << e.what() << ")" << std::endl;
        return;
    }
    platform::Window& window = *windowPtr;
    const std::string driver = platform::windowSystem().driverName();
    if (driver.empty() || driver == "dummy" || driver == "offscreen") {
        std::cout << "[swapchain] SKIPPED (no display)" << std::endl;
        return;
    }
    std::cout << "[swapchain] presentation on a " << driver << " window" << std::endl;
    render::VulkanContextConfig cfg;
    render::VulkanContext ctx(cfg);
    CHECK(ctx.init(&window));
    if (!ctx.isValid()) return;
    render::VulkanSwapchain swapchain(ctx, &window, /*vsync=*/true);
    CHECK(swapchain.init());
    render::VulkanPresenter presenter(ctx, swapchain);
    CHECK(presenter.init());

    auto presentFrame = [&](uint32_t w, uint32_t h) {
        ctx.frames().beginFrame();
        std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0xFF336699u);
        return presenter.presentPixels(pixels.data(), w, h);
    };
    for (int i = 0; i < 4; ++i) CHECK(presentFrame(160, 120));
    CHECK(swapchain.presentMode() == VK_PRESENT_MODE_FIFO_KHR ||
          swapchain.presentMode() == VK_PRESENT_MODE_FIFO_RELAXED_KHR);

    // Resize: the swapchain follows the window, whatever size the frame is.
    window.setWindowSize(220, 170);
    window.sync();
    for (int i = 0; i < 4; ++i) {
        platform::pumpEvents();
        CHECK(presentFrame(160, 120));
    }
    int pw = 0, ph = 0;
    window.getSizeInPixels(pw, ph);
    CHECK(swapchain.extent().width == static_cast<uint32_t>(pw));
    CHECK(swapchain.extent().height == static_cast<uint32_t>(ph));

    swapchain.setVSync(false);
    CHECK(presentFrame(160, 120));
    std::cout << "  vsync off -> present mode " << swapchain.presentMode() << std::endl;

    // Hidden: nothing to present, and nothing fails.
    window.hide();
    window.sync();
    CHECK(presentFrame(160, 120));
    window.show();
    window.sync();
    for (int i = 0; i < 3; ++i) CHECK(presentFrame(160, 120));
    ctx.queue().waitIdle();
}

void testSkiaGpu(render::VulkanContext& ctx) {
    std::cout << "[skia gpu] Ganesh on the shared device, images sampled in place" << std::endl;
    render::SkiaGpu gpu(ctx);
    CHECK(gpu.init());
    if (!gpu.context()) return;
    render::VulkanQueue& queue = ctx.queue();

    // Skia's own submission goes through VulkanQueue and takes a ticket.
    const uint64_t before = queue.lastSubmittedTicket();
    render::LayerSurface surf = gpu.makeSurface(64, 64);
    CHECK(surf.isGpu() && surf.image->view != VK_NULL_HANDLE);
    if (!surf.isGpu()) return;
    CHECK(queue.lastSubmittedTicket() > before);
    {
        auto lock = gpu.lock();
        SkCanvas* c = surf.surface->getCanvas();
        c->clear(SK_ColorRED);
        SkPaint blue;
        blue.setColor(SK_ColorBLUE);
        c->drawRect(SkRect::MakeXYWH(32, 0, 32, 64), blue);
        SkPaint halfGreen;  // premultiplied (0, 128, 0, 128) once drawn with kSrc
        halfGreen.setColor(SkColorSetARGB(128, 0, 255, 0));
        halfGreen.setBlendMode(SkBlendMode::kSrc);
        c->drawRect(SkRect::MakeXYWH(0, 48, 16, 16), halfGreen);
    }
    const uint64_t drawn = queue.lastSubmittedTicket();
    gpu.finish(surf.surface.get());
    CHECK(queue.lastSubmittedTicket() > drawn);

    // The presenter samples the image where Skia left it: no copy, no readback.
    render::VulkanPresenter presenter(ctx);
    CHECK(presenter.init());
    ctx.frames().beginFrame();
    render::PresentFrame frame;
    render::PresentImage& img = frame.images.emplace_back(render::PresentImage::at1to1(
        surf.image->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 64, 64));
    img.view = surf.image->view;
    frame.clearColor[0] = 1.0f; frame.clearColor[1] = 1.0f; frame.clearColor[2] = 1.0f; frame.clearColor[3] = 1.0f;
    CHECK(presenter.present(frame));
    std::vector<uint8_t> out;
    uint32_t w = 0, h = 0;
    CHECK(presenter.readbackPixels(out, w, h));
    CHECK(w == 64 && h == 64);
    if (w == 64 && h == 64) {
        const uint8_t* red = px(out, w, 8, 8);
        CHECK(red[0] == 255 && red[1] == 0 && red[2] == 0 && red[3] == 255);
        const uint8_t* blue = px(out, w, 48, 8);
        CHECK(blue[0] == 0 && blue[1] == 0 && blue[2] == 255 && blue[3] == 255);
        const uint8_t* green = px(out, w, 8, 56);  // half green over the white clear
        CHECK(near(green[0], 127) && green[1] == 255 && near(green[2], 127) && green[3] == 255);
    }

    // A readback through Skia (getImageData, capture) sees the same pixels,
    // and the surface draws again after the presenter sampled it.
    {
        auto lock = gpu.lock();
        std::vector<uint8_t> pixels(64 * 64 * 4);
        SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        CHECK(surf.surface->readPixels(info, pixels.data(), 64 * 4, 0, 0));
        const uint8_t* blue = px(pixels, 64, 48, 8);
        CHECK(blue[0] == 0 && blue[1] == 0 && blue[2] == 255 && blue[3] == 255);
        const uint8_t* green = px(pixels, 64, 8, 56);
        CHECK(green[0] == 0 && near(green[1], 128) && green[2] == 0 && near(green[3], 128));
        surf.surface->getCanvas()->clear(SK_ColorGREEN);
    }
    gpu.finish(surf.surface.get());
    ctx.frames().beginFrame();
    CHECK(presenter.present(frame));
    CHECK(presenter.readbackPixels(out, w, h));
    if (w == 64 && h == 64) {
        const uint8_t* green = px(out, w, 48, 8);
        CHECK(green[0] == 0 && green[1] == 255 && green[2] == 0);
    }

    // Two threads draw on the one context, each under the lock, each into its
    // own surface (the raster thread's layers and a main-thread canvas).
    render::LayerSurface other;
    std::thread worker([&] {
        other = gpu.makeSurface(32, 32);
        for (int i = 0; i < 50 && other; ++i) {
            auto lock = gpu.lock();
            other.surface->getCanvas()->clear(SkColorSetARGB(255, 0, 0, static_cast<U8CPU>(i * 5)));
            gpu.finish(other.surface.get());
        }
    });
    for (int i = 0; i < 50; ++i) {
        auto lock = gpu.lock();
        surf.surface->getCanvas()->clear(SkColorSetARGB(255, static_cast<U8CPU>(i * 5), 0, 0));
        gpu.finish(surf.surface.get());
    }
    worker.join();
    CHECK(other.isGpu());
    if (other.isGpu()) {
        auto lock = gpu.lock();
        uint8_t a[4] = {}, b[4] = {};
        SkImageInfo one = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        CHECK(surf.surface->readPixels(one, a, 4, 10, 10));
        CHECK(other.surface->readPixels(one, b, 4, 10, 10));
        CHECK(a[0] == 245 && a[2] == 0 && b[0] == 0 && b[2] == 245);
    }

    // Dropped surfaces are destroyed once the GPU is done with them.
    surf.reset();
    other.reset();
    frame.images.clear();
    queue.waitIdle();
    gpu.collect();
}

} // namespace

int main() {
    // The pipeline cache goes to a directory of this run's own, so the first
    // context starts cold and the second must find what the first wrote.
    namespace fs = std::filesystem;
    const fs::path cacheDir = fs::temp_directory_path() / "bro_vulkan_test_pipeline_cache";
    std::error_code ec;
    fs::remove_all(cacheDir, ec);
#ifdef _WIN32
    _putenv_s("BRO_PIPELINE_CACHE_DIR", cacheDir.string().c_str());
#else
    setenv("BRO_PIPELINE_CACHE_DIR", cacheDir.string().c_str(), 1);
#endif

    testKernelShaders();
    std::string cacheFile;
    {
        render::VulkanContextConfig cfg;
        cfg.headless = true;
        render::VulkanContext ctx(cfg);
        if (!ctx.init()) {
            std::cerr << "FAILED: headless VulkanContext init" << std::endl;
            return 1;
        }
        std::cout << "Device: " << ctx.deviceProperties().deviceName << std::endl;
        CHECK(ctx.persistentPipelineCache().loadedBytes() == 0);
        cacheFile = ctx.persistentPipelineCache().path();
        testQueueAndFrames(ctx);
        testQueueThreads(ctx);
        testPresenterOffscreen(ctx);  // builds the presenter's blend pipeline
        testSkiaGpu(ctx);
    }
    if (!cacheFile.empty() && !fs::exists(cacheFile)) {
        // Not written: the driver's cache data failed the header check
        // (lavapipe 26.2 returns a truncated header).
        std::cout << "[pipeline cache] SKIPPED (the driver gave no usable cache data)" << std::endl;
    } else {
        std::cout << "[pipeline cache] written at teardown, seeded on the next init" << std::endl;
        CHECK(!cacheFile.empty());
        render::VulkanContextConfig cfg;
        cfg.headless = true;
        render::VulkanContext ctx(cfg);
        CHECK(ctx.init());
        CHECK(ctx.persistentPipelineCache().path() == cacheFile);
        CHECK(ctx.persistentPipelineCache().loadedBytes() > 0);
    }
    fs::remove_all(cacheDir, ec);
    testSwapchain();

    const uint32_t validationErrors = render::vulkanValidationErrorCount();
    if (validationErrors > 0) {
        std::cerr << "FAILED: " << validationErrors << " Vulkan validation error(s)" << std::endl;
        return 1;
    }
    if (gFailures > 0) {
        std::cerr << "FAILED: " << gFailures << " check(s)" << std::endl;
        return 1;
    }
    std::cout << "bro_vulkan_test: all checks passed" << std::endl;
    return 0;
}
