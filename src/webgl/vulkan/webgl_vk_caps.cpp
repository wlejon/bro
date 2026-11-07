// The capabilities snapshot (webgl_vk_caps.h): implementation limits read
// from the Vulkan device rather than assumed, so a page sizing itself by them
// asks for what this device (and this implementation) can do.

#include "webgl/vulkan/webgl_vk_caps.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "webgl/vulkan/webgl_vk_texformat.h"
#include "webgl/vulkan/webgl_vk_types.h"
#include "render/vulkan_context.h"
#include "render/vulkan_util.h"

#include <algorithm>
#include <climits>
#include <cstdio>

namespace bro::webgl::vk {

namespace {

int64_t clampInt(uint64_t v) { return static_cast<int64_t>(std::min<uint64_t>(v, INT_MAX)); }

// The highest count in a VkSampleCountFlags mask.
int64_t highestSampleCount(VkSampleCountFlags flags) {
    for (int64_t n = 64; n > 1; n /= 2)
        if (flags & static_cast<VkSampleCountFlags>(n)) return n;
    return 0;
}

void addLimits(WebGLCaps& caps, const VkPhysicalDeviceLimits& L) {
    auto set = [&caps](GLenum pname, int64_t v) { caps.limits.emplace_back(pname, v); };
    // --- Texture and framebuffer sizes ---
    set(GL_MAX_TEXTURE_SIZE, clampInt(L.maxImageDimension2D));
    set(GL_MAX_CUBE_MAP_TEXTURE_SIZE, clampInt(L.maxImageDimensionCube));
    set(GL_MAX_3D_TEXTURE_SIZE, clampInt(L.maxImageDimension3D));
    set(GL_MAX_ARRAY_TEXTURE_LAYERS, clampInt(L.maxImageArrayLayers));
    set(GL_MAX_RENDERBUFFER_SIZE,
        clampInt(std::min({L.maxImageDimension2D, L.maxFramebufferWidth, L.maxFramebufferHeight})));
    const int64_t drawBuffers = clampInt(std::min({8u, L.maxColorAttachments, L.maxFragmentOutputAttachments}));
    set(GL_MAX_DRAW_BUFFERS, drawBuffers);
    set(GL_MAX_COLOR_ATTACHMENTS, drawBuffers);
    set(GL_MAX_SAMPLES, highestSampleCount(L.framebufferColorSampleCounts & L.framebufferDepthSampleCounts &
                                           L.framebufferStencilSampleCounts));
    set(GL_MIN_PROGRAM_TEXEL_OFFSET, L.minTexelOffset);
    set(GL_MAX_PROGRAM_TEXEL_OFFSET, clampInt(L.maxTexelOffset));
    set(GL_SUBPIXEL_BITS, clampInt(L.subPixelPrecisionBits));

    // --- Shader resources ---
    // Attributes: the VAO and the generic-attribute block hold 16.
    set(GL_MAX_VERTEX_ATTRIBS, clampInt(std::min(16u, L.maxVertexInputAttributes)));
    // The default uniform block is a uniform buffer.
    const int64_t uniformVectors = clampInt(std::min<uint64_t>(L.maxUniformBufferRange, 65536) / 16);
    set(GL_MAX_VERTEX_UNIFORM_VECTORS, uniformVectors);
    set(GL_MAX_FRAGMENT_UNIFORM_VECTORS, uniformVectors);
    set(GL_MAX_VERTEX_UNIFORM_COMPONENTS, uniformVectors * 4);
    set(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, uniformVectors * 4);
    const int64_t varyingVectors = clampInt(std::min(L.maxVertexOutputComponents, L.maxFragmentInputComponents) / 4);
    set(GL_MAX_VARYING_VECTORS, varyingVectors);
    set(GL_MAX_VARYING_COMPONENTS, varyingVectors * 4);
    set(GL_MAX_VERTEX_OUTPUT_COMPONENTS, clampInt(L.maxVertexOutputComponents));
    set(GL_MAX_FRAGMENT_INPUT_COMPONENTS, clampInt(L.maxFragmentInputComponents));
    // Samplers: one descriptor set per program (kMaxTextureUnits). Each
    // sampler is visible only to the stages that use it, so a stage is
    // bounded by the per-stage limit and the program by the set limit.
    const int64_t stageUnits = clampInt(std::min({kMaxStageTextureUnits, L.maxPerStageDescriptorSamplers,
                                                  L.maxPerStageDescriptorSampledImages}));
    set(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, stageUnits);
    set(GL_MAX_TEXTURE_IMAGE_UNITS, stageUnits);
    set(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,
        clampInt(std::min({kMaxTextureUnits, L.maxDescriptorSetSamplers, L.maxDescriptorSetSampledImages,
                           2 * static_cast<uint32_t>(stageUnits)})));
    // Uniform blocks: the default block takes one uniform-buffer slot.
    set(GL_MAX_UNIFORM_BUFFER_BINDINGS, kMaxUniformBufferBindings);
    const int64_t blocks = clampInt(std::min<uint64_t>({kMaxUniformBufferBindings,
                                                        L.maxPerStageDescriptorUniformBuffers - 1,
                                                        L.maxDescriptorSetUniformBuffers - 1}));
    set(GL_MAX_VERTEX_UNIFORM_BLOCKS, blocks);
    set(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, blocks);
    set(GL_MAX_COMBINED_UNIFORM_BLOCKS, blocks);
    set(0x8A34 /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */, clampInt(L.minUniformBufferOffsetAlignment));
    set(GL_MAX_ELEMENTS_VERTICES, clampInt(L.maxDrawIndexedIndexValue));
    set(GL_MAX_ELEMENTS_INDICES, clampInt(L.maxDrawIndexedIndexValue));

    // The limits GL types as 64-bit.
    const auto blockSize = static_cast<int64_t>(L.maxUniformBufferRange);
    set(GL_MAX_UNIFORM_BLOCK_SIZE, blockSize);
    set(GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS, blocks * blockSize / 4 + uniformVectors * 4);
    set(GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS, blocks * blockSize / 4 + uniformVectors * 4);
    set(GL_MAX_ELEMENT_INDEX, static_cast<int64_t>(L.maxDrawIndexedIndexValue));
    // waitSync has nothing to wait on: GPU work is ordered on one queue.
    set(GL_MAX_SERVER_WAIT_TIMEOUT, 0);

    // Transform feedback's ES 3.0 minimums, which the capturing vertex stage
    // holds (0 where the device cannot capture).
    const bool tf = caps.transformFeedback;
    set(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, tf ? 64 : 0);
    set(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, tf ? kMaxFeedbackBuffers : 0);
    set(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, tf ? 4 : 0);
}

bool formatSupports(VkPhysicalDevice device, VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

// The sample counts of a renderable format, highest first.
std::vector<GLint> sampleCountsOf(render::VulkanContext& device, GLenum internalformat) {
    std::vector<GLint> counts;
    VkFormat format = colorRenderableFormat(internalformat);
    if (format == VK_FORMAT_UNDEFINED) format = depthStencilFormat(device.physicalDevice(), internalformat);
    // ES 3.0: integer formats are never multisampled.
    if (format == VK_FORMAT_UNDEFINED || isIntegerFormat(format)) return counts;
    const VkImageAspectFlags aspects = render::imageAspectFor(format);
    const VkImageUsageFlags usage = (aspects == VK_IMAGE_ASPECT_COLOR_BIT
                                         ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                         : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageFormatProperties props{};
    if (vkGetPhysicalDeviceImageFormatProperties(device.physicalDevice(), format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, usage, 0, &props) != VK_SUCCESS)
        return counts;
    const VkPhysicalDeviceLimits& limits = device.deviceProperties().limits;
    VkSampleCountFlags flags = props.sampleCounts;
    if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) flags &= limits.framebufferColorSampleCounts;
    if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) flags &= limits.framebufferDepthSampleCounts;
    if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) flags &= limits.framebufferStencilSampleCounts;
    for (GLint n = 64; n > 1; n /= 2)
        if (flags & static_cast<VkSampleCountFlags>(n)) counts.push_back(n);
    return counts;
}

void addSampleCounts(WebGLCaps& caps, render::VulkanContext& device) {
    // Every sized internal format GL has lies in [0x8000, 0xA000): whatever
    // of them colorRenderableFormat / depthStencilFormat accept is listed,
    // so this follows those two as they change.
    for (GLenum f = 0x8000; f < 0xA000; ++f) {
        const bool renderable = colorRenderableFormat(f) != VK_FORMAT_UNDEFINED ||
                                depthStencilFormat(device.physicalDevice(), f) != VK_FORMAT_UNDEFINED;
        if (renderable) caps.sampleCounts.emplace_back(f, sampleCountsOf(device, f));
    }
}

std::string vendorName(uint32_t vendorID) {
    // The PCI vendor ids Vulkan reports, and the Khronos ids of the rest.
    switch (vendorID) {
        case 0x1002: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x106B: return "Apple";
        case 0x13B5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x1010: return "Imagination Technologies";
        case 0x10005: return "Mesa";
        default: {
            char id[16];
            std::snprintf(id, sizeof id, "0x%04X", vendorID);
            return id;
        }
    }
}

void addExtensions(WebGLCaps& caps, render::VulkanContext& device) {
    const VkPhysicalDevice phys = device.physicalDevice();
    const CompressedExtension* exts = nullptr;
    const size_t n = compressedExtensions(exts);
    for (size_t i = 0; i < n; ++i) {
        if (!compressedExtensionSupported(phys, device.features(), exts[i])) continue;
        WebGLCaps::CompressedExtension ext;
        ext.name = exts[i].name;
        for (size_t f = 0; f < exts[i].count; ++f) ext.formats.push_back(static_cast<GLint>(exts[i].formats[f]));
        caps.compressedExtensions.push_back(std::move(ext));
    }

    // Compiles finish inside compileShader / linkProgram, so a parallel
    // compile is always complete; the device names itself to
    // WEBGL_debug_renderer_info.
    caps.extensions = {"EXT_color_buffer_float", "BRO_buffer_map", "WEBGL_lose_context",
                       "KHR_parallel_shader_compile", "WEBGL_debug_renderer_info"};
    // The 16-bit float formats render (and blend) on every Vulkan device.
    if (formatSupports(phys, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
        caps.extensions.emplace_back("EXT_color_buffer_half_float");
    if (caps.anisotropicFiltering) caps.extensions.emplace_back("EXT_texture_filter_anisotropic");
    // Compressed formats the device samples natively, and only those.
    for (const auto& ext : caps.compressedExtensions) caps.extensions.push_back(ext.name);
    // What 32-bit float formats can do is the device's to say.
    if (formatSupports(phys, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
        caps.extensions.emplace_back("EXT_float_blend");
    if (formatSupports(phys, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        caps.extensions.emplace_back("OES_texture_float_linear");
    // GPU timer queries, on Vulkan timestamps.
    if (caps.timerQueries()) caps.extensions.emplace_back("EXT_disjoint_timer_query_webgl2");
}

template <class T>
bool find(const std::vector<std::pair<GLenum, T>>& table, GLenum key, T& out) {
    auto it = std::lower_bound(table.begin(), table.end(), key,
                               [](const std::pair<GLenum, T>& e, GLenum k) { return e.first < k; });
    if (it == table.end() || it->first != key) return false;
    out = it->second;
    return true;
}

} // namespace

bool WebGLCaps::limit(GLenum pname, int64_t& out) const { return find(limits, pname, out); }

bool WebGLCaps::floatLimit(GLenum pname, float& out) const { return find(floatLimits, pname, out); }

const std::vector<GLint>& WebGLCaps::samples(GLenum internalformat) const {
    static const std::vector<GLint> kNone;
    auto it = std::lower_bound(sampleCounts.begin(), sampleCounts.end(), internalformat,
                               [](const auto& e, GLenum k) { return e.first < k; });
    return it != sampleCounts.end() && it->first == internalformat ? it->second : kNone;
}

bool WebGLCaps::supports(const std::string& extension) const {
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

const WebGLCaps::CompressedExtension* WebGLCaps::compressedExtension(const std::string& name) const {
    for (const auto& ext : compressedExtensions)
        if (ext.name == name) return &ext;
    return nullptr;
}

std::shared_ptr<const WebGLCaps> buildWebGLCaps(render::VulkanContext& device) {
    auto caps = std::make_shared<WebGLCaps>();
    const VkPhysicalDeviceProperties& props = device.deviceProperties();
    const VkPhysicalDeviceLimits& L = props.limits;

    caps->transformFeedback = device.features().vertexPipelineStoresAndAtomics == VK_TRUE;
    caps->anisotropicFiltering = device.features().samplerAnisotropy == VK_TRUE;
    // EXT_disjoint_timer_query_webgl2 needs the graphics queue to timestamp.
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &familyCount, families.data());
    const int family = device.queueFamilies().graphicsFamily;
    if (family >= 0 && static_cast<uint32_t>(family) < familyCount) {
        caps->timestampBits = families[static_cast<uint32_t>(family)].timestampValidBits;
        caps->timestampPeriodNs = L.timestampPeriod;
    }

    addLimits(*caps, L);
    std::sort(caps->limits.begin(), caps->limits.end());
    caps->floatLimits.emplace_back(GL_MAX_TEXTURE_LOD_BIAS, L.maxSamplerLodBias);
    if (caps->anisotropicFiltering) caps->floatLimits.emplace_back(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, L.maxSamplerAnisotropy);
    std::sort(caps->floatLimits.begin(), caps->floatLimits.end());

    caps->maxViewportDims[0] = static_cast<GLint>(clampInt(std::min(L.maxViewportDimensions[0], L.maxFramebufferWidth)));
    caps->maxViewportDims[1] = static_cast<GLint>(clampInt(std::min(L.maxViewportDimensions[1], L.maxFramebufferHeight)));
    // gl_PointSize beyond 1 needs the largePoints feature; wide lines the
    // wideLines feature (MoltenVK has none).
    caps->aliasedPointSizeRange[1] = device.largePoints() ? L.pointSizeRange[1] : 1.0f;
    caps->aliasedLineWidthRange[1] = device.wideLines() ? L.lineWidthRange[1] : 1.0f;

    caps->unmaskedVendor = vendorName(props.vendorID);
    caps->unmaskedRenderer = props.deviceName;

    addExtensions(*caps, device);
    addSampleCounts(*caps, device);
    return caps;
}

} // namespace bro::webgl::vk
