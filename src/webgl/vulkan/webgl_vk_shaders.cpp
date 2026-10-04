#include "webgl/vulkan/webgl_vk_shaders.h"
#include "util/log.h"

#include <sstream>
#include <regex>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <mutex>

namespace bro::webgl::vk {

namespace {

std::mutex s_compilerMutex;

} // namespace

std::unordered_map<std::string, std::vector<uint32_t>> WebGLVkShaderCompiler::s_spirvCache;

TranslatedShader WebGLVkShaderCompiler::translateToVulkanGLSL(const std::string& glslSource, GLenum shaderType) {
    ParsedShader parsed = WebGLVkShaderParser::parse(glslSource, shaderType);
    TranslatedShader result;
    result.source = WebGLVkShaderParser::generateStandaloneVulkanGLSL(parsed);

    int nextLoc = 0;
    for (const auto& attr : parsed.attributes) {
        int loc = (attr.location >= 0) ? attr.location : nextLoc;
        nextLoc = std::max(nextLoc, loc + 1);
        result.attributeLocations[attr.name] = loc;
    }
    int nextBinding = 0;
    for (const auto& s : parsed.samplers) {
        result.samplerBindings[s.name] = nextBinding++;
    }
    uint32_t currentOffset = 0;
    int nextUniLoc = 0;
    for (const auto& u : parsed.uniforms) {
        auto [baseSize, align] = WebGLVkShaderParser::getUniformSizeAndAlign(u.type);
        uint32_t totalSize = u.isArray ? (u.arraySize * WebGLVkShaderParser::alignTo(baseSize, 16)) : baseSize;
        currentOffset = WebGLVkShaderParser::alignTo(currentOffset, align);
        VkUniformInfo info;
        info.name = u.name + (u.isArray ? "[0]" : "");
        info.location = nextUniLoc++;
        info.type = WebGLVkShaderParser::typeStringToGLenum(u.type);
        info.offset = currentOffset;
        info.size = totalSize;
        info.count = u.arraySize;
        result.uniforms.push_back(info);
        currentOffset += totalSize;
    }
    result.pushConstantSize = WebGLVkShaderParser::alignTo(currentOffset, 16);
    return result;
}

ProgramLinkResult WebGLVkShaderCompiler::linkShaders(const std::string& vsSource, const std::string& fsSource,
                                                     const std::unordered_map<std::string, GLuint>& boundAttribs)
{
    ParsedShader vsParsed = WebGLVkShaderParser::parse(vsSource, GL_VERTEX_SHADER);
    ParsedShader fsParsed = WebGLVkShaderParser::parse(fsSource, GL_FRAGMENT_SHADER);
    return WebGLVkShaderParser::linkAndGenerateVulkanGLSL(vsParsed, fsParsed, boundAttribs);
}

std::vector<uint32_t> WebGLVkShaderCompiler::compileToSpirv(const std::string& source,
                                                            VkShaderStageFlagBits stage,
                                                            std::string* outLog)
{
    std::lock_guard<std::mutex> lock(s_compilerMutex);

    std::string stageStr = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? "vertex" : "fragment";
    std::string cacheKey = stageStr + "|" + source;

    auto it = s_spirvCache.find(cacheKey);
    if (it != s_spirvCache.end()) {
        return it->second;
    }

    int inPipe[2];
    int outPipe[2];
    int errPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        LOG_ERROR("WebGLVkShaderCompiler: pipe() failed");
        return {};
    }

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR("WebGLVkShaderCompiler: fork() failed");
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return {};
    }

    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);

        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);

        std::string stageArg = "-fshader-stage=" + stageStr;
        execlp("glslc", "glslc", stageArg.c_str(), "-", "-o", "-", nullptr);
        _exit(127);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);

    // Write source to glslc stdin
    size_t totalWritten = 0;
    while (totalWritten < source.size()) {
        ssize_t written = write(inPipe[1], source.data() + totalWritten, source.size() - totalWritten);
        if (written <= 0) break;
        totalWritten += written;
    }
    close(inPipe[1]);

    // Read SPIR-V binary from stdout
    std::vector<uint8_t> spvBytes;
    uint8_t buf[4096];
    ssize_t bytesRead = 0;
    while ((bytesRead = read(outPipe[0], buf, sizeof(buf))) > 0) {
        spvBytes.insert(spvBytes.end(), buf, buf + bytesRead);
    }
    close(outPipe[0]);

    // Read compiler error/warnings from stderr
    std::string errStr;
    char errBuf[1024];
    ssize_t errBytesRead = 0;
    while ((errBytesRead = read(errPipe[0], errBuf, sizeof(errBuf))) > 0) {
        errStr.append(errBuf, errBytesRead);
    }
    close(errPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !spvBytes.empty()) {
        std::vector<uint32_t> spirv(spvBytes.size() / 4);
        std::memcpy(spirv.data(), spvBytes.data(), spvBytes.size());
        s_spirvCache[cacheKey] = spirv;
        return spirv;
    }

    if (outLog) {
        *outLog = errStr;
    }
    LOG_ERROR("WebGLVkShaderCompiler: Compilation failed for %s shader:\n%s\nErrors:\n%s",
              stageStr.c_str(), source.c_str(), errStr.c_str());
    return {};
}

VkShaderModule WebGLVkShaderCompiler::createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    if (device == VK_NULL_HANDLE || spirv.empty()) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(uint32_t);
    createInfo.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkShaderCompiler: Failed to create VkShaderModule");
        return VK_NULL_HANDLE;
    }
    return module;
}

void WebGLVkShaderCompiler::destroyShaderModule(VkDevice device, VkShaderModule module) {
    if (device != VK_NULL_HANDLE && module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
}

} // namespace bro::webgl::vk
