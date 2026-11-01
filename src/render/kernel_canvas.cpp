#include "render/kernel_canvas.h"

#include "render/kernel_shader.h"
#include "render/vulkan_compute.h"
#include "render/vulkan_context.h"
#include "render/vulkan_frames.h"
#include "render/vulkan_util.h"

namespace bro::render {

namespace {

struct BlitPush {
    int32_t width, height, offset, pad;
};

constexpr uint32_t kBlitTile = 16;   // the shader's workgroup is 16 x 16

} // namespace

KernelCanvasBlit::KernelCanvasBlit(VulkanContext& context) : context_(context) {
    VkDevice dev = context_.device();
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &setLayout_) != VK_SUCCESS) {
        setLayout_ = VK_NULL_HANDLE;
        error_ = "could not create the blit's descriptor set layout";
        return;
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlitPush)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &layout_) != VK_SUCCESS) {
        layout_ = VK_NULL_HANDLE;
        error_ = "could not create the blit's pipeline layout";
        return;
    }
    const char* entries[2] = {"blit_rgba8", "blit_rgba32f"};
    for (int i = 0; i < 2; ++i) {
        const KernelShader shader = renderKernelShader(RenderKernelSource::KernelCanvas, entries[i], &error_);
        if (!shader) return;
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = shader.spirv.size() * sizeof(uint32_t);
        mi.pCode = shader.spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) {
            error_ = "could not create the blit's shader module";
            return;
        }
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = shader.entry.c_str();
        ci.layout = layout_;
        if (vkCreateComputePipelines(dev, context_.pipelineCache(), 1, &ci, nullptr, &pipelines_[i]) != VK_SUCCESS) {
            pipelines_[i] = VK_NULL_HANDLE;
            error_ = std::string("could not create the ") + entries[i] + " pipeline";
        }
        vkDestroyShaderModule(dev, module, nullptr);
    }
}

KernelCanvasBlit::~KernelCanvasBlit() {
    const VkDevice dev = context_.device();
    const VkPipeline p0 = pipelines_[0], p1 = pipelines_[1];
    const VkPipelineLayout layout = layout_;
    const VkDescriptorSetLayout setLayout = setLayout_;
    // Frames already submitted may still run them.
    context_.frames().defer([dev, p0, p1, layout, setLayout] {
        if (p0 != VK_NULL_HANDLE) vkDestroyPipeline(dev, p0, nullptr);
        if (p1 != VK_NULL_HANDLE) vkDestroyPipeline(dev, p1, nullptr);
        if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, layout, nullptr);
        if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    });
}

bool KernelCanvasBlit::record(VkCommandBuffer cmd, const ComputeBuffer& buffer, uint32_t offset, VkImageView view,
                              uint32_t width, uint32_t height, bool rgba32f) {
    if (!valid()) return false;
    VkDescriptorSet set = context_.frames().allocDescriptorSet(setLayout_);
    if (set == VK_NULL_HANDLE) return false;
    // The whole buffer is bound (a storage buffer's offset has an alignment
    // a byte offset need not keep); the shader starts at `offset` texels.
    VkDescriptorBufferInfo bufInfo{buffer.buffer(), 0, buffer.size()};
    VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &bufInfo;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(context_.device(), 2, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[rgba32f ? 1 : 0]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    const BlitPush pc{static_cast<int32_t>(width), static_cast<int32_t>(height), static_cast<int32_t>(offset), 0};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (width + kBlitTile - 1) / kBlitTile, (height + kBlitTile - 1) / kBlitTile, 1);
    return true;
}

KernelCanvas::KernelCanvas(VulkanContext& context, std::shared_ptr<KernelCanvasBlit> blit)
    : context_(context), blit_(std::move(blit)) {}

KernelCanvas::~KernelCanvas() { releaseImage(); }

void KernelCanvas::releaseImage() {
    if (image_ == VK_NULL_HANDLE) return;
    VulkanContext* ctx = &context_;
    const VkDevice dev = context_.device();
    const VkImage image = image_;
    const VkImageView view = view_;
    const uint64_t allocId = allocId_;
    // Frames already submitted may still sample it.
    context_.frames().defer([ctx, dev, image, view, allocId] {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
        ctx->destroyImage(image, allocId);
    });
    image_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    allocId_ = 0;
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    width_ = height_ = 0;
    format_ = VK_FORMAT_UNDEFINED;
}

bool KernelCanvas::ensureImage(uint32_t width, uint32_t height, VkFormat format, std::string* err) {
    if (image_ != VK_NULL_HANDLE && width == width_ && height == height_ && format == format_) return true;
    releaseImage();
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memOffset = 0;
    if (!context_.createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image_, memory, memOffset, allocId_)) {
        image_ = VK_NULL_HANDLE;
        if (err) *err = "could not allocate a " + std::to_string(width) + "x" + std::to_string(height) + " image";
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(context_.device(), &vi, nullptr, &view_) != VK_SUCCESS) {
        view_ = VK_NULL_HANDLE;
        releaseImage();
        if (err) *err = "could not create the image's view";
        return false;
    }
    width_ = width;
    height_ = height;
    format_ = format;
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

bool KernelCanvas::present(const ComputeBuffer& buffer, uint64_t offset, uint32_t width, uint32_t height,
                           Format format, std::string* err) {
    const bool f32 = format == Format::RGBA32F;
    const uint32_t texelBytes = f32 ? 16u : 4u;
    const uint64_t bytes = static_cast<uint64_t>(width) * height * texelBytes;
    if (width == 0 || height == 0) {
        if (err) *err = "the canvas has no pixels";
        return false;
    }
    if (offset % texelBytes != 0) {
        if (err) *err = "byteOffset " + std::to_string(offset) + " is not a multiple of the texel's " +
                        std::to_string(texelBytes) + " bytes";
        return false;
    }
    if (offset > buffer.size() || bytes > buffer.size() - offset || offset / texelBytes > 0x7fffffffull) {
        if (err)
            *err = std::to_string(width) + "x" + std::to_string(height) + " texels of " + std::to_string(texelBytes) +
                   " bytes run past the buffer (" + std::to_string(buffer.size()) + " bytes)";
        return false;
    }
    if (!blit_ || !blit_->valid()) {
        if (err) *err = blit_ ? blit_->error() : "no blit pipeline";
        return false;
    }
    if (!ensureImage(width, height, f32 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM, err)) return false;
    const VkImageLayout oldLayout = layout_;
    bool stored = false;
    const bool recorded = context_.compute().recordGraphics([&](VkCommandBuffer cmd) {
        // Earlier frames' fragment stages done sampling it, then the store.
        ImageBarrier toStore;
        toStore.image = image_;
        toStore.oldLayout = oldLayout;
        toStore.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toStore.srcStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        toStore.srcAccess = 0;
        toStore.dstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        toStore.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
        cmdImageBarrier(cmd, toStore);
        stored = blit_->record(cmd, buffer, static_cast<uint32_t>(offset / texelBytes), view_, width, height, f32);
        ImageBarrier toRead;
        toRead.image = image_;
        toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        toRead.srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        toRead.dstStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        toRead.dstAccess = VK_ACCESS_SHADER_READ_BIT;
        cmdImageBarrier(cmd, toRead);
    });
    if (recorded) layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!recorded || !stored) {
        if (err) *err = "the store could not be recorded";
        return false;
    }
    ++generation_;
    return true;
}

LayerImage KernelCanvas::image() const {
    LayerImage img;
    if (image_ == VK_NULL_HANDLE) return img;
    img.image = image_;
    img.view = view_;
    img.layout = layout_;
    img.format = format_;
    img.width = width_;
    img.height = height_;
    return img;
}

} // namespace bro::render
