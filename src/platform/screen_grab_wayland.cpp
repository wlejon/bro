// screen_grab.h on Linux: wlr-screencopy (zwlr_screencopy_manager_v1) over a
// Wayland connection of its own to $WAYLAND_DISPLAY, through browl: each
// output's frame into a shared-memory buffer, blocking on round trips. helm,
// sway, Hyprland and the other wlroots compositors speak it; GNOME and KDE do
// not (their screenshots go through the desktop portal, which asks the user).
// Under `bro --drm` the engine reads its own scanout instead (screen_capture.cpp).
#include "platform/screen_grab.h"

#include "browl/display.h"
#include "browl/output.h"
#include "browl/screencopy.h"
#include "browl/shm_pool.h"

#include <wayland-client.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace bro::platform::screen {

namespace {

struct Session {
    std::unique_ptr<browl::Display> display;
    std::vector<std::shared_ptr<browl::Output>> outputs;
    std::vector<Display> infos;
};

Status open(Session& s, std::string& why) {
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    if (!wd || !*wd) {
        why = "no Wayland session (WAYLAND_DISPLAY is unset) and not running under bro --drm";
        return Status::Unsupported;
    }
    std::string err;
    s.display = browl::Display::connect("", &err);
    if (!s.display) {
        why = "cannot connect to the Wayland compositor: " + err;
        return Status::Unsupported;
    }
    // The globals, then each output's geometry and modes.
    s.display->roundtrip();
    s.display->roundtrip();
    if (!s.display->has_screencopy()) {
        why = "the Wayland compositor does not offer wlr-screencopy (GNOME and KDE capture through "
              "the desktop portal only)";
        return Status::Unsupported;
    }
    s.outputs = s.display->outputs();
    int maxScale = 1;
    for (const auto& o : s.outputs) maxScale = std::max(maxScale, o->scale() > 0 ? o->scale() : 1);
    for (size_t i = 0; i < s.outputs.size(); ++i) {
        const auto& o = s.outputs[i];
        const browl::Rect l = o->logical();
        Display d;
        d.index = static_cast<int>(i);
        d.name = o->name().empty() ? o->model() : o->name();
        d.x = l.x * maxScale;
        d.y = l.y * maxScale;
        d.width = o->current_mode().width;
        d.height = o->current_mode().height;
        d.scale = o->scale() > 0 ? o->scale() : 1;
        d.primary = i == 0;
        s.infos.push_back(d);
    }
    if (s.infos.empty()) {
        why = "the Wayland compositor has no outputs";
        return Status::Unsupported;
    }
    return Status::Ok;
}

// One output's frame, RGBA top-down, `w` x `h`.
bool captureOutput(Session& s, browl::Output& out, bool cursor, std::vector<uint8_t>& rgba, int& w, int& h,
                   std::string& why) {
    auto mgr = s.display->screencopy_manager();
    if (!mgr) {
        why = "no screencopy manager";
        return false;
    }
    std::unique_ptr<browl::ScreenCopyFrame> frame = mgr->capture_output(out, /*overlay_cursor=*/cursor);
    if (!frame) {
        why = "the compositor refused the capture";
        return false;
    }
    s.display->flush();
    s.display->roundtrip();  // buffer (and buffer_done) arrive
    const browl::ScreenCopyFrameSnapshot info = frame->snapshot();
    if (frame->is_failed() || info.width == 0 || info.height == 0 || info.stride == 0) {
        why = "the compositor offered no shared-memory buffer";
        return false;
    }
    const size_t size = static_cast<size_t>(info.stride) * info.height;
    auto pool = s.display->create_shm_pool(size);
    auto buffer = pool ? pool->allocate_buffer(static_cast<int32_t>(info.width), static_cast<int32_t>(info.height),
                                               static_cast<int32_t>(info.stride), info.format)
                       : nullptr;
    if (!buffer) {
        why = "could not allocate the shared-memory buffer";
        return false;
    }
    frame->copy(buffer->wl_buffer_ptr());
    s.display->flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!frame->is_ready() && !frame->is_failed()) {
        if (std::chrono::steady_clock::now() > deadline) {
            why = "the compositor did not finish the copy";
            return false;
        }
        if (s.display->roundtrip() < 0) {
            why = "the Wayland connection failed";
            return false;
        }
    }
    if (frame->is_failed()) {
        why = "the compositor failed the copy";
        return false;
    }
    w = static_cast<int>(info.width);
    h = static_cast<int>(info.height);
    rgba.resize(static_cast<size_t>(w) * h * 4);
    // xRGB/ARGB8888 are B,G,R,x in memory; xBGR/ABGR8888 R,G,B,x.
    const bool bgr = info.format == WL_SHM_FORMAT_XRGB8888 || info.format == WL_SHM_FORMAT_ARGB8888;
    const bool rgb = info.format == 0x34324258 /*XBGR8888*/ || info.format == 0x34324241 /*ABGR8888*/;
    if (!bgr && !rgb) {
        why = "the compositor's capture format is not 8-bit RGB";
        return false;
    }
    const bool flip = (info.flags & 1u) != 0;  // ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT
    const auto* base = static_cast<const uint8_t*>(buffer->data());
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = base + static_cast<size_t>(flip ? h - 1 - y : y) * info.stride;
        uint8_t* dst = rgba.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + (bgr ? 2 : 0)];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + (bgr ? 0 : 2)];
            dst[x * 4 + 3] = 255;
        }
    }
    return true;
}

}  // namespace

std::vector<Display> displays(std::string* why) {
    Session s;
    std::string err;
    if (open(s, err) != Status::Ok) {
        if (why) *why = err;
        return {};
    }
    return s.infos;
}

Status grab(int x, int y, int width, int height, Grab& out, std::string& why, bool cursor) {
    Session s;
    if (Status st = open(s, why); st != Status::Ok) return st;
    int vx0 = s.infos[0].x, vy0 = s.infos[0].y;
    int vx1 = vx0 + s.infos[0].width, vy1 = vy0 + s.infos[0].height;
    for (const Display& d : s.infos) {
        vx0 = std::min(vx0, d.x);
        vy0 = std::min(vy0, d.y);
        vx1 = std::max(vx1, d.x + d.width);
        vy1 = std::max(vy1, d.y + d.height);
    }
    if (width <= 0 || height <= 0) {
        x = vx0;
        y = vy0;
        width = vx1 - vx0;
        height = vy1 - vy0;
    }
    const int cx0 = std::max(x, vx0), cy0 = std::max(y, vy0);
    const int cx1 = std::min(x + width, vx1), cy1 = std::min(y + height, vy1);
    if (cx1 <= cx0 || cy1 <= cy0) {
        why = "the rectangle is outside the desktop";
        return Status::Failed;
    }
    out.x = cx0;
    out.y = cy0;
    out.width = cx1 - cx0;
    out.height = cy1 - cy0;
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    bool any = false;
    std::string lastWhy;
    for (size_t i = 0; i < s.infos.size(); ++i) {
        const Display& d = s.infos[i];
        const int px0 = std::max(out.x, d.x), py0 = std::max(out.y, d.y);
        const int px1 = std::min(out.x + out.width, d.x + d.width), py1 = std::min(out.y + out.height, d.y + d.height);
        if (px1 <= px0 || py1 <= py0) continue;
        std::vector<uint8_t> img;
        int w = 0, h = 0;
        if (!captureOutput(s, *s.outputs[i], cursor, img, w, h, lastWhy)) continue;
        for (int yy = py0; yy < py1; ++yy) {
            const int sy = yy - d.y;
            if (sy >= h) break;
            const int sx0 = px0 - d.x;
            const int n = std::min(px1 - px0, w - sx0);
            if (n <= 0) continue;
            std::memcpy(out.rgba.data() + (static_cast<size_t>(yy - out.y) * out.width + (px0 - out.x)) * 4,
                        img.data() + (static_cast<size_t>(sy) * w + sx0) * 4, static_cast<size_t>(n) * 4);
        }
        any = true;
    }
    if (!any) {
        why = lastWhy.empty() ? "no output could be captured" : lastWhy;
        return Status::Failed;
    }
    out.backend = "wlr-screencopy";
    return Status::Ok;
}

}  // namespace bro::platform::screen
