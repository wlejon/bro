#pragma once

// A remote viewer's decoded pictures as images the compositor samples: the
// <remoteview> element's layer (engine/remote_views.cpp). bro.remote.connect
// sessions (broremote's ViewerSession) decode; these two get the result onto
// bro's Vulkan device.
//
//   D3D11PictureBridge  (Windows) A Media Foundation picture is an NV12 slice
//                       of the decoder's own D3D11 texture. On that D3D11
//                       device the video processor converts it (BT.709
//                       limited range to full-range RGB) into one of a ring
//                       of shared BGRA textures, which Vulkan imports once
//                       each (VK_KHR_external_memory_win32). No CPU readback,
//                       no copy through system memory: the picture goes from
//                       the decoder to the compositor on the GPU.
//   RemotePictureUploader  A CPU picture (RGBA8, BGRA8, NV12) copied into a
//                       VkImage through the frame's upload arena: Raw, a
//                       software decoder, and the fallback when the bridge
//                       cannot work (another adapter than Vulkan's).
//
// Linux: brovideo has no VA-API decode yet. When it does, a picture is a
// dmabuf (brovideo::PictureMemory gains Dmabuf), and it goes to the
// compositor the way a client window's buffer does: VulkanDmabufImporter,
// with the decoder's sync fd as its acquire fence. Nothing else here changes.

#include "render/layer_image.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bro::render {

class VulkanContext;

/// One converted picture. While any copy of it lives its slot of the ring is
/// busy: the decode thread never converts into it. The engine keeps the one
/// it shows, and lets go of a replaced one only once the GPU frames that
/// sampled it have completed (VulkanFrames::defer).
struct BridgedPicture {
    std::shared_ptr<void> slot;  // the bridge's slot (opaque here)
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t serial = 0;         // counts conversions from 1
};

class D3D11PictureBridge {
public:
    /// Main thread. Null (with *err saying why) where it cannot work: not
    /// Windows, or the Vulkan device lacks VK_KHR_external_memory_win32.
    static std::shared_ptr<D3D11PictureBridge> create(VulkanContext& context, std::string* err);
    ~D3D11PictureBridge();
    D3D11PictureBridge(const D3D11PictureBridge&) = delete;
    D3D11PictureBridge& operator=(const D3D11PictureBridge&) = delete;

    /// Decode thread. `device`, `texture`, `subresource` are the decoder's
    /// picture (brovideo::GpuPicture: an ID3D11Device*, an NV12
    /// ID3D11Texture2D*, the array slice), (x, y, w, h) its visible
    /// rectangle. Converts it into a free slot and waits until the GPU has
    /// finished, so the slot can be sampled at once. Null (with *err) when it
    /// cannot: the decoder's adapter is not Vulkan's, no slot is free, a
    /// D3D11 call failed.
    std::shared_ptr<const BridgedPicture> convert(void* device, void* texture, uint32_t subresource, uint32_t x,
                                                  uint32_t y, uint32_t w, uint32_t h, std::string* err);

    /// Decode thread. The luma (0..255) of `n` points of the picture (xs[i],
    /// ys[i] relative to its visible origin (x0, y0)), read through a small
    /// staging copy of the NV12 texture's luma: how a latency probe reads
    /// its marker without reading the picture back. False when it cannot.
    bool readLuma(void* device, void* texture, uint32_t subresource, uint32_t x0, uint32_t y0, const uint32_t* xs,
                  const uint32_t* ys, uint32_t n, uint8_t* out);

    /// Main thread, once a frame: imports the slots the decode thread made
    /// since the last call. A slot is converted into only once it has been
    /// imported (so its layout transition never meets a picture), which
    /// costs the first frame or two of a stream (convert() fails meanwhile).
    void importPending();

    /// Main thread. The picture's Vulkan image, in SHADER_READ_ONLY_OPTIMAL.
    /// Empty on failure.
    LayerImage image(const BridgedPicture& picture);

    uint64_t conversions() const { return conversions_.load(std::memory_order_relaxed); }
    uint64_t imports() const { return imports_; }

    struct Impl;

private:
    explicit D3D11PictureBridge(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    std::atomic<uint64_t> conversions_{0};
    uint64_t imports_ = 0;
};

/// CPU pixel layouts RemotePictureUploader takes (brovideo::PixelFormat's
/// values).
enum class RemotePixels : uint8_t { RGBA8 = 0, NV12 = 1, BGRA8 = 2 };

class RemotePictureUploader {
public:
    explicit RemotePictureUploader(VulkanContext& context);
    ~RemotePictureUploader();  // its image is destroyed once the GPU is done with it
    RemotePictureUploader(const RemotePictureUploader&) = delete;
    RemotePictureUploader& operator=(const RemotePictureUploader&) = delete;

    /// Main thread. Copies a CPU picture of w x h into this uploader's image
    /// (made again, deferred, on a size change) and submits the copy now, so
    /// queue order puts it before the frame's composite. `stride` is the
    /// row pitch in bytes; `uvOffset` where NV12's interleaved UV rows start
    /// (NV12 is converted to RGBA here, BT.709 limited range). Returns the
    /// image (SHADER_READ_ONLY_OPTIMAL), or an empty one on failure.
    LayerImage upload(const uint8_t* data, uint32_t stride, uint32_t uvOffset, RemotePixels format, uint32_t w,
                      uint32_t h);
    /// The last uploaded picture's image.
    LayerImage image() const;

    uint64_t uploads() const { return uploads_; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    uint64_t uploads_ = 0;
};

}  // namespace bro::render
