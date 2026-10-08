// Live screen capture (docs/screen-capture.md): a PNG of what is actually on
// the display. Under DRM that is the last frame presented to KMS — read back
// from the scanout buffer, so client windows, the shell and its overlays
// exactly as composited — and a developer can ask for it from outside the
// process, without the shell's cooperation:
//
//   kill -USR2 <pid>                         -> $XDG_RUNTIME_DIR/bro-screen.png
//   echo /tmp/x.png > $XDG_RUNTIME_DIR/bro-capture   (empty file: the default path)
//
// Both are polled by the DRM frame loop, so the capture is taken on the thread
// that presents, between frames; the PNG is encoded off it.

#include "engine/engine.h"
#include "engine/capture_path.h"
#include "render/vulkan_presenter.h"
#include "util/log.h"

#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif

#include "broimage/encode.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace bro::engine {

namespace {

std::atomic<bool> g_captureSignalled{false};

#if !defined(_WIN32)
void onCaptureSignal(int) { g_captureSignalled.store(true, std::memory_order_relaxed); }
#endif

std::string runtimeDir() {
    const char* d = std::getenv("XDG_RUNTIME_DIR");
    if (d && *d) return d;
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec).string();
}

// Frames between looks at the request file: a stat, not worth every frame.
constexpr int kRequestPollFrames = 30;

} // namespace

std::string Engine::defaultScreenCapturePath() {
    return (std::filesystem::path(runtimeDir()) / "bro-screen.png").string();
}

std::string Engine::screenCaptureRequestPath() {
    return (std::filesystem::path(runtimeDir()) / "bro-capture").string();
}

void Engine::installScreenCaptureSignal() {
#if !defined(_WIN32)
    struct sigaction sa {};
    sa.sa_handler = onCaptureSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
#endif
}

bool Engine::captureScreen(const std::string& path, std::string* why) {
    auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (path.empty()) return fail("no path");
#if BRO_WITH_DMABUF
    if (displayMode_ == DisplayMode::Drm) {
        auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
        if (!kms) return fail("no KMS presenter");
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        std::string err;
        if (!kms->readLastFrame(rgba, w, h, &err)) return fail(err);
        if (!ensureParentDir(path)) return fail("cannot create the directory of " + path);
        if (!broimage::encode_png_file(path, rgba.data(), static_cast<int>(w), static_cast<int>(h), 4))
            return fail("writing " + path + " failed");
        return true;
    }
#endif
    if (!screenshot(path)) return fail("the engine composite could not be captured to " + path);
    return true;
}

void Engine::pollScreenCaptureTriggers() {
    static int s_frame = 0;
    std::string target;
    if (g_captureSignalled.exchange(false, std::memory_order_relaxed)) target = defaultScreenCapturePath();
    if (target.empty() && ++s_frame % kRequestPollFrames == 0) {
        const std::string req = screenCaptureRequestPath();
        std::error_code ec;
        if (std::filesystem::exists(req, ec)) {
            std::ifstream in(req);
            std::getline(in, target);
            while (!target.empty() && (target.back() == '\r' || target.back() == ' ')) target.pop_back();
            std::filesystem::remove(req, ec);
            if (target.empty()) target = defaultScreenCapturePath();
        }
    }
    if (target.empty()) return;

#if BRO_WITH_DMABUF
    // The scanout readback happens here, between frames; the encode does not
    // hold the next frame up.
    if (displayMode_ == DisplayMode::Drm) {
        auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        std::string err = "no KMS presenter";
        if (!kms || !kms->readLastFrame(rgba, w, h, &err)) {
            LOG_WARN("Screen capture to '%s' failed: %s", target.c_str(), err.c_str());
            return;
        }
        std::thread([target, rgba = std::move(rgba), w, h]() {
            if (ensureParentDir(target) &&
                broimage::encode_png_file(target, rgba.data(), static_cast<int>(w), static_cast<int>(h), 4))
                LOG_INFO("Screen captured to '%s' (%ux%u)", target.c_str(), w, h);
            else
                LOG_WARN("Screen capture: writing '%s' failed", target.c_str());
        }).detach();
        return;
    }
#endif
    std::string err;
    if (captureScreen(target, &err)) LOG_INFO("Screen captured to '%s'", target.c_str());
    else LOG_WARN("Screen capture to '%s' failed: %s", target.c_str(), err.c_str());
}

} // namespace bro::engine
