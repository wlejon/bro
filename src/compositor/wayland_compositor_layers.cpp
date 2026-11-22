// Client layer, drag-icon, and cursor acquisition for WaylandCompositor,
// including CPU compositing of wl_shm (ShmFd) client surfaces (GTK portals,
// software clients).

#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"

#include <include/core/SkImageInfo.h>
#include <include/core/SkPixmap.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#if BRO_HAVE_WAYLAND_SERVER
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace bro::compositor {

#if BRO_HAVE_WAYLAND_SERVER
namespace {

constexpr uint32_t kArgb8888 = 0x34325241, kXrgb8888 = 0x34325258;  // 'AR24', 'XR24'

bool isShmBgra(const brocompositor::SharedImage& img) {
    return img.type == brocompositor::ImageHandleType::ShmFd && !img.planes.empty() &&
           (img.drm_format == kArgb8888 || img.drm_format == kXrgb8888);
}

// Reads a wl_shm BGRA/XRGB buffer into tightly packed premultiplied BGRA (XRGB made opaque).
bool readShmImage(const brocompositor::SharedImage& img, std::vector<uint8_t>& out) {
    if (img.width == 0 || img.height == 0 || img.planes.empty()) return false;
    const auto& plane = img.planes[0];
    const size_t rowBytes = static_cast<size_t>(img.width) * 4;
    if (plane.stride < rowBytes) return false;
    const size_t len = static_cast<size_t>(plane.offset) + static_cast<size_t>(plane.stride) * img.height;
    const int fd = brocompositor::wl::fd_of(plane.handle);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < 0 || static_cast<size_t>(st.st_size) < len) return false;
    void* map = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) return false;
    out.resize(rowBytes * img.height);
    const auto* src = static_cast<const uint8_t*>(map) + plane.offset;
    for (uint32_t row = 0; row < img.height; ++row)
        std::memcpy(out.data() + row * rowBytes, src + static_cast<size_t>(row) * plane.stride, rowBytes);
    ::munmap(map, len);
    if (img.drm_format == kXrgb8888)
        for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return true;
}

engine::UILayer buildClientQuad(const brocompositor::wl::SurfaceNode& node, float baseX, float baseY,
                                uint32_t imgW, uint32_t imgH, float scaleX, float scaleY) {
    engine::UILayer layer;
    layer.quad.x = baseX + static_cast<float>(node.offset.x) * scaleX;
    layer.quad.y = baseY + static_cast<float>(node.offset.y) * scaleY;
    const bool logical = node.size.width > 0 && node.size.height > 0;
    layer.quad.w = static_cast<float>(logical ? node.size.width : static_cast<int32_t>(imgW)) * scaleX;
    layer.quad.h = static_cast<float>(logical ? node.size.height : static_cast<int32_t>(imgH)) * scaleY;
    layer.quad.clipW = -1.0f;
    return layer;
}

}  // namespace

void WaylandCompositor::appendSurfaceNode(const brocompositor::wl::SurfaceNode& node, float baseX, float baseY,
                                          std::vector<engine::UILayer>& outLayers,
                                          std::vector<LeasedSurfaceFrame>& leasedFrames,
                                          float scaleX, float scaleY) {
    if (!backend_) return;
    auto surface = backend_->surface(node.surface);
    if (!surface) return;
    auto frameOpt = surface->acquire();
    if (!frameOpt) return;
    auto imgOpt = surface->image(frameOpt->image_id);
    if (!imgOpt) {
        surface->release(*frameOpt);
        return;
    }

    if (imgOpt->type == brocompositor::ImageHandleType::DmaBuf) {
        render::DmabufLayerSource dmabufSrc;
        dmabufSrc.bufferId = imgOpt->id;
        dmabufSrc.width = imgOpt->width;
        dmabufSrc.height = imgOpt->height;
        dmabufSrc.drmFormat = imgOpt->drm_format;
        dmabufSrc.modifier = imgOpt->drm_modifier;
        dmabufSrc.planeCount = static_cast<uint32_t>(imgOpt->planes.size());
        for (size_t p = 0; p < imgOpt->planes.size() && p < 4; ++p) {
            dmabufSrc.fds[p] = brocompositor::wl::fd_of(imgOpt->planes[p].handle);
            dmabufSrc.strides[p] = imgOpt->planes[p].stride;
            dmabufSrc.offsets[p] = imgOpt->planes[p].offset;
        }
        dmabufSrc.syncFd = brocompositor::wl::fd_of(frameOpt->sync_fd);

        engine::UILayer layer = buildClientQuad(node, baseX, baseY, imgOpt->width, imgOpt->height, scaleX, scaleY);
        layer.content = dmabufSrc;
        outLayers.push_back(std::move(layer));
    } else if (isShmBgra(*imgOpt)) {
        auto& entry = shmSurfaces_[static_cast<uint32_t>(node.surface)];
        if (entry.imageId != frameOpt->image_id || entry.sequence != frameOpt->sequence || !entry.image) {
            std::vector<uint8_t> pixels;
            if (!readShmImage(*imgOpt, pixels)) {
                surface->release(*frameOpt);
                return;
            }
            const SkImageInfo info = SkImageInfo::Make(static_cast<int>(imgOpt->width),
                                                       static_cast<int>(imgOpt->height),
                                                       kBGRA_8888_SkColorType, kPremul_SkAlphaType);
            sk_sp<SkImage> skImg = SkImages::RasterFromPixmapCopy(
                SkPixmap(info, pixels.data(), static_cast<size_t>(imgOpt->width) * 4));
            if (!skImg) {
                surface->release(*frameOpt);
                return;
            }
            entry.imageId = frameOpt->image_id;
            entry.sequence = frameOpt->sequence;
            entry.image = std::move(skImg);
        }

        engine::UILayer layer = buildClientQuad(node, baseX, baseY, imgOpt->width, imgOpt->height, scaleX, scaleY);
        const uint64_t key = (static_cast<uint64_t>(node.surface) << 40) ^ frameOpt->sequence ^
                             (frameOpt->image_id << 16) ^ 0x53484d31ull;
        layer.content = engine::ShmClientLayer{entry.image, key};
        outLayers.push_back(std::move(layer));
    } else {
        surface->release(*frameOpt);
        return;
    }

    LeasedSurfaceFrame leased;
    leased.surfaceId = static_cast<uint32_t>(node.surface);
    leased.surface = surface;
    leased.frame = *frameOpt;
    leasedFrames.push_back(std::move(leased));
}

void WaylandCompositor::appendLayerShell(brocompositor::wl::Layer which, std::vector<engine::UILayer>& out,
                                         std::vector<LeasedSurfaceFrame>& leased) {
    if (!backend_) return;
    for (const auto& ls : backend_->layer_surfaces()) {
        if (!ls.mapped || ls.layer != which) continue;
        for (const auto& node : backend_->layer_surface_tree(ls.id))
            appendSurfaceNode(node, static_cast<float>(ls.rect.x), static_cast<float>(ls.rect.y), out, leased);
    }
}
#endif

std::vector<LeasedSurfaceFrame> WaylandCompositor::acquireClientLayers(std::vector<engine::UILayer>& outLayers) {
#if BRO_HAVE_WAYLAND_SERVER
    std::vector<render::ClientWindowRef> refs;
    for (const auto& w : stack()) refs.push_back(render::ClientWindowRef{w.id, false, 0.0f, 0.0f});
    return acquireClientLayers(refs, render::kClientLayersBelow | render::kClientLayersAbove, outLayers);
#else
    (void)outLayers;
    return {};
#endif
}

std::vector<LeasedSurfaceFrame> WaylandCompositor::acquireClientLayers(std::span<const render::ClientWindowRef> windows,
                                                                       uint32_t parts,
                                                                       std::vector<engine::UILayer>& outLayers) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return {};
    auto& b = *backend_;
    std::vector<LeasedSurfaceFrame> leased;
    using Layer = brocompositor::wl::Layer;

    // Locked: the lock surfaces alone, above everything.
    if (b.session_lock_state() != brocompositor::wl::LockState::Unlocked) {
        if (!(parts & render::kClientLayersAbove)) return leased;
        for (const auto& mon : b.monitors())
            for (const auto& node : b.lock_surface_tree(mon.id))
                appendSurfaceNode(node, static_cast<float>(mon.bounds.x), static_cast<float>(mon.bounds.y),
                                  outLayers, leased);
        return leased;
    }

    if (parts & render::kClientLayersBelow) {
        appendLayerShell(Layer::Background, outLayers, leased);
        appendLayerShell(Layer::Bottom, outLayers, leased);
    }
    for (const auto& ref : windows) {
        if (!b.visible(ref.windowId)) continue;
        float wx = ref.x, wy = ref.y;
        float sx = 1.0f, sy = 1.0f;
        if (!ref.pinned) {
            auto snap = b.query(ref.windowId);
            if (!snap) continue;
            wx = static_cast<float>(snap->frame.x);
            wy = static_cast<float>(snap->frame.y);
        } else if (ref.w > 0.0f && ref.h > 0.0f) {
            if (auto snap = b.query(ref.windowId); snap && snap->frame.width > 0 && snap->frame.height > 0) {
                const float fw = static_cast<float>(snap->frame.width);
                const float fh = static_cast<float>(snap->frame.height);
                if (std::abs(ref.w - fw) > 0.5f || std::abs(ref.h - fh) > 0.5f) {
                    sx = ref.w / fw;
                    sy = ref.h / fh;
                }
            }
        }
        for (const auto& node : b.window_surfaces(ref.windowId))
            appendSurfaceNode(node, wx, wy, outLayers, leased, sx, sy);
    }
    if (parts & render::kClientLayersAbove) {
        for (const auto& u : b.unmanaged_surfaces()) {
            brocompositor::wl::SurfaceNode node;
            node.surface = u.surface;
            node.size = brocompositor::Size{u.rect.width, u.rect.height};
            appendSurfaceNode(node, static_cast<float>(u.rect.x), static_cast<float>(u.rect.y), outLayers, leased);
        }
        appendLayerShell(Layer::Top, outLayers, leased);
        appendLayerShell(Layer::Overlay, outLayers, leased);
    }
    return leased;
#else
    (void)windows;
    (void)parts;
    (void)outLayers;
    return {};
#endif
}

bool WaylandCompositor::acquireDragIcon(float px, float py, std::vector<engine::UILayer>& outLayers,
                                        std::vector<LeasedSurfaceFrame>& leased, DragIconPixels& pixels) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return false;
    const auto node = backend_->drag_icon();
    if (!node) {
        dragIconSurface_ = 0;
        dragIconPixels_.clear();
        return false;
    }
    auto surface = backend_->surface(node->surface);
    if (!surface) return false;
    const float x = px + static_cast<float>(node->offset.x), y = py + static_cast<float>(node->offset.y);
    auto frame = surface->acquire();
    if (!frame) return false;
    auto img = surface->image(frame->image_id);
    if (img && img->type == brocompositor::ImageHandleType::DmaBuf) {
        surface->release(*frame);
        const size_t before = outLayers.size();
        appendSurfaceNode(brocompositor::wl::SurfaceNode{node->surface, {}, node->size, false}, x, y,
                          outLayers, leased);
        return outLayers.size() > before;
    }
    const bool shm = img && isShmBgra(*img);
    if (shm && (dragIconSurface_ != node->surface || dragIconSequence_ != frame->sequence) &&
        readShmImage(*img, dragIconPixels_)) {
        dragIconSurface_ = node->surface;
        dragIconSequence_ = frame->sequence;
        dragIconW_ = static_cast<int>(img->width);
        dragIconH_ = static_cast<int>(img->height);
    }
    surface->release(*frame);
    surface->presented_on(brocompositor::kNoMonitor, 0);
    if (!shm || dragIconSurface_ != node->surface || dragIconPixels_.empty()) return false;
    pixels.x = x;
    pixels.y = y;
    pixels.w = static_cast<float>(node->size.width > 0 ? node->size.width : dragIconW_);
    pixels.h = static_cast<float>(node->size.height > 0 ? node->size.height : dragIconH_);
    pixels.pixelW = dragIconW_;
    pixels.pixelH = dragIconH_;
    pixels.bgra = &dragIconPixels_;
    pixels.key = (static_cast<uint64_t>(node->surface) << 40) ^ dragIconSequence_;
    return true;
#else
    (void)px;
    (void)py;
    (void)outLayers;
    (void)leased;
    (void)pixels;
    return false;
#endif
}

bool WaylandCompositor::acquireClientCursor(CursorPixels& pixels) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_ || isDragActive()) return false;
    const auto c = backend_->cursor();
    if (c.hidden || c.surface == brocompositor::wl::kNoSurface) {
        cursorSurface_ = 0;
        cursorPixels_.clear();
        return false;
    }
    auto surface = backend_->surface(c.surface);
    if (!surface) return false;
    auto frame = surface->acquire();
    if (!frame) return false;
    auto img = surface->image(frame->image_id);
    const bool shm = img && isShmBgra(*img);
    if (shm && (cursorSurface_ != c.surface || cursorSequence_ != frame->sequence) &&
        readShmImage(*img, cursorPixels_)) {
        cursorSurface_ = c.surface;
        cursorSequence_ = frame->sequence;
        cursorW_ = static_cast<int>(img->width);
        cursorH_ = static_cast<int>(img->height);
    }
    surface->release(*frame);
    surface->presented_on(brocompositor::kNoMonitor, 0);
    if (!shm || cursorSurface_ != c.surface || cursorPixels_.empty()) return false;
    const auto st = surface->state();
    pixels.w = static_cast<float>(st.size.width > 0 ? st.size.width : cursorW_);
    pixels.h = static_cast<float>(st.size.height > 0 ? st.size.height : cursorH_);
    pixels.hotX = static_cast<float>(c.hotspot.x);
    pixels.hotY = static_cast<float>(c.hotspot.y);
    pixels.pixelW = cursorW_;
    pixels.pixelH = cursorH_;
    pixels.bgra = &cursorPixels_;
    pixels.key = (static_cast<uint64_t>(c.surface) << 40) ^ cursorSequence_;
    return true;
#else
    (void)pixels;
    return false;
#endif
}

}  // namespace bro::compositor
