// Shader and program objects: compile (a glslang check of one stage), link
// (both stages to SPIR-V with their reflection, webgl_vk_glsl.h) and the
// per-program Vulkan objects a linked program draws with.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_glsl.h"
#include "util/log.h"

#include <algorithm>
#include <string>

namespace bro::webgl::vk {

namespace {

VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

} // namespace

WebGLShader WebGLVkContext::createShader(GLenum type) {
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        setSyntheticError(GL_INVALID_ENUM);
        return {};
    }
    GLuint id = nextObjectId_++;
    shaders_[id] = VkShaderResource{type};
    return {id, type};
}

void WebGLVkContext::deleteShader(WebGLShader s) {
    auto it = shaders_.find(s.id);
    if (it == shaders_.end()) return;
    it->second.deleteStatus = true;
    if (it->second.attachCount == 0) shaders_.erase(it);
}

void WebGLVkContext::shaderSource(WebGLShader s, const std::string& src) {
    if (auto it = shaders_.find(s.id); it != shaders_.end()) it->second.source = src;
}

void WebGLVkContext::compileShader(WebGLShader s) {
    auto it = shaders_.find(s.id);
    if (it == shaders_.end()) return;
    VkShaderResource& sh = it->second;
    sh.compileStatus = glsl::compile(sh.source, sh.type, sh.infoLog);
}

GLint WebGLVkContext::getShaderParameter(WebGLShader s, GLenum pname) {
    auto it = shaders_.find(s.id);
    if (it == shaders_.end()) return 0;
    if (pname == GL_COMPILE_STATUS) return it->second.compileStatus ? GL_TRUE : GL_FALSE;
    if (pname == GL_SHADER_TYPE) return static_cast<GLint>(it->second.type);
    if (pname == GL_DELETE_STATUS) return it->second.deleteStatus ? GL_TRUE : GL_FALSE;
    return 0;
}

std::string WebGLVkContext::getShaderInfoLog(WebGLShader s) {
    auto it = shaders_.find(s.id);
    return (it != shaders_.end()) ? it->second.infoLog : "";
}

WebGLProgram WebGLVkContext::createProgram() {
    GLuint id = nextObjectId_++;
    programs_[id] = VkProgramResource{};
    return {id};
}

// The executable a link made: modules (no recorded command refers to them),
// and the layouts, which recorded commands do and so go once the GPU is done.
void WebGLVkContext::releaseProgramExecutable(VkProgramResource& prog) {
    VkDevice dev = context_.device();
    pipelineCache_.evictShaders(prog.vertModule, prog.fragModule);
    if (prog.vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(dev, prog.vertModule, nullptr);
    if (prog.fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(dev, prog.fragModule, nullptr);
    prog.vertModule = VK_NULL_HANDLE;
    prog.fragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = prog.setLayout;
    VkPipelineLayout pipelineLayout = prog.pipelineLayout;
    if (setLayout != VK_NULL_HANDLE || pipelineLayout != VK_NULL_HANDLE) {
        stream_.defer([dev, setLayout, pipelineLayout] {
            if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
            if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
        });
    }
    prog.setLayout = VK_NULL_HANDLE;
    prog.pipelineLayout = VK_NULL_HANDLE;
    prog.drawSegmentSerial = 0;
    prog.drawSet = VK_NULL_HANDLE;
}

void WebGLVkContext::destroyProgram(GLuint id) {
    auto it = programs_.find(id);
    if (it == programs_.end()) return;
    releaseProgramExecutable(it->second);
    if (it->second.vertShaderId != 0) detachShader({id}, {it->second.vertShaderId});
    if (it->second.fragShaderId != 0) detachShader({id}, {it->second.fragShaderId});
    programs_.erase(it);
}

void WebGLVkContext::deleteProgram(WebGLProgram p) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return;
    // The current program stays usable until another one replaces it.
    if (currentProgramId_ == p.id) {
        it->second.deleteStatus = true;
        return;
    }
    destroyProgram(p.id);
}

void WebGLVkContext::attachShader(WebGLProgram p, WebGLShader s) {
    auto itP = programs_.find(p.id);
    auto itS = shaders_.find(s.id);
    if (itP == programs_.end() || itS == shaders_.end()) return;
    GLuint& slot = itS->second.type == GL_VERTEX_SHADER ? itP->second.vertShaderId : itP->second.fragShaderId;
    if (slot != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    slot = s.id;
    itS->second.attachCount++;
}

void WebGLVkContext::detachShader(WebGLProgram p, WebGLShader s) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return;
    GLuint sId = 0;
    if (it->second.vertShaderId == s.id) { sId = s.id; it->second.vertShaderId = 0; }
    if (it->second.fragShaderId == s.id) { sId = s.id; it->second.fragShaderId = 0; }
    if (sId == 0) return;
    auto itS = shaders_.find(sId);
    if (itS == shaders_.end()) return;
    if (itS->second.attachCount > 0) itS->second.attachCount--;
    if (itS->second.deleteStatus && itS->second.attachCount == 0) shaders_.erase(itS);
}

// One descriptor set layout for the program — its samplers, uniform blocks
// and default-block buffer at the bindings glslang gave them — and the
// pipeline layout adding the fragment stage's FragmentPush.
bool WebGLVkContext::buildProgramLayouts(VkProgramResource& prog) {
    const ProgramInterface& iface = prog.iface;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const VkSamplerBinding& s : iface.samplers)
        bindings.push_back({s.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, s.count,
                            s.stages ? s.stages : VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    for (const VkUniformBlockInfo& b : iface.uniformBlocks) {
        auto it = std::find_if(bindings.begin(), bindings.end(),
                               [&](const VkDescriptorSetLayoutBinding& x) { return x.binding == b.descriptorBinding; });
        if (it == bindings.end())
            bindings.push_back({b.descriptorBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b.arrayElement + 1,
                                VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
        else
            it->descriptorCount = std::max(it->descriptorCount, b.arrayElement + 1);
    }
    if (iface.defaultBlockBinding >= 0)
        bindings.push_back({static_cast<uint32_t>(iface.defaultBlockBinding), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                            VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    for (uint32_t b = 0; b < iface.feedbackBuffers; ++b)
        bindings.push_back({kFeedbackBinding + b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                            nullptr});

    VkDevice dev = context_.device();
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &prog.setLayout) != VK_SUCCESS) return false;

    // A capturing vertex stage reads its FeedbackPush past the fragment's.
    const VkPushConstantRange pushes[] = {
        {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FragmentPush)},
        {VK_SHADER_STAGE_VERTEX_BIT, kFeedbackPushOffset, sizeof(FeedbackPush)},
    };
    VkPipelineLayoutCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &prog.setLayout;
    pipelineInfo.pushConstantRangeCount = iface.feedbackVaryings.empty() ? 1 : 2;
    pipelineInfo.pPushConstantRanges = pushes;
    return vkCreatePipelineLayout(dev, &pipelineInfo, nullptr, &prog.pipelineLayout) == VK_SUCCESS;
}

void WebGLVkContext::linkProgram(WebGLProgram p) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return;
    VkProgramResource& prog = it->second;
    // ES 3.0 2.15.2: not the program an active transform feedback captures with.
    for (const auto& [fid, f] : feedbacks_) {
        if (f.active && f.program == p.id) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
    }
    auto itV = shaders_.find(prog.vertShaderId);
    auto itF = shaders_.find(prog.fragShaderId);
    prog.validateStatus = false;  // until the next validateProgram

    auto fail = [&](std::string log) {
        releaseProgramExecutable(prog);
        prog.iface = ProgramInterface{};
        prog.linkStatus = false;
        prog.infoLog = std::move(log);
    };
    if (itV == shaders_.end() || itF == shaders_.end()) {
        fail("ERROR: a program needs a vertex and a fragment shader attached\n");
        return;
    }
    if (!itV->second.compileStatus || !itF->second.compileStatus) {
        fail("ERROR: an attached shader did not compile\n");
        return;
    }

    glsl::Limits limits;
    limits.maxVertexAttribs = static_cast<uint32_t>(getParameterInt(GL_MAX_VERTEX_ATTRIBS));
    limits.maxDrawBuffers = static_cast<uint32_t>(getParameterInt(GL_MAX_DRAW_BUFFERS));
    glsl::LinkResult linked = glsl::link(itV->second.source, itF->second.source, prog.boundAttribLocations, limits,
                                         prog.feedbackRequest);
    if (!linked.ok) {
        fail(std::move(linked.log));
        return;
    }
    // The resources the program declares must fit what getParameter reports.
    const ProgramInterface& li = linked.iface;
    const auto maxDefaultBlock = static_cast<uint32_t>(getParameterInt(GL_MAX_FRAGMENT_UNIFORM_VECTORS)) * 16u;
    if (li.defaultBlockSize > maxDefaultBlock) {
        fail("ERROR: the program's uniforms take " + std::to_string(li.defaultBlockSize / 16) +
             " vectors; MAX_*_UNIFORM_VECTORS is " + std::to_string(maxDefaultBlock / 16) + "\n");
        return;
    }
    if (li.vertexSamplerUnits > static_cast<uint32_t>(getParameterInt(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS))) {
        fail("ERROR: the vertex shader uses more samplers than MAX_VERTEX_TEXTURE_IMAGE_UNITS\n");
        return;
    }
    if (li.fragmentSamplerUnits > static_cast<uint32_t>(getParameterInt(GL_MAX_TEXTURE_IMAGE_UNITS))) {
        fail("ERROR: the fragment shader uses more samplers than MAX_TEXTURE_IMAGE_UNITS\n");
        return;
    }
    if (li.samplerUnitCount > static_cast<uint32_t>(getParameterInt(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS))) {
        fail("ERROR: the program uses more samplers than MAX_COMBINED_TEXTURE_IMAGE_UNITS\n");
        return;
    }
    // A portability device without mutableComparisonSamplers (an old
    // MoltenVK) cannot bind a depth-compare sampler, which every shadow
    // sampler reads through: such a program fails here rather than drawing
    // uncompared depth.
    if (!context_.comparisonSamplers()) {
        for (const VkSamplerBinding& s : li.samplers) {
            if (s.type == GL_SAMPLER_2D_SHADOW || s.type == GL_SAMPLER_CUBE_SHADOW ||
                s.type == GL_SAMPLER_2D_ARRAY_SHADOW) {
                fail("ERROR: this device has no depth-compare samplers, so no sampler*Shadow\n");
                return;
            }
        }
    }
    if (li.uniformBlocks.size() > static_cast<size_t>(getParameterInt(GL_MAX_COMBINED_UNIFORM_BLOCKS))) {
        fail("ERROR: the program uses more uniform blocks than MAX_COMBINED_UNIFORM_BLOCKS\n");
        return;
    }
    const auto maxBlockSize = static_cast<uint64_t>(getParameterInt64(GL_MAX_UNIFORM_BLOCK_SIZE));
    for (const VkUniformBlockInfo& b : li.uniformBlocks) {
        if (b.dataSize > maxBlockSize) {
            fail("ERROR: uniform block '" + b.name + "' is larger than MAX_UNIFORM_BLOCK_SIZE\n");
            return;
        }
    }

    releaseProgramExecutable(prog);
    VkDevice dev = context_.device();
    prog.iface = std::move(linked.iface);
    prog.vertModule = createShaderModule(dev, linked.vertSpirv);
    prog.fragModule = createShaderModule(dev, linked.fragSpirv);
    if (prog.vertModule == VK_NULL_HANDLE || prog.fragModule == VK_NULL_HANDLE || !buildProgramLayouts(prog)) {
        LOG_ERROR("WebGLVkContext: failed to create the Vulkan objects of program %u", p.id);
        fail("ERROR: the program's Vulkan objects could not be created\n");
        return;
    }
    prog.uniformBytes.assign(prog.iface.defaultBlockSize, 0);
    prog.samplerUnits.assign(prog.iface.samplerUnitCount, 0);
    prog.blockBindings.assign(prog.iface.uniformBlocks.size(), 0);
    prog.linkStatus = true;
    prog.infoLog = std::move(linked.log);
}

void WebGLVkContext::useProgram(WebGLProgram p) {
    if (feedbackCapturing()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (p.id != 0) {
        auto it = programs_.find(p.id);
        if (it == programs_.end() || !it->second.linkStatus) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
    }
    const GLuint previous = currentProgramId_;
    currentProgramId_ = p.id;
    if (previous != p.id) {
        auto it = programs_.find(previous);
        if (it != programs_.end() && it->second.deleteStatus) destroyProgram(previous);
    }
}

GLint WebGLVkContext::getProgramParameter(WebGLProgram p, GLenum pname) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return 0;
    const VkProgramResource& prog = it->second;
    switch (pname) {
        case GL_LINK_STATUS: return prog.linkStatus ? GL_TRUE : GL_FALSE;
        case GL_DELETE_STATUS: return prog.deleteStatus ? GL_TRUE : GL_FALSE;
        case GL_VALIDATE_STATUS: return prog.validateStatus ? GL_TRUE : GL_FALSE;
        case GL_ATTACHED_SHADERS: return (prog.vertShaderId != 0 ? 1 : 0) + (prog.fragShaderId != 0 ? 1 : 0);
        case GL_ACTIVE_ATTRIBUTES: return static_cast<GLint>(prog.iface.attribs.size());
        case GL_ACTIVE_UNIFORMS: return static_cast<GLint>(prog.iface.uniforms.size());
        case GL_ACTIVE_UNIFORM_BLOCKS: return static_cast<GLint>(prog.iface.uniformBlocks.size());
        case GL_TRANSFORM_FEEDBACK_VARYINGS: return static_cast<GLint>(prog.iface.feedbackVaryings.size());
        case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: return static_cast<GLint>(prog.iface.feedbackBufferMode);
        case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH: {
            size_t longest = 0;
            for (const VkFeedbackVarying& v : prog.iface.feedbackVaryings) longest = std::max(longest, v.name.size() + 1);
            return static_cast<GLint>(longest);
        }
        default:
            setSyntheticError(GL_INVALID_ENUM);
            return 0;
    }
}

std::string WebGLVkContext::getProgramInfoLog(WebGLProgram p) {
    auto it = programs_.find(p.id);
    return (it != programs_.end()) ? it->second.infoLog : "";
}

} // namespace bro::webgl::vk
