// D3D11PictureBridge (remote_picture.h): Media Foundation's decoded NV12
// pictures to Vulkan images with no CPU readback.
//
// On the decoder's own D3D11 device (the decode thread), the video processor
// converts the picture's slice of the decoder's texture into one of a ring of
// BGRA textures made shareable (an NT handle). bro's Vulkan device imports
// each texture once (VK_KHR_external_memory_win32, D3D11_TEXTURE handle
// type, a dedicated allocation) and samples it in place. The decode thread
// waits for the conversion's event query before handing the slot over, so
// the image is complete before any Vulkan submission that samples it.
//
// Slots: created by the decode thread, imported by the main thread
// (importPending) before they are ever converted into, so the import's
// layout transition never meets a picture. A slot is busy while a
// BridgedPicture naming it lives; the engine keeps the one it shows until the
// GPU frames that sampled it have completed. A new decoder device or picture
// size makes a new ring; the old slots go when their last holder does.

#include "render/remote_picture.h"

#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace bro::render {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kSlots = 6;  // shown + two frames in flight + one converting, with room

std::string hrText(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

struct Slot {
    // D3D11 (decode thread).
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    HANDLE shared = nullptr;  // NT handle, closed once imported (or with the slot)
    uint32_t width = 0, height = 0;
    std::atomic<bool> imported{false};
    std::atomic<bool> busy{false};
    // Vulkan (main thread makes them; whoever drops the last reference
    // destroys them, by then unused by the GPU).
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    ~Slot() {
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        if (shared) CloseHandle(shared);
    }
};

// Wait for the device's work so far (an event query), spinning briefly: the
// conversion takes well under a millisecond on any GPU that decodes.
bool waitForGpu(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(device->CreateQuery(&qd, &query))) return false;
    ctx->End(query.Get());
    ctx->Flush();
    for (int spins = 0;; ++spins) {
        BOOL done = FALSE;
        const HRESULT hr = ctx->GetData(query.Get(), &done, sizeof done, 0);
        if (hr == S_OK && done) return true;
        if (FAILED(hr)) return false;
        if (spins > 200000) return false;  // a second or more: something is wrong
        if (spins < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

}  // namespace

struct D3D11PictureBridge::Impl {
    VulkanContext* context = nullptr;
    LUID vulkanLuid{};
    bool haveLuid = false;
    PFN_vkGetMemoryWin32HandlePropertiesKHR getHandleProperties = nullptr;

    // Decode thread.
    ID3D11Device* device = nullptr;  // the device the state below was made on (not owned)
    bool adapterChecked = false, adapterOk = false;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    uint32_t procInW = 0, procInH = 0, procOutW = 0, procOutH = 0;
    struct InputView {
        ID3D11Texture2D* texture = nullptr;  // kept alive by the view
        uint32_t subresource = 0;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };
    std::vector<InputView> inputViews;
    ComPtr<ID3D11Texture2D> lumaStaging;
    uint32_t stagingW = 0, stagingH = 0;

    // The ring; the main thread reads it under `m` to import.
    std::mutex m;
    std::condition_variable importedCv;  // importPending imported a slot
    std::vector<std::shared_ptr<Slot>> ring;
    uint64_t serial = 0;

    void resetDevice(ID3D11Device* d) {
        device = d;
        adapterChecked = adapterOk = false;
        device1.Reset();
        videoDevice.Reset();
        videoContext.Reset();
        enumerator.Reset();
        processor.Reset();
        procInW = procInH = procOutW = procOutH = 0;
        inputViews.clear();
        lumaStaging.Reset();
        stagingW = stagingH = 0;
        std::lock_guard<std::mutex> lk(m);
        ring.clear();
    }

    bool checkAdapter(std::string* err) {
        if (adapterChecked) return adapterOk;
        adapterChecked = true;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&desc))) {
            if (err) *err = "cannot tell the decoder's adapter";
            return adapterOk = false;
        }
        if (haveLuid && std::memcmp(&desc.AdapterLuid, &vulkanLuid, sizeof(LUID)) != 0) {
            if (err) *err = "the decoder's adapter is not the one bro renders on";
            return adapterOk = false;
        }
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
            FAILED(device->QueryInterface(IID_PPV_ARGS(&videoDevice)))) {
            if (err) *err = "the decoder's device has no video processor";
            return adapterOk = false;
        }
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        if (FAILED(ctx.As(&videoContext))) {
            if (err) *err = "the decoder's device has no video context";
            return adapterOk = false;
        }
        return adapterOk = true;
    }

    bool ensureProcessor(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, std::string* err) {
        if (processor && inW == procInW && inH == procInH && outW == procOutW && outH == procOutH) return true;
        processor.Reset();
        enumerator.Reset();
        inputViews.clear();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = inW;
        cd.InputHeight = inH;
        cd.OutputWidth = outW;
        cd.OutputHeight = outH;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = videoDevice->CreateVideoProcessorEnumerator(&cd, &enumerator);
        if (FAILED(hr)) {
            if (err) *err = hrText("CreateVideoProcessorEnumerator", hr);
            return false;
        }
        hr = videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor);
        if (FAILED(hr)) {
            if (err) *err = hrText("CreateVideoProcessor", hr);
            return false;
        }
        procInW = inW;
        procInH = inH;
        procOutW = outW;
        procOutH = outH;
        // BT.709 limited-range YCbCr in, full-range RGB out; no enhancement.
        ComPtr<ID3D11VideoContext1> vc1;
        if (SUCCEEDED(videoContext.As(&vc1))) {
            vc1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            vc1->VideoProcessorSetOutputColorSpace1(processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
            in.YCbCr_Matrix = 1;
            in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &in);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
            out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &out);
        }
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        return true;
    }

    ID3D11VideoProcessorInputView* inputView(ID3D11Texture2D* tex, uint32_t sub, std::string* err) {
        for (auto& v : inputViews) {
            if (v.texture == tex && v.subresource == sub) return v.view.Get();
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = 0;
        d.Texture2D.ArraySlice = sub;
        InputView v;
        v.texture = tex;
        v.subresource = sub;
        const HRESULT hr = videoDevice->CreateVideoProcessorInputView(tex, enumerator.Get(), &d, &v.view);
        if (FAILED(hr)) {
            if (err) *err = hrText("CreateVideoProcessorInputView", hr);
            return nullptr;
        }
        if (inputViews.size() >= 64) inputViews.erase(inputViews.begin());
        inputViews.push_back(std::move(v));
        return inputViews.back().view.Get();
    }

    std::shared_ptr<Slot> makeSlot(uint32_t w, uint32_t h, std::string* err) {
        auto s = std::make_shared<Slot>();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &s->texture);
        if (FAILED(hr)) {
            if (err) *err = hrText("CreateTexture2D (shared)", hr);
            return nullptr;
        }
        ComPtr<IDXGIResource1> res;
        if (FAILED(s->texture.As(&res))) {
            if (err) *err = "no IDXGIResource1";
            return nullptr;
        }
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                     &s->shared);
        if (FAILED(hr)) {
            if (err) *err = hrText("CreateSharedHandle", hr);
            return nullptr;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = videoDevice->CreateVideoProcessorOutputView(s->texture.Get(), enumerator.Get(), &od, &s->outputView);
        if (FAILED(hr)) {
            if (err) *err = hrText("CreateVideoProcessorOutputView", hr);
            return nullptr;
        }
        s->width = w;
        s->height = h;
        return s;
    }

    // Main thread.
    bool import(Slot& s) {
        VkDevice dev = context->device();
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &ext;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {s.width, s.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        s.device = dev;
        if (vkCreateImage(dev, &ici, nullptr, &s.image) != VK_SUCCESS) return false;

        VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (!getHandleProperties ||
            getHandleProperties(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, s.shared, &hp) != VK_SUCCESS) {
            LOG_WARN("remoteview: vkGetMemoryWin32HandlePropertiesKHR failed");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, s.image, &req);
        const uint32_t bits = hp.memoryTypeBits & req.memoryTypeBits;
        if (!bits) return false;
        uint32_t type = 0;
        while (!(bits & (1u << type))) ++type;

        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = s.image;
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imp.pNext = &ded;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle = s.shared;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &imp;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev, &ai, nullptr, &s.memory) != VK_SUCCESS) return false;
        if (vkBindImageMemory(dev, s.image, s.memory, 0) != VK_SUCCESS) return false;
        // Importing an NT handle does not take it; the memory keeps the
        // texture alive now.
        CloseHandle(s.shared);
        s.shared = nullptr;

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = colorRange();
        if (vkCreateImageView(dev, &vi, nullptr, &s.view) != VK_SUCCESS) return false;

        // Into the layout the compositor expects, before anything is drawn
        // into it (the slot is not converted into until it is imported).
        VulkanFrames& frames = context->frames();
        frames.ensureFrame();
        VkCommandBuffer cmd = frames.beginCommands();
        if (!cmd) return false;
        cmdTransitionImage(cmd, s.image, colorRange(), VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // The transition must be done before the decode thread writes the
        // texture: wait for this one submission.
        const uint64_t ticket = frames.submit(cmd);
        if (!ticket) return false;
        frames.queue().wait(ticket);
        return true;
    }
};

D3D11PictureBridge::D3D11PictureBridge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

D3D11PictureBridge::~D3D11PictureBridge() = default;

std::shared_ptr<D3D11PictureBridge> D3D11PictureBridge::create(VulkanContext& context, std::string* err) {
    if (!context.isValid() || !context.hasExternalMemoryWin32()) {
        if (err) *err = "the Vulkan device has no VK_KHR_external_memory_win32";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->context = &context;
    impl->getHandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vkGetDeviceProcAddr(context.device(), "vkGetMemoryWin32HandlePropertiesKHR"));
    if (!impl->getHandleProperties) {
        if (err) *err = "vkGetMemoryWin32HandlePropertiesKHR is missing";
        return nullptr;
    }
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(context.physicalDevice(), &p2);
    if (idp.deviceLUIDValid) {
        std::memcpy(&impl->vulkanLuid, idp.deviceLUID, sizeof(LUID));
        impl->haveLuid = true;
        // brovideo's decoders make their D3D11 device on the default
        // adapter: when that is not the one bro renders on, pictures cannot
        // be shared, and the session should ask for CPU pictures instead.
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)) &&
            SUCCEEDED(adapter->GetDesc1(&desc)) &&
            std::memcmp(&desc.AdapterLuid, &impl->vulkanLuid, sizeof(LUID)) != 0) {
            if (err) *err = "the default adapter, which decodes, is not the one bro renders on";
            return nullptr;
        }
    }
    return std::shared_ptr<D3D11PictureBridge>(new D3D11PictureBridge(std::move(impl)));
}

std::shared_ptr<const BridgedPicture> D3D11PictureBridge::convert(void* devicePtr, void* texturePtr,
                                                                  uint32_t subresource, uint32_t x, uint32_t y,
                                                                  uint32_t w, uint32_t h, std::string* err) {
    Impl& im = *impl_;
    auto* device = static_cast<ID3D11Device*>(devicePtr);
    auto* texture = static_cast<ID3D11Texture2D*>(texturePtr);
    if (!device || !texture || w == 0 || h == 0) {
        if (err) *err = "no picture";
        return nullptr;
    }
    if (device != im.device) im.resetDevice(device);
    if (!im.checkAdapter(err)) return nullptr;

    D3D11_TEXTURE2D_DESC sd{};
    texture->GetDesc(&sd);
    if (!im.ensureProcessor(sd.Width, sd.Height, w, h, err)) return nullptr;

    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lk(im.m);
        if (!im.ring.empty() && (im.ring.front()->width != w || im.ring.front()->height != h)) im.ring.clear();
        if (im.ring.empty()) {
            for (uint32_t i = 0; i < kSlots; ++i) {
                auto s = im.makeSlot(w, h, err);
                if (!s) {
                    im.ring.clear();
                    return nullptr;
                }
                im.ring.push_back(std::move(s));
            }
        }
        auto freeSlot = [&]() -> std::shared_ptr<Slot> {
            for (auto& s : im.ring) {
                if (s->imported.load(std::memory_order_acquire) && !s->busy.load(std::memory_order_acquire)) return s;
            }
            return nullptr;
        };
        slot = freeSlot();
        // A new ring waits for the main thread to import it (its next
        // frame): a stream's first picture may be the only one for a while
        // (a still desktop sends nothing more), so it must not be dropped.
        auto pending = [&] {
            return std::any_of(im.ring.begin(), im.ring.end(), [](const auto& s) {
                return s->shared && !s->imported.load(std::memory_order_acquire);
            });
        };
        if (!slot && pending()) {
            std::unique_lock<std::mutex> ul(im.m, std::adopt_lock);
            im.importedCv.wait_for(ul, std::chrono::milliseconds(250), [&] { return (slot = freeSlot()) != nullptr; });
            ul.release();
        }
    }
    if (!slot) {
        if (err) *err = "no slot is free (or none imported yet)";
        return nullptr;
    }

    ID3D11VideoProcessorInputView* in = im.inputView(texture, subresource, err);
    if (!in) return nullptr;
    RECT src{LONG(x), LONG(y), LONG(x + w), LONG(y + h)};
    RECT dst{0, 0, LONG(w), LONG(h)};
    im.videoContext->VideoProcessorSetStreamSourceRect(im.processor.Get(), 0, TRUE, &src);
    im.videoContext->VideoProcessorSetStreamDestRect(im.processor.Get(), 0, TRUE, &dst);
    im.videoContext->VideoProcessorSetOutputTargetRect(im.processor.Get(), TRUE, &dst);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in;
    const HRESULT hr = im.videoContext->VideoProcessorBlt(im.processor.Get(), slot->outputView.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        if (err) *err = hrText("VideoProcessorBlt", hr);
        return nullptr;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!waitForGpu(device, ctx.Get())) {
        if (err) *err = "the conversion did not finish";
        return nullptr;
    }

    slot->busy.store(true, std::memory_order_release);
    auto out = std::make_shared<BridgedPicture>();
    // The holder marks the slot free again when the last copy goes, and
    // keeps the slot (and its Vulkan image) alive until then.
    out->slot = std::shared_ptr<void>(slot.get(), [keep = slot](void*) mutable {
        keep->busy.store(false, std::memory_order_release);
        keep.reset();
    });
    out->width = w;
    out->height = h;
    out->serial = ++im.serial;
    conversions_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

bool D3D11PictureBridge::readLuma(void* devicePtr, void* texturePtr, uint32_t subresource, uint32_t x0, uint32_t y0,
                                  const uint32_t* xs, const uint32_t* ys, uint32_t n, uint8_t* out) {
    Impl& im = *impl_;
    auto* device = static_cast<ID3D11Device*>(devicePtr);
    auto* texture = static_cast<ID3D11Texture2D*>(texturePtr);
    if (!device || !texture || n == 0) return false;
    if (device != im.device) im.resetDevice(device);
    D3D11_TEXTURE2D_DESC sd{};
    texture->GetDesc(&sd);
    if (sd.Format != DXGI_FORMAT_NV12) return false;
    uint32_t maxX = 0, maxY = 0;
    for (uint32_t i = 0; i < n; ++i) {
        maxX = std::max(maxX, xs[i]);
        maxY = std::max(maxY, ys[i]);
    }
    // An NV12 region must have even sizes and origin.
    const uint32_t bx = x0 & ~1u, by = y0 & ~1u;
    uint32_t bw = ((x0 - bx + maxX + 2) + 1) & ~1u;
    uint32_t bh = ((y0 - by + maxY + 2) + 1) & ~1u;
    bw = std::min(bw, sd.Width - bx);
    bh = std::min(bh, sd.Height - by);
    if (!im.lumaStaging || im.stagingW != bw || im.stagingH != bh) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = bw;
        td.Height = bh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        im.lumaStaging.Reset();
        if (FAILED(device->CreateTexture2D(&td, nullptr, &im.lumaStaging))) return false;
        im.stagingW = bw;
        im.stagingH = bh;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    D3D11_BOX box{bx, by, 0, bx + bw, by + bh, 1};
    ctx->CopySubresourceRegion(im.lumaStaging.Get(), 0, 0, 0, 0, texture, subresource, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(im.lumaStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    const auto* base = static_cast<const uint8_t*>(map.pData);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t px = x0 - bx + xs[i], py = y0 - by + ys[i];
        out[i] = (px < bw && py < bh) ? base[size_t(py) * map.RowPitch + px] : 0;
    }
    ctx->Unmap(im.lumaStaging.Get(), 0);
    return true;
}

void D3D11PictureBridge::importPending() {
    Impl& im = *impl_;
    std::vector<std::shared_ptr<Slot>> todo;
    {
        std::lock_guard<std::mutex> lk(im.m);
        for (auto& s : im.ring) {
            if (!s->imported.load(std::memory_order_acquire) && s->shared) todo.push_back(s);
        }
    }
    for (auto& s : todo) {
        if (im.import(*s)) {
            {
                std::lock_guard<std::mutex> lk(im.m);
                s->imported.store(true, std::memory_order_release);
            }
            im.importedCv.notify_all();
            ++imports_;
        } else {
            LOG_WARN("remoteview: importing a %ux%u D3D11 texture into Vulkan failed", s->width, s->height);
            // Never converted into; it stays unimported (and the bridge
            // falls back to failing conversions, which the view reports).
            if (s->shared) {
                CloseHandle(s->shared);
                s->shared = nullptr;
            }
        }
    }
}

LayerImage D3D11PictureBridge::image(const BridgedPicture& picture) {
    auto* s = static_cast<Slot*>(picture.slot.get());
    LayerImage out;
    if (!s || !s->imported.load(std::memory_order_acquire) || !s->image) return out;
    out.image = s->image;
    out.view = s->view;
    out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.format = VK_FORMAT_B8G8R8A8_UNORM;
    out.width = s->width;
    out.height = s->height;
    return out;
}

}  // namespace bro::render
