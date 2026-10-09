#include "platform/wayland/wayland_window.h"

#include "platform/window_system.h"

#include "util/log.h"
#include "util/time.h"

#include "broimage/decode.h"

// VK_KHR_wayland_surface without wayland-client.h: the create info only
// carries the two pointers.
struct wl_display;
struct wl_surface;
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace bro::platform::wl {

namespace {

constexpr uint32_t kShmXrgb8888 = 1;  // WL_SHM_FORMAT_XRGB8888

browl::CursorShape browlCursor(CursorShape s) {
    using B = browl::CursorShape;
    switch (s) {
        case CursorShape::Default:    return B::Default;
        case CursorShape::Pointer:    return B::Pointer;
        case CursorShape::Text:       return B::Text;
        case CursorShape::Move:       return B::Move;
        case CursorShape::Crosshair:  return B::Crosshair;
        case CursorShape::Wait:       return B::Wait;
        case CursorShape::Progress:   return B::Progress;
        case CursorShape::NotAllowed: return B::NotAllowed;
        case CursorShape::ResizeEW:   return B::EwResize;
        case CursorShape::ResizeNS:   return B::NsResize;
        case CursorShape::ResizeNESW: return B::NeswResize;
        case CursorShape::ResizeNWSE: return B::NwseResize;
        case CursorShape::None:       return B::Hidden;
        case CursorShape::Count_:     break;
    }
    return B::Default;
}

}  // namespace

browl::CursorShape toBrowlCursor(CursorShape s) { return browlCursor(s); }

WaylandWindow::WaylandWindow(Connection& conn, const WindowConfig& cfg)
    : conn_(conn), title_(cfg.title), width_(cfg.width), height_(cfg.height),
      resizable_(cfg.resizable), borderless_(cfg.borderless), alwaysOnTop_(cfg.alwaysOnTop),
      vsyncPref_(cfg.vsync), backend_(cfg.backend) {
    browl::WindowConfig wc;
    wc.title = cfg.title;
    wc.app_id = cfg.appId;
    wc.server_side_decorations = !cfg.borderless;
    wc.mapped = !cfg.hidden;
    window_ = conn_.display().create_window(wc);
    if (!window_) throw std::runtime_error("the Wayland compositor would not make a window");

    logicalW_ = static_cast<int>(std::max<uint32_t>(1, cfg.width));
    logicalH_ = static_cast<int>(std::max<uint32_t>(1, cfg.height));
    conn_.addWindow(this);
    if (!resizable_) applyResizable();
    // Placement (cfg.x/y, displayId, fitToWorkArea) is the compositor's on
    // Wayland: a client cannot place its windows.
    if (window_->mapped()) waitForConfigure();
    updateLogicalSize();

    LOG_INFO("Created Wayland window \"%s\" (%dx%d logical, scale %.2f) with %s", title_.c_str(),
             logicalW_, logicalH_, scale120_ / 120.0,
             backend_ == GraphicsBackend::Vulkan ? "Vulkan" : "software presentation");
}

WaylandWindow::~WaylandWindow() {
    if (relative_) {
        if (browl::Seat* seat = conn_.seat()) seat->unlock_pointer();
    }
    conn_.removeWindow(this);
    shmBuffers_[0].reset();
    shmBuffers_[1].reset();
    shmPool_.reset();
    window_.reset();
    conn_.display().flush();
}

void WaylandWindow::waitForConfigure() {
    // The first configure says the size (and that a buffer may be attached).
    const double deadline = util::currentTimeMs() + 2000.0;
    while (!window_->snapshot().configured && util::currentTimeMs() < deadline) conn_.pump(20);
    if (!window_->snapshot().configured) LOG_WARN("Wayland: no configure for window \"%s\" within 2 s", title_.c_str());
    spendLaunchToken();
}

void WaylandWindow::spendLaunchToken() {
    if (launchTokenSpent_) return;
    launchTokenSpent_ = true;
    // The first window to map spends the token this process was launched
    // with: the launcher asked for it to come up focused.
    static bool spent = false;
    if (spent) return;
    spent = true;
    const std::string& token = launchActivationToken();
    if (!token.empty()) conn_.display().activate(token, window_->wl_surface_ptr());
}

bool WaylandWindow::applyConfigure(const browl::WindowSnapshot& snap) {
    bool changed = false;
    if (snap.configured_size.width > 0 && snap.configured_size.height > 0 &&
        (snap.configured_size.width != logicalW_ || snap.configured_size.height != logicalH_)) {
        logicalW_ = snap.configured_size.width;
        logicalH_ = snap.configured_size.height;
        changed = true;
    }
    if (snap.scale120 != scale120_) {
        scale120_ = snap.scale120 ? snap.scale120 : 120;
        scaleChanged_ = true;
        changed = true;
    }
    if (changed) {
        updateLogicalSize();
        resized_ = true;
    }
    if (snap.states & browl::window_state::Activated) minimized_ = false;
    return changed;
}

bool WaylandWindow::applyScale(uint32_t scale120) {
    if (!scale120 || scale120 == scale120_) return false;
    scale120_ = scale120;
    updateLogicalSize();
    scaleChanged_ = true;
    return true;
}

void WaylandWindow::updateLogicalSize() {
    if (conn_.display().has_viewporter()) {
        // Any buffer size shows at the logical size: the drawable can follow
        // a fractional scale exactly.
        window_->set_logical_size(logicalW_, logicalH_);
    } else {
        window_->set_buffer_scale(std::max(1, static_cast<int>(scale120_ / 120)));
    }
}

// --- Presentation ---

bool WaylandWindow::createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) {
    auto create = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
    if (!create) {
        LOG_ERROR("Vulkan: vkCreateWaylandSurfaceKHR unavailable (VK_KHR_wayland_surface not enabled?)");
        return false;
    }
    VkWaylandSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = conn_.display().wl_display_ptr();
    info.surface = window_->wl_surface_ptr();
    const VkResult r = create(instance, &info, nullptr, surface);
    if (r != VK_SUCCESS) {
        LOG_ERROR("vkCreateWaylandSurfaceKHR failed: %d", static_cast<int>(r));
        return false;
    }
    return true;
}

bool WaylandWindow::presentPixels(const void* pixels, int width, int height, int stride, bool bgra) {
    if (backend_ != GraphicsBackend::Software || !pixels || width <= 0 || height <= 0) return false;
    int pw = 0, ph = 0;
    getSizeInPixels(pw, ph);
    if (pw <= 0 || ph <= 0) return false;
    const size_t frameBytes = static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4;
    if (!shmPool_ || !shmBuffers_[0] || shmBuffers_[0]->width() != pw || shmBuffers_[0]->height() != ph) {
        shmBuffers_[0].reset();
        shmBuffers_[1].reset();
        shmPool_ = conn_.display().create_shm_pool(frameBytes * 2);
        if (!shmPool_) return false;
        shmBuffers_[0] = shmPool_->allocate_buffer(pw, ph, pw * 4, kShmXrgb8888);
        shmBuffers_[1] = shmPool_->allocate_buffer(pw, ph, pw * 4, kShmXrgb8888);
        if (!shmBuffers_[0] || !shmBuffers_[1]) return false;
    }
    browl::ShmBuffer* buf = nullptr;
    for (int attempt = 0; attempt < 50 && !buf; ++attempt) {
        for (auto& b : shmBuffers_)
            if (!b->is_busy()) { buf = b.get(); break; }
        if (!buf) conn_.pump(2);
    }
    if (!buf) return false;

    // Copy, clipped to the window, as XRGB8888 (BGRA in memory).
    auto* dst = static_cast<uint8_t*>(buf->data());
    std::memset(dst, 0, frameBytes);
    const int rows = std::min(height, ph), cols = std::min(width, pw);
    for (int y = 0; y < rows; ++y) {
        const auto* s = static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * stride;
        uint8_t* d = dst + static_cast<size_t>(y) * pw * 4;
        if (bgra) {
            std::memcpy(d, s, static_cast<size_t>(cols) * 4);
        } else {
            for (int x = 0; x < cols; ++x) {
                d[x * 4 + 0] = s[x * 4 + 2];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 0];
                d[x * 4 + 3] = 0xff;
            }
        }
    }
    beforePresent(0);
    window_->attach_buffer(buf->wl_buffer_ptr());
    buf->set_busy(true);
    window_->damage(0, 0, pw, ph);
    window_->commit();
    conn_.display().flush();
    return true;
}

void WaylandWindow::beforePresent(uint64_t tag) {
    // The frame callback paces the loop (waitForFrame) when presents are
    // vsynced; an unthrottled loop (vsync off) has nothing to wait for.
    const browl::RequestId frame = vsyncPref_ ? conn_.display().request_frame_callback(window_->wl_surface_ptr()) : 0;
    const browl::RequestId id = conn_.display().request_presentation_feedback(window_->wl_surface_ptr());
    std::lock_guard<std::mutex> lock(presentMu_);
    frameRequest_ = frame;
    if (id) pendingFeedback_[id] = tag;
}

void WaylandWindow::frameDone(browl::RequestId request) {
    std::lock_guard<std::mutex> lock(presentMu_);
    if (request == frameRequest_) {
        frameRequest_ = 0;
        earlyFrame_ = false;
    }
}

bool WaylandWindow::waitForFrame(double timeoutMs) {
    const double until = util::currentTimeMs() + timeoutMs;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(presentMu_);
            if (!frameRequest_) return true;
            // Input (or a configure) while waiting: one frame now, rather
            // than at the next callback, so its answer reaches the
            // compositor in time for the compositor's next frame. One per
            // callback: input streaming in (pointer motion) still paces at
            // the compositor's rate. (The present does not block: the
            // swapchain is MAILBOX where this paces, pacesPresents.)
            if (!earlyFrame_ && conn_.frameWorthyPending()) {
                earlyFrame_ = true;
                return true;
            }
        }
        // Hidden (suspended): the compositor answers nothing until shown.
        if (conn_.lost() || isMinimized()) break;
        const double left = until - util::currentTimeMs();
        if (left <= 0.0) break;
        conn_.pump(std::max(1, static_cast<int>(std::ceil(left))));
    }
    // Not answered (a compositor that stopped drawing the window without
    // saying so): give up on this one; the present paces as it can.
    std::lock_guard<std::mutex> lock(presentMu_);
    frameRequest_ = 0;
    return false;
}

void WaylandWindow::addPresentation(const browl::PresentationFeedbackEvent& ev) {
    std::lock_guard<std::mutex> lock(presentMu_);
    auto it = pendingFeedback_.find(ev.request);
    if (it == pendingFeedback_.end()) return;
    PresentedFrame f;
    f.tag = it->second;
    f.presented = ev.presented;
    f.presentedMs = static_cast<double>(ev.time_ns) / 1e6;
    f.refreshMs = static_cast<double>(ev.refresh_ns) / 1e6;
    f.sequence = ev.sequence;
    pendingFeedback_.erase(it);
    presented_.push_back(f);
    // A consumer that never asks must not grow this without bound.
    if (presented_.size() > 512) presented_.erase(presented_.begin(), presented_.begin() + 256);
}

std::vector<PresentedFrame> WaylandWindow::takePresentedFrames() {
    std::lock_guard<std::mutex> lock(presentMu_);
    std::vector<PresentedFrame> out;
    out.swap(presented_);
    return out;
}

bool WaylandWindow::reportsPresentation() const { return conn_.display().has_presentation(); }

// --- Geometry ---

void WaylandWindow::getSize(int& w, int& h) const {
    w = logicalW_;
    h = logicalH_;
}

void WaylandWindow::getSizeInPixels(int& w, int& h) const {
    const float d = getPixelDensity();
    w = std::max(1, static_cast<int>(std::lround(logicalW_ * d)));
    h = std::max(1, static_cast<int>(std::lround(logicalH_ * d)));
}

float WaylandWindow::getPixelDensity() const {
    if (conn_.display().has_viewporter()) return static_cast<float>(scale120_) / 120.0f;
    return static_cast<float>(std::max<uint32_t>(1, scale120_ / 120));
}

float WaylandWindow::getDisplayScale() const { return static_cast<float>(scale120_) / 120.0f; }

void WaylandWindow::setWindowSize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    // The client picks its own size unless the compositor pinned it
    // (maximized, fullscreen, tiled): the next commit shows it.
    const uint32_t pinned = browl::window_state::Maximized | browl::window_state::Fullscreen |
                            browl::window_state::TiledLeft | browl::window_state::TiledRight |
                            browl::window_state::TiledTop | browl::window_state::TiledBottom;
    if (window_->snapshot().states & pinned) return;
    const int w = std::max(1, static_cast<int>(width)), h = std::max(1, static_cast<int>(height));
    if (w == logicalW_ && h == logicalH_) return;
    logicalW_ = w;
    logicalH_ = h;
    if (!resizable_) applyResizable();
    updateLogicalSize();
    resized_ = true;
}

void WaylandWindow::setPosition(int, int) {}  // the compositor places windows

void WaylandWindow::getPosition(int& x, int& y) const { x = y = 0; }

void WaylandWindow::setMinimumSize(int w, int h) {
    minW_ = std::max(0, w);
    minH_ = std::max(0, h);
    if (resizable_) window_->set_min_size(minW_, minH_);
}

void WaylandWindow::getMinimumSize(int& w, int& h) const {
    w = minW_;
    h = minH_;
}

void WaylandWindow::setMaximumSize(int w, int h) {
    maxW_ = std::max(0, w);
    maxH_ = std::max(0, h);
    if (resizable_) window_->set_max_size(maxW_, maxH_);
}

void WaylandWindow::getMaximumSize(int& w, int& h) const {
    w = maxW_;
    h = maxH_;
}

void WaylandWindow::applyResizable() {
    if (resizable_) {
        window_->set_min_size(minW_, minH_);
        window_->set_max_size(maxW_, maxH_);
    } else {
        // A fixed size is min == max (what xdg-shell has for "not resizable").
        window_->set_min_size(logicalW_, logicalH_);
        window_->set_max_size(logicalW_, logicalH_);
    }
}

// --- Style and state ---

void WaylandWindow::setTitle(const std::string& title) {
    title_ = title;
    window_->set_title(title);
}

void WaylandWindow::setOpacity(float opacity) {
    // xdg-shell has no window opacity; record the request (it reads back).
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
}

void WaylandWindow::setResizable(bool resizable) {
    resizable_ = resizable;
    applyResizable();
}

void WaylandWindow::setBorderless(bool borderless) {
    borderless_ = borderless;
    window_->set_server_side_decorations(!borderless);
}

void WaylandWindow::setFullscreen(bool fullscreen) { window_->set_fullscreen(fullscreen); }

bool WaylandWindow::isFullscreen() const {
    return (window_->snapshot().states & browl::window_state::Fullscreen) != 0;
}

void WaylandWindow::minimize() {
    window_->set_minimized();
    // Wayland never says a window was minimized; it is until it is activated.
    if (!minimized_) {
        minimized_ = true;
        minimizedNow_ = true;
    }
}

void WaylandWindow::maximize() { window_->set_maximized(true); }

void WaylandWindow::restore() {
    const uint32_t states = window_->snapshot().states;
    if (states & browl::window_state::Fullscreen) window_->set_fullscreen(false);
    else if (states & browl::window_state::Maximized) window_->set_maximized(false);
    if (minimized_) raise();
}

bool WaylandWindow::isMaximized() const {
    return (window_->snapshot().states & browl::window_state::Maximized) != 0;
}

void WaylandWindow::show() {
    if (window_->mapped()) return;
    window_->map();
    waitForConfigure();
    updateLogicalSize();
}

void WaylandWindow::hide() {
    if (!window_->mapped()) return;
    window_->unmap();
    conn_.display().flush();
}

void WaylandWindow::sync() {
    conn_.roundtrip();
    conn_.roundtrip();
}

void WaylandWindow::raise() { conn_.activate(this, conn_.takePendingToken()); }

bool WaylandWindow::flash(bool on) {
    if (on) conn_.requestAttention(this);
    return true;
}

void WaylandWindow::setIcon(const std::string& pngPath) {
    broimage::Image img;
    std::string err;
    if (!broimage::decode_file(pngPath, img, &err)) {
        LOG_INFO("Window icon: could not load '%s' (%s)", pngPath.c_str(), err.c_str());
        return;
    }
    setIconPixels(img.width, img.height, img.pixels.data());
}

bool WaylandWindow::setIconPixels(int width, int height, const uint8_t* rgba) {
    if (width <= 0 || height <= 0 || !rgba) return false;
    if (!window_->has_icon_protocol()) return false;
    if (width == height) return window_->set_icon(width, height, rgba);
    // xdg-toplevel-icon wants a square: center the image on a clear one.
    const int side = std::max(width, height);
    std::vector<uint8_t> square(static_cast<size_t>(side) * side * 4, 0);
    const int ox = (side - width) / 2, oy = (side - height) / 2;
    for (int y = 0; y < height; ++y)
        std::memcpy(&square[(static_cast<size_t>(y + oy) * side + ox) * 4],
                    rgba + static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 4);
    return window_->set_icon(side, side, square.data());
}

// --- Displays ---

uint32_t WaylandWindow::currentDisplay() const {
    const auto snap = window_->snapshot();
    return snap.outputs.empty() ? 0 : snap.outputs.front();
}

bool WaylandWindow::moveToDisplay(uint32_t displayId) {
    // A Wayland client cannot place its window; report whether it exists.
    return conn_.display().output_by_id(displayId) != nullptr;
}

NativeHandle WaylandWindow::nativeHandle() const {
    NativeHandle h;
    h.kind = NativeHandle::Kind::Wayland;
    h.waylandDisplay = conn_.display().wl_display_ptr();
    h.waylandSurface = window_->wl_surface_ptr();
    return h;
}

// --- TextInput ---

void WaylandWindow::start() {
    if (textInputActive_) return;
    textInputActive_ = true;
    browl::Seat* seat = conn_.seat();
    if (seat && seat->has_text_input() && seat->keyboard_focus() == window_->id())
        seat->enable_text_input();
}

void WaylandWindow::stop() {
    if (!textInputActive_) return;
    textInputActive_ = false;
    browl::Seat* seat = conn_.seat();
    if (seat && seat->text_input_enabled() && seat->keyboard_focus() == window_->id())
        seat->disable_text_input();
}

void WaylandWindow::setArea(int x, int y, int w, int h, int cursor) {
    (void)w;
    browl::Seat* seat = conn_.seat();
    if (!seat || !seat->text_input_enabled()) return;
    seat->set_text_input_cursor_rect(browl::Rect{x + cursor, y, 1, std::max(1, h)});
}

// --- Cursor ---

void WaylandWindow::setShape(CursorShape shape) {
    if (shape == cursorShape_) return;
    cursorShape_ = shape;
    browl::Seat* seat = conn_.seat();
    // The seat shows one cursor: this window's, while the pointer is on it
    // (the EventLoop applies a window's shape as the pointer enters it).
    if (seat && seat->pointer_focus() == window_->id() && !relative_)
        seat->set_cursor(browlCursor(shape));
}

void WaylandWindow::setRelativeMode(bool enabled) {
    if (enabled == relative_) return;
    browl::Seat* seat = conn_.seat();
    if (!seat) return;
    if (enabled) {
        if (!seat->lock_pointer(window_->wl_surface_ptr())) {
            LOG_INFO("Wayland: pointer lock unavailable (no pointer constraints / relative pointer)");
            return;
        }
        relative_ = true;
        seat->set_cursor(browl::CursorShape::Hidden);
    } else {
        seat->unlock_pointer();
        relative_ = false;
        seat->set_cursor(browlCursor(cursorShape_));
    }
}

void WaylandWindow::warp(float x, float y) {
    if (browl::Seat* seat = conn_.seat()) seat->warp_pointer(window_->wl_surface_ptr(), x, y);
}

}  // namespace bro::platform::wl
