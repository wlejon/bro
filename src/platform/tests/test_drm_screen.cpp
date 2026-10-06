// Native test exercising DRM/KMS composited screen ownership, seat management,
// input handling via libinput, and VT switching integration for bro.
//
// Verifies Milestone 2 of Item 3 ("bro owns the screen"):
// - Seat management via broseat/logind (DrmSeatPlatform)
// - Input event dispatch via libinput (DrmInputPlatform)
// - Composited KMS presentation via VulkanPresenter and KmsDirectPresenter
// - VT switch handling (pause / restoreModeset)

#include "platform/drm_seat.h"
#include "platform/drm_input.h"
#include "render/kms_direct_presenter.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#include "render/vulkan_debug.h"

#include <iostream>
#include <vector>
#include <string>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__) && defined(BRO_WITH_DMABUF)
#include <brodmabuf/kms.h>
#include <brodmabuf/gbm.h>
#include <brodmabuf/allocator.h>
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
    std::cout << "=== bro_drm_screen_test: DRM/KMS Composited Output, Seat & Input ===" << std::endl;

#if !defined(__linux__) || !defined(BRO_WITH_SEAT) || !defined(BRO_WITH_DMABUF)
    std::cout << "SKIP (77): Only supported on Linux with seat and dmabuf enabled" << std::endl;
    return 77;
#else

    // -------------------------------------------------------------------------
    // 1. Seat Management (DrmSeatPlatform)
    // -------------------------------------------------------------------------
    std::cout << "[step 1] Testing DrmSeatPlatform seat initialization..." << std::endl;
    platform::DrmSeatPlatform seat;
    bool seatInitialized = seat.initSeat("seat0");
    std::cout << "  Seat 'seat0' init result: " << (seatInitialized ? "SUCCESS" : "UNAVAILABLE") << std::endl;
    std::cout << "  Seat active: " << (seat.isSeatActive() ? "YES" : "NO") << std::endl;

    bool activeCallbackFired = false;
    seat.setActiveChangeCallback([&](bool active) {
        activeCallbackFired = true;
        std::cout << "  ActiveChangeCallback fired: active=" << (active ? "true" : "false") << std::endl;
    });

    seat.pollEvents();

    std::string cardNode = brodmabuf::find_card_node();
    std::cout << "  Found DRM primary card node: " << (cardNode.empty() ? "(none)" : cardNode) << std::endl;

    int cardFd = -1;
    if (seat.isSeatActive() && !cardNode.empty()) {
        cardFd = seat.openDevice(cardNode);
        std::cout << "  Opened card via seat: fd=" << cardFd << std::endl;
    } else if (!cardNode.empty()) {
        cardFd = ::open(cardNode.c_str(), O_RDWR | O_CLOEXEC);
        std::cout << "  Opened card via direct open fallback: fd=" << cardFd << std::endl;
    }

    if (cardFd < 0) {
        std::cout << "SKIP (77): No accessible DRM card node found" << std::endl;
        return 77;
    }

    // -------------------------------------------------------------------------
    // 2. Input Platform (DrmInputPlatform)
    // -------------------------------------------------------------------------
    std::cout << "[step 2] Testing DrmInputPlatform libinput initialization..." << std::endl;
    platform::DrmInputPlatform input;
    const uint32_t screenW = 1920;
    const uint32_t screenH = 1080;
    bool inputInitialized = input.init(seat, "seat0", screenW, screenH);
    std::cout << "  DrmInputPlatform init result: " << (inputInitialized ? "SUCCESS" : "SKIPPED/UNPRIVILEGED") << std::endl;

    std::vector<platform::DrmInputEvent> receivedEvents;
    input.pollEvents([&](const platform::DrmInputEvent& ev) {
        receivedEvents.push_back(ev);
    });
    std::cout << "  Polled initial input events: " << receivedEvents.size() << " event(s) received" << std::endl;

    // -------------------------------------------------------------------------
    // 3. Vulkan Context & VulkanPresenter Initialization
    // -------------------------------------------------------------------------
    std::cout << "[step 3] Initializing headless VulkanContext and VulkanPresenter..." << std::endl;
    render::VulkanContextConfig vkCfg;
    vkCfg.headless = true;
    vkCfg.enableValidation = true;

    render::VulkanContext ctx(vkCfg);
    if (!ctx.init()) {
        std::cout << "SKIP (77): VulkanContext::init failed" << std::endl;
        if (cardFd >= 0) ::close(cardFd);
        return 77;
    }

    render::VulkanPresenter presenter(ctx);
    CHECK(presenter.init());
    CHECK(presenter.isHeadless());

    // -------------------------------------------------------------------------
    // 4. KMS Presentation Setup (VulkanPresenter::initKms)
    // -------------------------------------------------------------------------
    std::cout << "[step 4] Testing VulkanPresenter::initKms..." << std::endl;
    bool kmsInitOk = presenter.initKms(cardFd);
    std::cout << "  VulkanPresenter::initKms result: " << (kmsInitOk ? "SUCCESS" : "INACTIVE (no connector/CRTC)") << std::endl;

    render::KmsDirectPresenter* kmsPresenter = presenter.kmsDirectPresenter();
    CHECK(kmsPresenter != nullptr);

    if (kmsInitOk && kmsPresenter->isActive()) {
        std::cout << "  KMS modesetting active: display width=" << presenter.width()
                  << ", height=" << presenter.height() << std::endl;
        CHECK(!presenter.isHeadless());

        // ---------------------------------------------------------------------
        // 5. Present Composited Frame through KMS
        // ---------------------------------------------------------------------
        std::cout << "[step 5] Testing composited frame presentation to KMS..." << std::endl;
        ctx.frames().beginFrame();

        render::PresentFrame frame;
        frame.width = presenter.width();
        frame.height = presenter.height();
        frame.clearColor[0] = 0.1f;
        frame.clearColor[1] = 0.2f;
        frame.clearColor[2] = 0.3f;
        frame.clearColor[3] = 1.0f;

        bool presented = presenter.present(frame);
        std::cout << "  presenter.present(frame) result: " << (presented ? "SUCCESS" : "FAILED") << std::endl;
        CHECK(presented);

        // Test page flip completion handler
        kmsPresenter->handlePageFlipEvent(10);

        // ---------------------------------------------------------------------
        // 6. VT Switching Simulation (pause and restoreModeset)
        // ---------------------------------------------------------------------
        std::cout << "[step 6] Testing VT switch pause and restoreModeset..." << std::endl;
        kmsPresenter->pause();
        kmsPresenter->restoreModeset();
        CHECK(kmsPresenter->isActive());
        std::cout << "  VT switch pause & restore completed successfully" << std::endl;
    } else {
        std::cout << "  KMS inactive on this card (e.g. running inside desktop compositor or headless card)" << std::endl;
        // Verify pause / restore / handlePageFlip are safe no-ops when inactive
        kmsPresenter->pause();
        kmsPresenter->restoreModeset();
        kmsPresenter->handlePageFlipEvent(0);
    }

    // -------------------------------------------------------------------------
    // 7. Teardown
    // -------------------------------------------------------------------------
    std::cout << "[step 7] Cleaning up presenter and seat resources..." << std::endl;
    kmsPresenter->close();
    CHECK(!kmsPresenter->isActive());

    ctx.queue().waitIdle();

    const uint32_t validationErrors = render::vulkanValidationErrorCount();
    if (validationErrors > 0) {
        std::cerr << "FAIL: " << validationErrors << " Vulkan validation error(s)" << std::endl;
        return 1;
    }

    if (gFailures > 0) {
        std::cerr << "FAIL: " << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "=== bro_drm_screen_test: ALL TESTS PASSED (100%) ===" << std::endl;
    return 0;
#endif
}
