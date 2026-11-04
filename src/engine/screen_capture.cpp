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

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
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

std::vector<platform::screen::Display> Engine::desktopDisplays(std::string* why) {
#if BRO_WITH_DMABUF
    if (displayMode_ == DisplayMode::Drm) {
        // The display bro scans out to, by the size of its last frame.
        platform::screen::Grab g;
        std::string err;
        if (grabDesktop(0, 0, 0, 0, g, err) != platform::screen::Status::Ok) {
            if (why) *why = err;
            return {};
        }
        platform::screen::Display d;
        d.name = "scanout";
        d.width = g.width;
        d.height = g.height;
        d.primary = true;
        return {d};
    }
#endif
    return platform::screen::displays(why);
}

platform::screen::Status Engine::grabDesktop(int x, int y, int width, int height, platform::screen::Grab& out,
                                             std::string& why, bool cursor) {
#if BRO_WITH_DMABUF
    if (displayMode_ == DisplayMode::Drm) {
        auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
        if (!kms) {
            why = "no KMS presenter";
            return platform::screen::Status::Failed;
        }
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!kms->readLastFrame(rgba, w, h, &why)) return platform::screen::Status::Failed;
        if (width <= 0 || height <= 0) {
            x = 0;
            y = 0;
            width = static_cast<int>(w);
            height = static_cast<int>(h);
        }
        const int x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int x1 = std::min(x + width, static_cast<int>(w)), y1 = std::min(y + height, static_cast<int>(h));
        if (x1 <= x0 || y1 <= y0) {
            why = "the rectangle is outside the screen";
            return platform::screen::Status::Failed;
        }
        out.x = x0;
        out.y = y0;
        out.width = x1 - x0;
        out.height = y1 - y0;
        out.backend = "kms";
        if (out.width == static_cast<int>(w) && out.height == static_cast<int>(h)) {
            out.rgba = std::move(rgba);
        } else {
            out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
            for (int row = 0; row < out.height; ++row)
                std::memcpy(out.rgba.data() + static_cast<size_t>(row) * out.width * 4,
                            rgba.data() + (static_cast<size_t>(y0 + row) * w + x0) * 4,
                            static_cast<size_t>(out.width) * 4);
        }
        for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;  // scanout is XRGB
        return platform::screen::Status::Ok;
    }
#endif
    return platform::screen::grab(x, y, width, height, out, why, cursor);
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
