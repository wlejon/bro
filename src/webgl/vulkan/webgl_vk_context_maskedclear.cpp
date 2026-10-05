// Clears the write masks only partly allow: a color mask that leaves some
// channels out, or a stencil write mask with some bits clear. Attachment
// clears write whole texels, so these are a draw of one triangle over the
// pass under the masks, with the clear value as its output. The fragment
// shader is generated per pass layout (an output of each written
// attachment's kind) and cached.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/glsl_compiler.h"
#include "util/log.h"

#include <cstring>

namespace bro::webgl::vk {

namespace {

// What both stages read: the clear value's raw bits (each written
// attachment's output reinterprets them as its kind) and the clear depth.
struct ClearPush {
    uint32_t color[4];
    float depth;
};

constexpr const char* kPushBlock =
    "layout(push_constant) uniform ClearPush { uvec4 color; float depth; } pc;\n";

VkShaderModule shaderModule(VkDevice device, const std::string& source, render::ShaderStage stage) {
    std::string log;
    const std::vector<uint32_t> spirv = render::compileGlslToSpirv(source, stage, &log);
    if (spirv.empty()) {
        LOG_ERROR("WebGLVkContext: masked clear shader failed: %s", log.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &module);
    return module;
}

} // namespace

// The fragment shader writing the clear value to the attachments in
// `colorMask` (bits of pass color indices), each as its format's kind.
VkShaderModule WebGLVkContext::maskedClearShader(uint32_t colorMask) {
    std::string key;
    std::string body;
    std::string decls;
    for (uint32_t i = 0; i < pass_.colorCount; ++i) {
        if (!(colorMask & (1u << i)) || !pass_.color[i]) continue;
        const VkFormat format = pass_.color[i].format;
        const char kind = !isIntegerFormat(format) ? 'f' : isSignedIntegerFormat(format) ? 'i' : 'u';
        key += std::to_string(i) + kind;
        const std::string name = "o" + std::to_string(i);
        const char* type = kind == 'f' ? "vec4" : kind == 'i' ? "ivec4" : "uvec4";
        decls += "layout(location = " + std::to_string(i) + ") out " + type + " " + name + ";\n";
        body += "    " + name + " = " +
                (kind == 'f' ? "uintBitsToFloat(pc.color)" : kind == 'i' ? "ivec4(pc.color)" : "pc.color") + ";\n";
    }
    auto it = maskedClear_.fragments.find(key);
    if (it != maskedClear_.fragments.end()) return it->second;
    const std::string source = std::string("#version 450\n") + kPushBlock + decls + "void main() {\n" + body + "}\n";
    VkShaderModule module = shaderModule(context_.device(), source, render::ShaderStage::Fragment);
    if (module != VK_NULL_HANDLE) maskedClear_.fragments.emplace(key, module);
    return module;
}

bool WebGLVkContext::maskedClearResources() {
    VkDevice dev = context_.device();
    if (maskedClear_.layout == VK_NULL_HANDLE) {
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ClearPush)};
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(dev, &info, nullptr, &maskedClear_.layout) != VK_SUCCESS) return false;
    }
    if (maskedClear_.vertex == VK_NULL_HANDLE) {
        // One triangle covering the target, at the clear depth.
        const std::string source = std::string("#version 450\n") + kPushBlock +
                                   "void main() {\n"
                                   "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
                                   "    gl_Position = vec4(p * 2.0 - 1.0, pc.depth, 1.0);\n"
                                   "}\n";
        maskedClear_.vertex = shaderModule(dev, source, render::ShaderStage::Vertex);
    }
    return maskedClear_.vertex != VK_NULL_HANDLE;
}

void WebGLVkContext::destroyMaskedClear() {
    VkDevice dev = context_.device();
    for (auto& [key, module] : maskedClear_.fragments) vkDestroyShaderModule(dev, module, nullptr);
    if (maskedClear_.vertex != VK_NULL_HANDLE) vkDestroyShaderModule(dev, maskedClear_.vertex, nullptr);
    if (maskedClear_.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, maskedClear_.layout, nullptr);
    maskedClear_ = MaskedClear{};
}

// Clear the color attachments in `colorMask` to `color` (raw bits) under the
// color mask, and/or the stencil buffer to `stencil` under the front stencil
// write mask, inside the open pass and the scissor box.
void WebGLVkContext::clearMasked(uint32_t colorMask, const uint32_t color[4], bool stencil, int32_t stencilValue) {
    if (!colorMask && !stencil) return;
    if (!maskedClearResources()) return;
    VkShaderModule frag = maskedClearShader(colorMask);
    if (frag == VK_NULL_HANDLE) return;

    PipelineKey key{};
    key.vertShader = maskedClear_.vertex;
    key.fragShader = frag;
    key.cullFaceEnable = VK_FALSE;
    key.depthTestEnable = VK_FALSE;
    key.depthWriteEnable = VK_FALSE;
    key.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    key.stencilTestEnable = stencil ? VK_TRUE : VK_FALSE;
    const VkStencilOpState replace{VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                                   VK_COMPARE_OP_ALWAYS,  0xFF, stencilWriteMaskFront_ & 0xFF,
                                   static_cast<uint32_t>(stencilValue)};
    key.stencilFront = replace;
    key.stencilBack = replace;
    key.blendEnable = VK_FALSE;
    key.colorWriteMask = 0;
    if (colorMask_[0]) key.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (colorMask_[1]) key.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (colorMask_[2]) key.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (colorMask_[3]) key.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    key.colorAttachmentCount = pass_.colorCount;
    for (uint32_t i = 0; i < 8; ++i) {
        key.colorAttachmentFormats[i] =
            i < pass_.colorCount && pass_.color[i] ? pass_.color[i].format : VK_FORMAT_UNDEFINED;
        if (i < pass_.colorCount && !(colorMask & (1u << i))) key.unwrittenMask |= static_cast<uint8_t>(1u << i);
        if (i < pass_.colorCount && pass_.color[i].alphaOne) key.alphaOneMask |= static_cast<uint8_t>(1u << i);
    }
    key.depthAttachmentFormat = pass_.depth ? pass_.depth.format : VK_FORMAT_UNDEFINED;
    key.stencilAttachmentFormat = pass_.stencil ? pass_.stencil.format : VK_FORMAT_UNDEFINED;
    key.samples = pass_.samples;

    VkPipeline pipeline = pipelineCache_.getOrCreatePipeline(key, maskedClear_.layout);
    if (pipeline == VK_NULL_HANDLE) return;
    // GL clears are not samples an occlusion query counts: draw in a pass
    // of its own without the query's slot.
    const bool suspend = openQuerySlot_ >= 0;
    if (suspend) {
        occlusionSuspended_ = true;
        endRendering();
        beginRendering();
        occlusionSuspended_ = false;
        if (!inRenderPass_) return;
    }
    VkCommandBuffer cmd = commands();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    ClearPush push{};
    std::memcpy(push.color, color, sizeof(push.color));
    vkCmdPushConstants(cmd, maskedClear_.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    setDynamicState(cmd, pass_.extent);
    // The whole target, whatever the GL viewport (clears ignore it).
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(pass_.extent.width),
                        static_cast<float>(pass_.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (suspend) endRendering();
}

} // namespace bro::webgl::vk
