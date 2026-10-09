// RemotePictureUploader: a CPU picture into a VkImage the compositor samples
// (remote_picture.h). The bridge (remote_picture_d3d11.cpp) is the GPU path.

#include "render/remote_picture.h"

#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace bro::render {

namespace {

inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// NV12 (BT.709 limited range, chroma shared by each 2x2 block) to RGBA8 rows
// of `w * 4` bytes, in 16.16 fixed point.
void nv12ToRgba(const uint8_t* data, uint32_t stride, uint32_t uvOffset, uint32_t w, uint32_t h, uint8_t* out) {
    constexpr int kY = 76309;    // 1.164383 * 65536
    constexpr int kRV = 117489;  // 1.792741
    constexpr int kGU = 13975;   // 0.213249
    constexpr int kGV = 34925;   // 0.532909
    constexpr int kBU = 138438;  // 2.112402
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* yrow = data + size_t(y) * stride;
        const uint8_t* uvrow = data + uvOffset + size_t(y / 2) * stride;
        uint8_t* o = out + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const int l = kY * (int(yrow[x]) - 16);
            const int u = int(uvrow[x & ~1u]) - 128;
            const int v = int(uvrow[(x & ~1u) + 1]) - 128;
            o[x * 4 + 0] = clamp8((l + kRV * v + 32768) >> 16);
            o[x * 4 + 1] = clamp8((l - kGU * u - kGV * v + 32768) >> 16);
            o[x * 4 + 2] = clamp8((l + kBU * u + 32768) >> 16);
            o[x * 4 + 3] = 255;
        }
    }
}

}  // namespace

struct RemotePictureUploader::Impl {
    VulkanContext* context = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t allocId = 0;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool written = false;
    std::vector<uint8_t> converted;  // NV12 converted to RGBA

    void release() {
        if (!image) return;
        VulkanContext* ctx = context;
        VkImage img = image;
        VkImageView v = view;
        uint64_t id = allocId;
        ctx->frames().defer([ctx, img, v, id] {
            if (v) vkDestroyImageView(ctx->device(), v, nullptr);
            ctx->destroyImage(img, id);
        });
        image = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        allocId = 0;
        width = height = 0;
        written = false;
    }

    bool ensure(uint32_t w, uint32_t h, VkFormat fmt) {
        if (image && w == width && h == height && fmt == format) return true;
        release();
        VkDeviceSize offset = 0;
        if (!context->createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, memory, offset, allocId)) {
            LOG_ERROR("remoteview: cannot make a %ux%u image for the picture", w, h);
            image = VK_NULL_HANDLE;
            return false;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = colorRange();
        if (vkCreateImageView(context->device(), &vi, nullptr, &view) != VK_SUCCESS) {
            context->destroyImage(image, allocId);
            image = VK_NULL_HANDLE;
            return false;
        }
        width = w;
        height = h;
        format = fmt;
        written = false;
        return true;
    }
};

RemotePictureUploader::RemotePictureUploader(VulkanContext& context) : impl_(std::make_unique<Impl>()) {
    impl_->context = &context;
}

RemotePictureUploader::~RemotePictureUploader() { impl_->release(); }

LayerImage RemotePictureUploader::upload(const uint8_t* data, uint32_t stride, uint32_t uvOffset, RemotePixels format,
                                         uint32_t w, uint32_t h) {
    if (!data || w == 0 || h == 0) return {};
    const uint8_t* src = data;
    uint32_t srcStride = stride;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    if (format == RemotePixels::NV12) {
        impl_->converted.resize(size_t(w) * h * 4);
        nv12ToRgba(data, stride, uvOffset, w, h, impl_->converted.data());
        src = impl_->converted.data();
        srcStride = w * 4;
    } else if (format == RemotePixels::BGRA8) {
        fmt = VK_FORMAT_B8G8R8A8_UNORM;
    }
    if (!impl_->ensure(w, h, fmt)) return {};

    VulkanContext& ctx = *impl_->context;
    VulkanFrames& frames = ctx.frames();
    frames.ensureFrame();
    const VkDeviceSize rowBytes = VkDeviceSize(w) * 4;
    UploadSlice slice = frames.allocUpload(rowBytes * h, 16);
    if (!slice) return {};
    auto* dst = static_cast<uint8_t*>(slice.mapped);
    if (srcStride == rowBytes) {
        std::memcpy(dst, src, size_t(rowBytes) * h);
    } else {
        for (uint32_t y = 0; y < h; ++y) std::memcpy(dst + rowBytes * y, src + size_t(srcStride) * y, rowBytes);
    }

    VkCommandBuffer cmd = frames.beginCommands();
    if (!cmd) return {};
    cmdTransitionImage(cmd, impl_->image, colorRange(),
                       impl_->written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = slice.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, slice.buffer, impl_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    cmdTransitionImage(cmd, impl_->image, colorRange(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!frames.submit(cmd)) return {};
    impl_->written = true;
    ++uploads_;
    return image();
}

LayerImage RemotePictureUploader::image() const {
    LayerImage out;
    if (!impl_->image || !impl_->written) return out;
    out.image = impl_->image;
    out.view = impl_->view;
    out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.format = impl_->format;
    out.width = impl_->width;
    out.height = impl_->height;
    return out;
}

#if !defined(_WIN32)
// The bridge is Windows' (remote_picture_d3d11.cpp).
struct D3D11PictureBridge::Impl {};
D3D11PictureBridge::D3D11PictureBridge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
D3D11PictureBridge::~D3D11PictureBridge() = default;
std::shared_ptr<D3D11PictureBridge> D3D11PictureBridge::create(VulkanContext&, std::string* err) {
    if (err) *err = "D3D11 pictures exist only on Windows";
    return nullptr;
}
std::shared_ptr<const BridgedPicture> D3D11PictureBridge::convert(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t,
                                                                  uint32_t, std::string* err) {
    if (err) *err = "no D3D11 here";
    return nullptr;
}
bool D3D11PictureBridge::readLuma(void*, void*, uint32_t, uint32_t, uint32_t, const uint32_t*, const uint32_t*,
                                  uint32_t, uint8_t*) {
    return false;
}
void D3D11PictureBridge::importPending() {}
LayerImage D3D11PictureBridge::image(const BridgedPicture&) { return {}; }
#endif

}  // namespace bro::render
