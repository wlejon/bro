#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_shaders.h"
#include "util/log.h"

#include <algorithm>

namespace bro::webgl::vk {

// ---------------------------------------------------------------------------
// Shaders & Programs
// ---------------------------------------------------------------------------

WebGLShader WebGLVkContext::createShader(GLenum type) {
    GLuint id = nextShaderId_++;
    shaders_[id] = VkShaderResource{type};
    return {id, type};
}

void WebGLVkContext::deleteShader(WebGLShader s) {
    auto it = shaders_.find(s.id);
    if (it != shaders_.end()) {
        it->second.deleteStatus = true;
        if (it->second.attachCount == 0) {
            if (it->second.module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(context_.device(), it->second.module, nullptr);
            }
            shaders_.erase(it);
        }
    }
}

void WebGLVkContext::shaderSource(WebGLShader s, const std::string& src) {
    if (shaders_.find(s.id) != shaders_.end()) {
        shaders_[s.id].source = src;
    }
}

void WebGLVkContext::compileShader(WebGLShader s) {
    auto it = shaders_.find(s.id);
    if (it == shaders_.end()) return;

    VkShaderResource& sh = it->second;
    TranslatedShader tr = WebGLVkShaderCompiler::translateToVulkanGLSL(sh.source, sh.type);
    sh.translatedSource = tr.source;

    VkShaderStageFlagBits stage = (sh.type == GL_VERTEX_SHADER)
        ? VK_SHADER_STAGE_VERTEX_BIT
        : VK_SHADER_STAGE_FRAGMENT_BIT;

    sh.spirv = WebGLVkShaderCompiler::compileToSpirv(sh.translatedSource, stage, &sh.infoLog);
    if (!sh.spirv.empty()) {
        sh.module = WebGLVkShaderCompiler::createShaderModule(context_.device(), sh.spirv);
        sh.compileStatus = (sh.module != VK_NULL_HANDLE);
    } else {
        sh.compileStatus = false;
    }
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
    GLuint id = nextProgramId_++;
    programs_[id] = VkProgramResource{};
    return {id};
}

void WebGLVkContext::deleteProgram(WebGLProgram p) {
    auto it = programs_.find(p.id);
    if (it != programs_.end()) {
        VkDevice dev = context_.device();
        pipelineCache_.evictShaders(it->second.vertModule, it->second.fragModule);
        if (it->second.vertModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, it->second.vertModule, nullptr);
            it->second.vertModule = VK_NULL_HANDLE;
        }
        if (it->second.fragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, it->second.fragModule, nullptr);
            it->second.fragModule = VK_NULL_HANDLE;
        }
        if (it->second.vertShaderId != 0) detachShader(p, {it->second.vertShaderId});
        if (it->second.fragShaderId != 0) detachShader(p, {it->second.fragShaderId});
        programs_.erase(it);
    }
    if (currentProgramId_ == p.id) currentProgramId_ = 0;
}

void WebGLVkContext::attachShader(WebGLProgram p, WebGLShader s) {
    auto itP = programs_.find(p.id);
    auto itS = shaders_.find(s.id);
    if (itP != programs_.end() && itS != shaders_.end()) {
        if (itS->second.type == GL_VERTEX_SHADER) itP->second.vertShaderId = s.id;
        else if (itS->second.type == GL_FRAGMENT_SHADER) itP->second.fragShaderId = s.id;
        itS->second.attachCount++;
    }
}

void WebGLVkContext::detachShader(WebGLProgram p, WebGLShader s) {
    auto it = programs_.find(p.id);
    if (it != programs_.end()) {
        GLuint sId = 0;
        if (it->second.vertShaderId == s.id) { sId = s.id; it->second.vertShaderId = 0; }
        if (it->second.fragShaderId == s.id) { sId = s.id; it->second.fragShaderId = 0; }
        if (sId != 0) {
            auto itS = shaders_.find(sId);
            if (itS != shaders_.end()) {
                if (itS->second.attachCount > 0) itS->second.attachCount--;
                if (itS->second.deleteStatus && itS->second.attachCount == 0) {
                    if (itS->second.module != VK_NULL_HANDLE) {
                        vkDestroyShaderModule(context_.device(), itS->second.module, nullptr);
                    }
                    shaders_.erase(itS);
                }
            }
        }
    }
}

void WebGLVkContext::linkProgram(WebGLProgram p) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return;

    VkProgramResource& prog = it->second;
    auto itV = shaders_.find(prog.vertShaderId);
    auto itF = shaders_.find(prog.fragShaderId);

    if (itV == shaders_.end() || itF == shaders_.end() ||
        !itV->second.compileStatus || !itF->second.compileStatus) {
        prog.linkStatus = false;
        prog.infoLog = "Attached shaders not compiled successfully";
        return;
    }

    ProgramLinkResult linkRes = WebGLVkShaderCompiler::linkShaders(
        itV->second.source, itF->second.source, prog.boundAttribLocations);

    if (!linkRes.success) {
        prog.linkStatus = false;
        prog.infoLog = linkRes.errorLog;
        return;
    }

    std::string vsLog, fsLog;
    auto vsSpirv = WebGLVkShaderCompiler::compileToSpirv(linkRes.vsVulkanSource, VK_SHADER_STAGE_VERTEX_BIT, &vsLog);
    auto fsSpirv = WebGLVkShaderCompiler::compileToSpirv(linkRes.fsVulkanSource, VK_SHADER_STAGE_FRAGMENT_BIT, &fsLog);

    if (vsSpirv.empty() || fsSpirv.empty()) {
        prog.linkStatus = false;
        prog.infoLog = "SPIR-V compilation error:\n" + vsLog + "\n" + fsLog;
        return;
    }

    VkDevice dev = context_.device();
    pipelineCache_.evictShaders(prog.vertModule, prog.fragModule);
    if (prog.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, prog.vertModule, nullptr);
        prog.vertModule = VK_NULL_HANDLE;
    }
    if (prog.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, prog.fragModule, nullptr);
        prog.fragModule = VK_NULL_HANDLE;
    }

    prog.vertModule = WebGLVkShaderCompiler::createShaderModule(dev, vsSpirv);
    prog.fragModule = WebGLVkShaderCompiler::createShaderModule(dev, fsSpirv);

    if (prog.vertModule == VK_NULL_HANDLE || prog.fragModule == VK_NULL_HANDLE) {
        prog.linkStatus = false;
        prog.infoLog = "Failed to create VkShaderModule";
        return;
    }

    prog.attribLocations = std::move(linkRes.attribLocations);
    prog.activeAttribs = std::move(linkRes.activeAttribs);
    prog.fragDataLocations = std::move(linkRes.fragDataLocations);
    prog.uniforms = std::move(linkRes.uniforms);
    prog.uniformLocations = std::move(linkRes.uniformLocations);
    prog.uniformBlockIndices = std::move(linkRes.uniformBlockIndices);
    prog.uniformBlocks = std::move(linkRes.uniformBlocks);

    prog.samplerLocToBinding.clear();
    prog.samplerBindings.clear();
    prog.samplerTypes.clear();
    for (const auto& [name, binding] : linkRes.samplerBindings) {
        auto locIt = prog.uniformLocations.find(name);
        if (locIt != prog.uniformLocations.end()) {
            prog.samplerLocToBinding[locIt->second] = binding;
            prog.samplerBindings[binding] = 0; // default to texture unit 0
            for (const auto& u : prog.uniforms) {
                if (u.location == locIt->second) {
                    prog.samplerTypes[binding] = u.type;
                    break;
                }
            }
        }
    }

    prog.uniformBytes.assign(std::max(16u, linkRes.defaultUniformSize), 0);
    prog.drawFrameSerial = 0;
    prog.linkStatus = true;
    prog.infoLog = "";
}

void WebGLVkContext::useProgram(WebGLProgram p) {
    currentProgramId_ = p.id;
}

GLint WebGLVkContext::getProgramParameter(WebGLProgram p, GLenum pname) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return 0;
    if (pname == GL_LINK_STATUS) return it->second.linkStatus ? GL_TRUE : GL_FALSE;
    if (pname == 0x8B80 /* GL_DELETE_STATUS */) return it->second.deleteStatus ? GL_TRUE : GL_FALSE;
    if (pname == GL_ATTACHED_SHADERS) {
        int count = 0;
        if (it->second.vertShaderId != 0) count++;
        if (it->second.fragShaderId != 0) count++;
        return count;
    }
    if (pname == 0x8B89 /* GL_ACTIVE_ATTRIBUTES */) return static_cast<GLint>(it->second.activeAttribs.size());
    if (pname == 0x8B86 /* GL_ACTIVE_UNIFORMS */) return static_cast<GLint>(it->second.uniforms.size());
    if (pname == 0x8A36 /* GL_ACTIVE_UNIFORM_BLOCKS */) return static_cast<GLint>(it->second.uniformBlocks.size());
    return 0;
}

std::string WebGLVkContext::getProgramInfoLog(WebGLProgram p) {
    auto it = programs_.find(p.id);
    return (it != programs_.end()) ? it->second.infoLog : "";
}

} // namespace bro::webgl::vk
