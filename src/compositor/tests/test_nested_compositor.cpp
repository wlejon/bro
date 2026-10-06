// Native integration test for bro as nested Wayland compositor:
// bro runs as a Wayland compositor inside another session (or headless),
// through brocompositor. Client buffers come in through brodmabuf as Vulkan
// images, with explicit sync and fences honoured, and composite as layers
// in bro's presenter.
//
// Oracle: a test client's frames appear in bro's headless screenshot.

#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"
#include "render/vulkan_context.h"
#include "render/vulkan_dmabuf_importer.h"
#include "render/vulkan_presenter.h"

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>

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

std::string findBcWlClient() {
    std::vector<std::string> paths = {
        "/home/j/projects/brocompositor/build-release/tests/bc_wl_client",
        "/home/j/projects/brocompositor/build/tests/bc_wl_client",
        "/home/j/projects/bro/build/brocompositor/tests/bc_wl_client",
    };
    for (const auto& p : paths) {
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    return "";
}

} // namespace

int main() {
    std::cout << "=== bro_nested_compositor_test: Nested Wayland Compositor in bro ===" << std::endl;

#if !BRO_HAVE_WAYLAND_SERVER
    std::cout << "SKIP (77): needs brocompositor's Wayland server (Linux, wlroots 0.18)" << std::endl;
    return 77;
#else

    std::string clientBin = findBcWlClient();
    if (clientBin.empty()) {
        std::cout << "SKIP (77): bc_wl_client binary not found" << std::endl;
        return 77;
    }

    // 1. Initialize headless Vulkan context and presenter
    std::cout << "[step 1] Initializing VulkanContext and VulkanPresenter..." << std::endl;
    render::VulkanContextConfig vkCfg;
    vkCfg.headless = true;
    vkCfg.enableValidation = true;

    render::VulkanContext ctx(vkCfg);
    if (!ctx.init()) {
        std::cout << "SKIP (77): Headless VulkanContext::init failed" << std::endl;
        return 77;
    }

    render::VulkanPresenter presenter(ctx);
    if (!presenter.init()) {
        std::cout << "SKIP (77): VulkanPresenter::init failed" << std::endl;
        return 77;
    }
    CHECK(presenter.isHeadless());
    std::cout << "  Headless VulkanPresenter initialized" << std::endl;

    // 2. Initialize WaylandCompositor in headless mode
    std::cout << "[step 2] Initializing WaylandCompositor in headless mode..." << std::endl;
    compositor::WaylandCompositor comp;
    compositor::CompositorConfig compCfg;
    compCfg.headless = true;
    compCfg.width = 640;
    compCfg.height = 480;

    std::string compErr;
    if (!comp.init(compCfg, &compErr)) {
        std::cout << "SKIP (77): WaylandCompositor::init failed: " << compErr << std::endl;
        return 77;
    }
    CHECK(comp.isRunning());
    std::string sock = comp.socketName();
    CHECK(!sock.empty());
    std::cout << "  WaylandCompositor running on socket: " << sock << std::endl;

    // 3. Spawn real test Wayland client (bc_wl_client)
    std::cout << "[step 3] Launching test Wayland client with green buffer (#00FF00)..." << std::endl;
    int pipeOut[2];
    int pipeIn[2];
    if (::pipe(pipeOut) != 0 || ::pipe(pipeIn) != 0) {
        std::cerr << "pipe failed" << std::endl;
        return 1;
    }

    pid_t pid = ::fork();
    if (pid == 0) {
        // Child
        ::close(pipeOut[0]);
        ::dup2(pipeOut[1], STDOUT_FILENO);
        ::close(pipeOut[1]);

        ::close(pipeIn[1]);
        ::dup2(pipeIn[0], STDIN_FILENO);
        ::close(pipeIn[0]);

        ::setenv("WAYLAND_DISPLAY", sock.c_str(), 1);
        ::execl(clientBin.c_str(), clientBin.c_str(),
                "--size", "200x200",
                "--color", "ff00ff00", // opaque green (ARGB)
                "--dmabuf",
                nullptr);
        std::exit(127);
    }
    ::close(pipeOut[1]);
    ::close(pipeIn[0]);

    // Wait for client to report "ready"
    std::cout << "  Waiting for client to commit buffer and report ready..." << std::endl;
    char buf[256];
    bool clientReady = false;
    for (int attempts = 0; attempts < 100 && !clientReady; ++attempts) {
        comp.pollEvents();
        struct pollfd pfd{pipeOut[0], POLLIN, 0};
        int pr = ::poll(&pfd, 1, 50);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::read(pipeOut[0], buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                if (std::strstr(buf, "ready") != nullptr) {
                    clientReady = true;
                    break;
                }
            }
        }
    }
    CHECK(clientReady);
    std::cout << "  Client reported ready! Buffer committed to compositor" << std::endl;

    // 4. Acquire client layers from compositor
    std::cout << "[step 4] Acquiring client layers from WaylandCompositor..." << std::endl;
    std::vector<engine::UILayer> layers;
    std::vector<compositor::LeasedSurfaceFrame> leased;
    for (int waitAttempt = 0; waitAttempt < 100; ++waitAttempt) {
        comp.pollEvents();
        layers.clear();
        leased = comp.acquireClientLayers(layers);
        if (!leased.empty()) {
            break;
        }
        // Drain any output from client
        struct pollfd pfd{pipeOut[0], POLLIN, 0};
        if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            char extra[256];
            ssize_t n = ::read(pipeOut[0], extra, sizeof(extra) - 1);
            if (n > 0) {
                extra[n] = '\0';
                std::cout << "  [client out] " << extra;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(!leased.empty());
    CHECK(!layers.empty());
    std::cout << "  Acquired " << layers.size() << " client layer(s)" << std::endl;

    if (!layers.empty()) {
        const auto& layer = layers[0];
        std::cout << "  Layer geometry: x=" << layer.quad.x << ", y=" << layer.quad.y
                  << ", w=" << layer.quad.w << ", h=" << layer.quad.h << std::endl;
        CHECK(layer.quad.w == 200.0f);
        CHECK(layer.quad.h == 200.0f);

        // Verify layer content holds DmabufLayerSource
        auto* dmabufSrc = std::get_if<render::DmabufLayerSource>(&layer.content);
        CHECK(dmabufSrc != nullptr);
        if (dmabufSrc) {
            std::cout << "  DmabufLayerSource: bufferId=" << dmabufSrc->bufferId
                      << ", size=" << dmabufSrc->width << "x" << dmabufSrc->height
                      << ", format=" << dmabufSrc->drmFormat << std::endl;
            CHECK(dmabufSrc->width == 200);
            CHECK(dmabufSrc->height == 200);

            // 5. Composite client layer into VulkanPresenter offscreen frame
            std::cout << "[step 5] Compositing client layer into VulkanPresenter..." << std::endl;
            ctx.frames().beginFrame();

            render::PresentFrame pFrame;
            pFrame.width = 640;
            pFrame.height = 480;

            static uint64_t s_frameCounter = 1;
            auto* imported = presenter.dmabufImporter()->getOrImport(*dmabufSrc, s_frameCounter++);
            CHECK(imported != nullptr);
            CHECK(imported->image != VK_NULL_HANDLE);
            CHECK(imported->view != VK_NULL_HANDLE);

            render::PresentImage& pImg = pFrame.images.emplace_back();
            pImg.image = imported->image;
            pImg.view = imported->view;
            pImg.width = imported->width;
            pImg.height = imported->height;
            pImg.dstX = layer.quad.x;
            pImg.dstY = layer.quad.y;
            pImg.dstW = layer.quad.w;
            pImg.dstH = layer.quad.h;

            CHECK(presenter.present(pFrame));

            // 6. Read back screenshot pixels and verify client's green content
            std::cout << "[step 6] Reading back composited screenshot and verifying client pixels..." << std::endl;
            std::vector<uint8_t> pixels;
            uint32_t rbW = 0, rbH = 0;
            CHECK(presenter.readbackPixels(pixels, rbW, rbH));
            CHECK(rbW == 640 && rbH == 480);
            CHECK(pixels.size() == rbW * rbH * 4);

            // Check client pixels at center of the client's window quad
            int checkX = static_cast<int>(layer.quad.x + layer.quad.w / 2);
            int checkY = static_cast<int>(layer.quad.y + layer.quad.h / 2);
            size_t pixelOffset = (static_cast<size_t>(checkY) * rbW + checkX) * 4;

            uint8_t r = pixels[pixelOffset + 0];
            uint8_t g = pixels[pixelOffset + 1];
            uint8_t b = pixels[pixelOffset + 2];
            uint8_t a = pixels[pixelOffset + 3];

            std::cout << "  Pixel at (" << checkX << ", " << checkY << "): RGBA=("
                      << int(r) << ", " << int(g) << ", " << int(b) << ", " << int(a) << ")" << std::endl;

            // Green color check (R=0, G~255, B=0, A=255)
            CHECK(g > 200);
            CHECK(r < 50);
            CHECK(b < 50);
            CHECK(a > 200);

            if (g > 200 && r < 50 && b < 50) {
                std::cout << "  [ORACLE VERIFIED] Test client's green frame successfully captured in headless screenshot!" << std::endl;
            } else {
                std::cerr << "  FAIL: Expected green pixel from Wayland client buffer, got RGBA("
                          << int(r) << "," << int(g) << "," << int(b) << "," << int(a) << ")" << std::endl;
            }
        }
    }

    // 7. Cleanup and release
    std::cout << "[step 7] Cleaning up and releasing leased frames..." << std::endl;
    comp.releaseClientLayers(leased);

    // Terminate test client
    if (::write(pipeIn[1], "quit\n", 5) < 0) {}
    ::close(pipeIn[1]);
    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(pipeOut[0]);

    comp.shutdown();
    ctx.queue().waitIdle();

    if (gFailures > 0) {
        std::cerr << "FAIL: " << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "=== bro_nested_compositor_test: ALL CHECKS PASSED (100%) ===" << std::endl;
    return 0;
#endif
}
