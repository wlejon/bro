// Native test exercising zero-copy DMA-BUF Vulkan import
// (VulkanDmabufImporter) and KMS direct scanout presenter (KmsDirectPresenter).
//
// Mandate: NO MOCKS. Allocates genuine DMA-BUFs from kernel DRM render nodes
// via brodmabuf, imports them into Vulkan via VK_KHR_external_memory_fd /
// VK_EXT_image_drm_format_modifier, and evaluates KMS direct presentation geometry.

#include "render/kms_direct_presenter.h"
#include "render/layer_source.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "render/vulkan_dmabuf_importer.h"
#include "render/vulkan_presenter.h"

#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(__linux__) && BRO_WITH_DMABUF
#include <brodmabuf/allocator.h>
#include <brodmabuf/formats.h>
#include <brodmabuf/gbm.h>
#include <brodmabuf/kms.h>
#include <brodmabuf/sync.h>
#endif

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

} // namespace

int main() {
    std::cout << "=== bro_vulkan_dmabuf_test: Vulkan DMA-BUF & KMS Direct Presenter ===" << std::endl;

#if !defined(__linux__) || !BRO_WITH_DMABUF
    std::cout << "[test_dmabuf_presenter] DMA-BUF is only supported on Linux with brodmabuf; skipping (77)" << std::endl;
    return 77;
#else

    // -------------------------------------------------------------------------
    // a) Check DRM render node / DMA-BUF allocator availability
    // -------------------------------------------------------------------------
    std::cout << "[step a] Checking DRM render node and DMA-BUF allocator..." << std::endl;
    auto alloc_res = brodmabuf::DmaBufAllocator::create_default();
    if (!alloc_res.ok()) {
        std::cout << "[test_dmabuf_presenter] SKIP (77): DMA-BUF allocator unavailable: "
                  << alloc_res.error_message() << std::endl;
        return 77;
    }

    auto allocator = std::move(alloc_res.value());
    if (!allocator || !allocator->is_valid() || allocator->drm_fd() < 0) {
        std::cout << "[test_dmabuf_presenter] SKIP (77): Allocator returned invalid device" << std::endl;
        return 77;
    }
    std::cout << "  DRM device available, fd=" << allocator->drm_fd() << std::endl;

    // -------------------------------------------------------------------------
    // b) Initialize a headless render::VulkanContext and verify extensions
    // -------------------------------------------------------------------------
    std::cout << "[step b] Initializing headless VulkanContext..." << std::endl;
    render::VulkanContextConfig vkCfg;
    vkCfg.headless = true;
    vkCfg.enableValidation = true;

    render::VulkanContext ctx(vkCfg);
    if (!ctx.init()) {
        std::cout << "[test_dmabuf_presenter] SKIP (77): Headless VulkanContext::init failed" << std::endl;
        return 77;
    }

    const auto& devExts = ctx.enabledDeviceExtensions();
    auto hasDeviceExt = [&](const char* name) {
        for (const auto& e : devExts) {
            if (e == name) return true;
        }
        return false;
    };

    const bool hasExtMemFd = hasDeviceExt(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    const bool hasDmaBufExt = hasDeviceExt(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    const bool hasModifierExt = hasDeviceExt(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);

    std::cout << "  Vulkan Device: " << ctx.deviceProperties().deviceName << std::endl;
    std::cout << "  - " << VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME << ": "
              << (hasExtMemFd ? "YES" : "NO") << std::endl;
    std::cout << "  - " << VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME << ": "
              << (hasDmaBufExt ? "YES" : "NO") << std::endl;
    std::cout << "  - " << VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME << ": "
              << (hasModifierExt ? "YES" : "NO") << std::endl;

    if (!hasExtMemFd || !hasDmaBufExt || !hasModifierExt) {
        std::cout << "[test_dmabuf_presenter] SKIP (77): Vulkan device lacks required external memory extensions" << std::endl;
        return 77;
    }

    // -------------------------------------------------------------------------
    // c) Allocate a real DMA-BUF buffer using DmaBufAllocator
    // -------------------------------------------------------------------------
    std::cout << "[step c] Allocating real DMA-BUF buffer via DmaBufAllocator..." << std::endl;
    constexpr uint32_t kWidth = 256;
    constexpr uint32_t kHeight = 256;
    constexpr uint32_t kDrmFormat = DRM_FORMAT_XRGB8888;

    auto buf_res = allocator->allocate(kWidth, kHeight, kDrmFormat);
    if (!buf_res.ok()) {
        std::cout << "[test_dmabuf_presenter] SKIP (77): Failed to allocate DMA-BUF: "
                  << buf_res.error_message() << std::endl;
        return 77;
    }

    brodmabuf::DmaBufAttributes attrs = std::move(buf_res.value());
    CHECK(attrs.is_valid());
    CHECK(attrs.width == kWidth);
    CHECK(attrs.height == kHeight);
    CHECK(attrs.drm_format == kDrmFormat);
    CHECK(!attrs.planes.empty());
    CHECK(attrs.planes[0].fd.valid());
    CHECK(attrs.planes[0].stride >= kWidth * 4);

    std::cout << "  Allocated DMA-BUF: " << attrs.width << "x" << attrs.height
              << ", format=" << brodmabuf::drm_format_to_string(attrs.drm_format)
              << ", modifier=" << brodmabuf::drm_modifier_to_string(attrs.modifier)
              << ", planes=" << attrs.planes.size()
              << ", plane0_fd=" << attrs.planes[0].fd.get()
              << ", stride0=" << attrs.planes[0].stride << std::endl;

    // -------------------------------------------------------------------------
    // d) Construct render::DmabufLayerSource with the allocated buffer's attributes
    // -------------------------------------------------------------------------
    std::cout << "[step d] Constructing DmabufLayerSource..." << std::endl;
    constexpr uint64_t kBufferId = 42001;
    render::DmabufLayerSource layerSrc;
    layerSrc.bufferId = kBufferId;
    layerSrc.width = attrs.width;
    layerSrc.height = attrs.height;
    layerSrc.drmFormat = attrs.drm_format;
    layerSrc.modifier = attrs.modifier;
    layerSrc.planeCount = static_cast<uint32_t>(attrs.planes.size());
    for (size_t i = 0; i < attrs.planes.size() && i < 4; ++i) {
        layerSrc.fds[i] = attrs.planes[i].fd.get();
        layerSrc.strides[i] = attrs.planes[i].stride;
        layerSrc.offsets[i] = attrs.planes[i].offset;
    }

    CHECK(layerSrc.bufferId == kBufferId);
    CHECK(layerSrc.fds[0] >= 0);
    CHECK(layerSrc.width == kWidth);
    CHECK(layerSrc.height == kHeight);

    // -------------------------------------------------------------------------
    // e) Initialize render::VulkanDmabufImporter with the Vulkan context
    // -------------------------------------------------------------------------
    std::cout << "[step e] Initializing VulkanDmabufImporter..." << std::endl;
    render::VulkanDmabufImporter importer(ctx);
    CHECK(importer.init());
    CHECK(importer.isSupported());

    // -------------------------------------------------------------------------
    // f) Import DmabufLayerSource into sampled Vulkan image view
    // -------------------------------------------------------------------------
    std::cout << "[step f] Importing DmabufLayerSource into sampled Vulkan image view..." << std::endl;
    constexpr uint64_t kFrameSerial1 = 1;
    render::ImportedClientBuffer* imported = importer.getOrImport(layerSrc, kFrameSerial1);
    CHECK(imported != nullptr);
    if (!imported) {
        std::cerr << "FAIL: getOrImport returned null" << std::endl;
        return 1;
    }

    CHECK(imported->bufferId == kBufferId);
    CHECK(imported->image != VK_NULL_HANDLE);
    CHECK(imported->view != VK_NULL_HANDLE);
    CHECK(imported->width == kWidth);
    CHECK(imported->height == kHeight);
    CHECK(imported->lastUsedFrame == kFrameSerial1);
    CHECK(imported->vulkanImage != nullptr);
    CHECK(imported->vulkanImage->handle() == imported->image);
    CHECK(imported->vulkanImage->format() == VK_FORMAT_B8G8R8A8_UNORM);

    std::cout << "  Imported VkImage=" << imported->image
              << ", VkImageView=" << imported->view
              << ", Format=" << imported->vulkanImage->format()
              << " (" << imported->width << "x" << imported->height << ")" << std::endl;

    // -------------------------------------------------------------------------
    // g) Test buffer caching (re-importing same FD/id returns cached image view)
    // -------------------------------------------------------------------------
    std::cout << "[step g] Testing buffer caching..." << std::endl;
    constexpr uint64_t kFrameSerial2 = 2;
    render::ImportedClientBuffer* cached = importer.getOrImport(layerSrc, kFrameSerial2);
    CHECK(cached != nullptr);
    CHECK(cached == imported);
    CHECK(cached->view == imported->view);
    CHECK(cached->image == imported->image);
    CHECK(cached->lastUsedFrame == kFrameSerial2);
    std::cout << "  Buffer cache hit confirmed: returned identical ImportedClientBuffer*" << std::endl;

    // -------------------------------------------------------------------------
    // h) Test render::KmsDirectPresenter logic: canDirectScanout evaluation
    // -------------------------------------------------------------------------
    std::cout << "[step h] Testing KmsDirectPresenter canDirectScanout logic..." << std::endl;
    render::KmsDirectPresenter kms;

    render::LayerQuad matchingQuad;
    matchingQuad.x = 0.0f;
    matchingQuad.y = 0.0f;
    matchingQuad.w = static_cast<float>(kWidth);
    matchingQuad.h = static_cast<float>(kHeight);
    matchingQuad.clipW = -1.0f; // unclipped

    // Before init, inactive presenter MUST reject scanout
    CHECK(!kms.isActive());
    CHECK(!kms.canDirectScanout(layerSrc, matchingQuad, kWidth, kHeight));

    // Try initializing KMS direct presenter with card node or allocator's DRM node
    std::string cardNode = brodmabuf::find_card_node();
    int cardFd = -1;
    if (!cardNode.empty()) {
        cardFd = ::open(cardNode.c_str(), O_RDWR | O_CLOEXEC);
    }
    if (cardFd < 0 && allocator->drm_fd() >= 0) {
        cardFd = ::dup(allocator->drm_fd());
    }

    bool kmsInited = (cardFd >= 0) && kms.init(cardFd);
    if (cardFd >= 0) {
        ::close(cardFd);
    }

    if (kmsInited && kms.isActive()) {
        std::cout << "  KMS presenter active on DRM device" << std::endl;

        // 1. Exact match (0,0), w/h match crtc and src, unclipped -> ALLOWED
        CHECK(kms.canDirectScanout(layerSrc, matchingQuad, kWidth, kHeight));

        // 2. Non-zero X offset -> REJECTED
        render::LayerQuad offsetQuadX = matchingQuad;
        offsetQuadX.x = 16.0f;
        CHECK(!kms.canDirectScanout(layerSrc, offsetQuadX, kWidth, kHeight));

        // 3. Non-zero Y offset -> REJECTED
        render::LayerQuad offsetQuadY = matchingQuad;
        offsetQuadY.y = 16.0f;
        CHECK(!kms.canDirectScanout(layerSrc, offsetQuadY, kWidth, kHeight));

        // 4. Scaled quad (w != crtcWidth) -> REJECTED
        render::LayerQuad scaledQuad = matchingQuad;
        scaledQuad.w = static_cast<float>(kWidth / 2);
        CHECK(!kms.canDirectScanout(layerSrc, scaledQuad, kWidth, kHeight));

        // 5. Clipped quad -> REJECTED
        render::LayerQuad clippedQuad = matchingQuad;
        clippedQuad.clipX = 0; clippedQuad.clipY = 0;
        clippedQuad.clipW = static_cast<float>(kWidth);
        clippedQuad.clipH = static_cast<float>(kHeight);
        CHECK(clippedQuad.clipped());
        CHECK(!kms.canDirectScanout(layerSrc, clippedQuad, kWidth, kHeight));

        // 6. Layer dimensions != CRTC dimensions -> REJECTED
        CHECK(!kms.canDirectScanout(layerSrc, matchingQuad, kWidth * 2, kHeight * 2));
        std::cout << "  Geometry validation passed: direct scanout rules verified" << std::endl;
    } else {
        std::cout << "  Note: KMS presenter inactive (no connected display pipeline or render node only); verified inactive rejection" << std::endl;
    }

    // -------------------------------------------------------------------------
    // i) Release and verify clean teardown of external memory and Vulkan resources
    // -------------------------------------------------------------------------
    std::cout << "[step i] Testing release, pruning, and clean teardown..." << std::endl;

    // Test explicit buffer release
    VkImageView oldView = imported->view;
    importer.releaseBuffer(kBufferId);

    // Re-importing after release must allocate a fresh buffer
    constexpr uint64_t kFrameSerial3 = 3;
    render::ImportedClientBuffer* reimported = importer.getOrImport(layerSrc, kFrameSerial3);
    CHECK(reimported != nullptr);
    CHECK(reimported->view != VK_NULL_HANDLE);
    CHECK(reimported->view != oldView);
    std::cout << "  releaseBuffer verified: old view released, fresh view created" << std::endl;

    // Present the imported DMA-BUF through VulkanPresenter offscreen composite
    render::VulkanPresenter presenter(ctx);
    CHECK(presenter.init());
    ctx.frames().beginFrame();

    render::PresentFrame pFrame;
    render::PresentImage& pImg = pFrame.images.emplace_back(
        render::PresentImage::at1to1(reimported->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kWidth, kHeight));
    pImg.view = reimported->view;
    pFrame.width = kWidth;
    pFrame.height = kHeight;

    CHECK(presenter.present(pFrame));
    std::vector<uint8_t> readback;
    uint32_t rbW = 0, rbH = 0;
    CHECK(presenter.readbackPixels(readback, rbW, rbH));
    CHECK(rbW == kWidth && rbH == kHeight);
    std::cout << "  VulkanPresenter composite + readback of DMA-BUF image view passed ("
              << rbW << "x" << rbH << ")" << std::endl;

    // Test prune
    importer.prune(/*currentFrameSerial=*/100, /*maxAgeFrames=*/10);
    render::ImportedClientBuffer* afterPrune = importer.getOrImport(layerSrc, 101);
    CHECK(afterPrune != nullptr);
    std::cout << "  prune verified" << std::endl;

    // Clear all imported buffers
    importer.clear();
    kms.close();
    CHECK(!kms.isActive());

    ctx.queue().waitIdle();
    std::cout << "  Clean teardown of importer and KMS presenter completed" << std::endl;

    const uint32_t validationErrors = render::vulkanValidationErrorCount();
    if (validationErrors > 0) {
        std::cerr << "FAIL: " << validationErrors << " Vulkan validation error(s)" << std::endl;
        return 1;
    }

    if (gFailures > 0) {
        std::cerr << "FAIL: " << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "=== bro_vulkan_dmabuf_test: ALL TESTS PASSED (100%) ===" << std::endl;
    return 0;
#endif
}
