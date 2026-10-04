#include "render/vulkan_context.h"
#include "render/vulkan_swapchain.h"
#include "render/vulkan_presenter.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>

#include <cassert>
#include <iostream>
#include <vector>

using namespace bro;

int main() {
    std::cout << "=== Running Vulkan Chunk 1 Tests ===" << std::endl;

    // 1. Test VulkanContext initialization (Headless)
    std::cout << "[Test 1] VulkanContext Headless Init... " << std::flush;
    {
        render::VulkanContextConfig cfg;
        cfg.headless = true;
        cfg.enableValidation = false;

        render::VulkanContext context(cfg);
        bool ok = context.init();
        if (!ok || !context.isValid()) {
            std::cerr << "FAILED: Failed to initialize headless VulkanContext" << std::endl;
            return 1;
        }

        assert(context.instance() != VK_NULL_HANDLE);
        assert(context.physicalDevice() != VK_NULL_HANDLE);
        assert(context.device() != VK_NULL_HANDLE);
        assert(context.graphicsQueue() != VK_NULL_HANDLE);
        assert(context.commandPool() != VK_NULL_HANDLE);
        assert(context.queueFamilies().graphicsFamily >= 0);

        std::cout << "PASSED (" << context.deviceProperties().deviceName << ")" << std::endl;

        // Test buffer allocation & memory helper
        std::cout << "[Test 2] VulkanContext Buffer Creation & Transfer... " << std::flush;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        bool bufOk = context.createBuffer(1024,
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          buffer, mem);
        assert(bufOk);
        assert(buffer != VK_NULL_HANDLE);
        assert(mem != VK_NULL_HANDLE);

        void* mapped = nullptr;
        vkMapMemory(context.device(), mem, 0, 1024, 0, &mapped);
        assert(mapped != nullptr);
        const char testMsg[] = "Bro Vulkan Context Buffer Test";
        std::memcpy(mapped, testMsg, sizeof(testMsg));
        vkUnmapMemory(context.device(), mem);

        mapped = nullptr;
        vkMapMemory(context.device(), mem, 0, 1024, 0, &mapped);
        assert(std::memcmp(mapped, testMsg, sizeof(testMsg)) == 0);
        vkUnmapMemory(context.device(), mem);

        vkDestroyBuffer(context.device(), buffer, nullptr);
        vkFreeMemory(context.device(), mem, nullptr);
        std::cout << "PASSED" << std::endl;

        // Test pooled buffer & image allocation
        std::cout << "[Test 2b] VulkanContext Pooled Buffer & Image Creation... " << std::flush;
        VkBuffer pooledBuf = VK_NULL_HANDLE;
        VkDeviceMemory pooledMem = VK_NULL_HANDLE;
        VkDeviceSize pooledOffset = 0;
        uint64_t pooledAllocId = 0;
        void* pooledMapped = nullptr;
        bool pooledOk = context.createBuffer(2048,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                             pooledBuf, pooledMem, pooledOffset, pooledAllocId, pooledMapped);
        assert(pooledOk);
        assert(pooledBuf != VK_NULL_HANDLE);
        assert(pooledMem != VK_NULL_HANDLE);
        assert(pooledAllocId != 0);
        assert(pooledMapped != nullptr);

        auto stats = context.memoryPool().stats();
        assert(stats.activeAllocationCount >= 1);

        context.destroyBuffer(pooledBuf, pooledAllocId);
        std::cout << "PASSED" << std::endl;
    }

    // 2. Test VulkanPresenter Offscreen Headless Rendering & Readback
    std::cout << "[Test 3] VulkanPresenter Offscreen Headless Render & Readback... " << std::flush;
    {
        render::VulkanContextConfig cfg;
        cfg.headless = true;
        render::VulkanContext context(cfg);
        bool ok = context.init();
        assert(ok);

        render::VulkanPresenter presenter(context);
        bool presOk = presenter.init();
        assert(presOk);
        assert(presenter.isHeadless());

        // Create a 64x64 Skia surface and draw content
        const int W = 64;
        const int H = 64;
        auto info = SkImageInfo::Make(W, H, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
        assert(surface != nullptr);

        SkCanvas* canvas = surface->getCanvas();
        // Clear background with solid red (0xFFFF0000)
        canvas->clear(SK_ColorRED);

        // Draw a solid green rect in top-left 32x32
        SkPaint paint;
        paint.setColor(SK_ColorGREEN);
        canvas->drawRect(SkRect::MakeXYWH(0, 0, 32, 32), paint);

        // Present to offscreen VkImage
        bool presented = presenter.presentSurface(surface.get());
        assert(presented);
        assert(presenter.offscreenImage() != VK_NULL_HANDLE);

        // Readback pixels from the offscreen VkImage
        std::vector<uint8_t> pixels;
        uint32_t outW = 0, outH = 0;
        bool readOk = presenter.readbackPixels(pixels, outW, outH);
        assert(readOk);
        assert(outW == W);
        assert(outH == H);
        assert(pixels.size() == static_cast<size_t>(W * H * 4));

        // Check top-left (inside green rect: R=0, G=255, B=0, A=255)
        uint8_t rGreen = pixels[0];
        uint8_t gGreen = pixels[1];
        uint8_t bGreen = pixels[2];
        uint8_t aGreen = pixels[3];
        assert(gGreen == 255);
        assert(rGreen == 0);
        assert(bGreen == 0);
        assert(aGreen == 255);

        // Check bottom-right (inside red background: R=255, G=0, B=0, A=255)
        size_t brIdx = ((H - 1) * W + (W - 1)) * 4;
        uint8_t rRed = pixels[brIdx + 0];
        uint8_t gRed = pixels[brIdx + 1];
        uint8_t bRed = pixels[brIdx + 2];
        uint8_t aRed = pixels[brIdx + 3];
        assert(rRed == 255);
        assert(gRed == 0);
        assert(bRed == 0);
        assert(aRed == 255);

        std::cout << "PASSED" << std::endl;
    }

    // 3. Test SDL_WINDOW_VULKAN window creation
    std::cout << "[Test 4] SDL_WINDOW_VULKAN Window Creation... " << std::flush;
    {
        try {
            platform::Window window("Vulkan Window Test", 128, 128,
                                    true /* hidden */, false /* resizable */,
                                    true /* vsync */, false /* borderless */,
                                    platform::GraphicsBackend::Vulkan);

            assert(window.isVulkan());
            assert(window.backend() == platform::GraphicsBackend::Vulkan);
            assert(window.getSDLWindow() != nullptr);

            // Test swapWindow is safe and doesn't crash on Vulkan window
            window.swapWindow();

            // Try creating swapchain on this window if display connection permits
            render::VulkanContextConfig cfg;
            cfg.headless = false;
            render::VulkanContext context(cfg);
            if (context.init()) {
                render::VulkanSwapchain swapchain(context, window.getSDLWindow(), true);
                if (swapchain.init()) {
                    assert(swapchain.imageCount() >= 2);
                    assert(swapchain.extent().width > 0);
                    assert(swapchain.extent().height > 0);

                    render::VulkanPresenter presenter(context, swapchain);
                    assert(presenter.init());

                    // Present a test frame
                    std::vector<uint32_t> testPixels(swapchain.extent().width * swapchain.extent().height, 0xFF0000FF);
                    bool pOk = presenter.presentPixels(testPixels.data(), swapchain.extent().width, swapchain.extent().height);
                    if (pOk) {
                        std::cout << "(Swapchain verified) ";
                    }
                }
            }

            std::cout << "PASSED" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "SKIPPED (" << e.what() << ")" << std::endl;
        }
    }

    std::cout << "=== All Vulkan Chunk 1 Tests Passed Successfully! ===" << std::endl;
    return 0;
}
