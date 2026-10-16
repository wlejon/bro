#include "render/frame_damage.h"

#include "render/glsl_compiler.h"
#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace bro::render {

void tileFlagsToRects(const uint32_t* flags, uint32_t tilesX, uint32_t tilesY, uint32_t width, uint32_t height,
                      std::vector<DamageRect>& out) {
    out.clear();
    if (!flags || tilesX == 0 || tilesY == 0 || width == 0 || height == 0) return;
    // Runs in tile units: [x0, x1) on rows [y0, y1). `open` are the runs the
    // row above ended with, which a run of this row with the same columns
    // extends.
    struct Run {
        uint32_t x0, x1, y0, y1;
    };
    std::vector<Run> open, next, done;
    for (uint32_t ty = 0; ty < tilesY; ++ty) {
        next.clear();
        const uint32_t* row = flags + static_cast<size_t>(ty) * tilesX;
        for (uint32_t tx = 0; tx < tilesX;) {
            if (!row[tx]) {
                ++tx;
                continue;
            }
            uint32_t end = tx + 1;
            while (end < tilesX && row[end]) ++end;
            Run r{tx, end, ty, ty + 1};
            for (Run& o : open) {
                if (o.x0 == tx && o.x1 == end) {
                    r.y0 = o.y0;
                    o.x1 = o.x0;  // taken
                    break;
                }
            }
            next.push_back(r);
            tx = end;
        }
        for (const Run& o : open) {
            if (o.x1 > o.x0) done.push_back(o);
        }
        open.swap(next);
    }
    done.insert(done.end(), open.begin(), open.end());
    if (done.empty()) return;

    const auto toRect = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        const uint32_t px0 = x0 * kDamageTile, py0 = y0 * kDamageTile;
        const uint32_t px1 = std::min(x1 * kDamageTile, width), py1 = std::min(y1 * kDamageTile, height);
        return DamageRect{px0, py0, px1 - px0, py1 - py0};
    };
    if (done.size() > kMaxDamageRects) {
        uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
        for (const Run& r : done) {
            x0 = std::min(x0, r.x0);
            y0 = std::min(y0, r.y0);
            x1 = std::max(x1, r.x1);
            y1 = std::max(y1, r.y1);
        }
        out.push_back(toRect(x0, y0, x1, y1));
        return;
    }
    out.reserve(done.size());
    for (const Run& r : done) out.push_back(toRect(r.x0, r.y0, r.x1, r.y1));
}

void diffFrames(const uint8_t* older, const uint8_t* newer, uint32_t width, uint32_t height, size_t stride,
                std::vector<DamageRect>& out) {
    out.clear();
    if (!older || !newer || width == 0 || height == 0) return;
    const uint32_t tilesX = (width + kDamageTile - 1) / kDamageTile;
    const uint32_t tilesY = (height + kDamageTile - 1) / kDamageTile;
    std::vector<uint32_t> flags(static_cast<size_t>(tilesX) * tilesY, 0);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t* row = flags.data() + static_cast<size_t>(y / kDamageTile) * tilesX;
        const uint8_t* a = older + static_cast<size_t>(y) * stride;
        const uint8_t* b = newer + static_cast<size_t>(y) * stride;
        if (std::memcmp(a, b, static_cast<size_t>(width) * 4) == 0) continue;
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            if (row[tx]) continue;
            const uint32_t x0 = tx * kDamageTile;
            const uint32_t n = std::min(kDamageTile, width - x0);
            if (std::memcmp(a + x0 * 4, b + x0 * 4, static_cast<size_t>(n) * 4) != 0) row[tx] = 1;
        }
    }
    tileFlagsToRects(flags.data(), tilesX, tilesY, width, height, out);
}

namespace {

// One workgroup per tile, one invocation per pixel: a pixel that differs
// marks its tile. The alpha is left out (scanout is XRGB: it is undefined).
constexpr const char* kDiffShader = R"(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D newer;
layout(set = 0, binding = 1) uniform sampler2D older;
layout(set = 0, binding = 2, std430) buffer Flags { uint flags[]; };
layout(push_constant) uniform Params { uvec2 size; uint tilesX; } pc;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= pc.size.x || p.y >= pc.size.y) return;
    vec3 a = texelFetch(newer, ivec2(p), 0).rgb;
    vec3 b = texelFetch(older, ivec2(p), 0).rgb;
    if (any(notEqual(a, b))) flags[gl_WorkGroupID.y * pc.tilesX + gl_WorkGroupID.x] = 1u;
}
)";
static_assert(kDamageTile == 16, "the shader's workgroup is one tile");

struct PushConstants {
    uint32_t width, height, tilesX, pad;
};

}  // namespace

GpuFrameDiff::GpuFrameDiff(VulkanContext& ctx) : ctx_(ctx) {
    VkDevice dev = ctx_.device();
    std::string log;
    const std::vector<uint32_t> spirv = compileGlslToSpirv(kDiffShader, ShaderStage::Compute, &log);
    if (spirv.empty()) {
        LOG_WARN("GpuFrameDiff: the diff shader does not compile: %s", log.c_str());
        return;
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) {
        sampler_ = VK_NULL_HANDLE;
        return;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &setLayout_) != VK_SUCCESS) {
        setLayout_ = VK_NULL_HANDLE;
        return;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        pipelineLayout_ = VK_NULL_HANDLE;
        return;
    }

    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = spirv.size() * sizeof(uint32_t);
    mi.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) return;

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = pipelineLayout_;
    if (vkCreateComputePipelines(dev, ctx_.pipelineCache(), 1, &ci, nullptr, &pipeline_) != VK_SUCCESS) {
        LOG_WARN("GpuFrameDiff: creating the diff pipeline failed");
        pipeline_ = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(dev, module, nullptr);
}

GpuFrameDiff::~GpuFrameDiff() {
    VkDevice dev = ctx_.device();
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
    if (setLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(dev, sampler_, nullptr);
}

bool GpuFrameDiff::record(VkCommandBuffer cmd, VkImage newer, VkImageView newerView, VkImage older,
                          VkImageView olderView, VkImageLayout layout, uint32_t width, uint32_t height) {
    flags_ = nullptr;
    if (!valid() || cmd == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    const uint32_t tilesX = (width + kDamageTile - 1) / kDamageTile;
    const uint32_t tilesY = (height + kDamageTile - 1) / kDamageTile;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(tilesX) * tilesY * sizeof(uint32_t);
    auto& frames = ctx_.frames();
    UploadSlice slice = frames.allocUpload(bytes, 16);
    VkDescriptorSet set = frames.allocDescriptorSet(setLayout_);
    if (!slice || set == VK_NULL_HANDLE) return false;
    // A host write before the submit is visible to the device without a barrier.
    std::memset(slice.mapped, 0, static_cast<size_t>(bytes));

    VkDescriptorImageInfo images[2]{};
    images[0] = {sampler_, newerView, layout};
    images[1] = {sampler_, olderView, layout};
    VkDescriptorBufferInfo buf{slice.buffer, slice.offset, bytes};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i < 2) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buf;
        }
    }
    vkUpdateDescriptorSets(ctx_.device(), 3, writes, 0, nullptr);

    // Both images' last writes, visible to the shader's reads.
    for (VkImage image : {newer, older}) {
        ImageBarrier b;
        b.image = image;
        b.oldLayout = layout;
        b.newLayout = layout;
        b.srcStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        b.srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        b.dstAccess = VK_ACCESS_SHADER_READ_BIT;
        cmdImageBarrier(cmd, b);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &set, 0, nullptr);
    const PushConstants pc{width, height, tilesX, 0};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, tilesX, tilesY, 1);
    cmdBufferBarrier(cmd, slice.buffer, slice.offset, bytes, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

    flags_ = static_cast<const uint32_t*>(slice.mapped);
    tilesX_ = tilesX;
    tilesY_ = tilesY;
    width_ = width;
    height_ = height;
    return true;
}

bool GpuFrameDiff::result(std::vector<DamageRect>& out) {
    out.clear();
    if (!flags_) return false;
    tileFlagsToRects(flags_, tilesX_, tilesY_, width_, height_, out);
    flags_ = nullptr;
    return true;
}

}  // namespace bro::render
