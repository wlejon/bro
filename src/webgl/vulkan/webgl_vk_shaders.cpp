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

uint32_t alignTo(uint32_t offset, uint32_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

std::pair<uint32_t, uint32_t> getUniformSizeAndAlign(const std::string& type) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool") {
        return {4, 4};
    } else if (type == "vec2" || type == "ivec2" || type == "uvec2" || type == "bvec2") {
        return {8, 8};
    } else if (type == "vec3" || type == "ivec3" || type == "uvec3" || type == "bvec3") {
        return {12, 16};
    } else if (type == "vec4" || type == "ivec4" || type == "uvec4" || type == "bvec4") {
        return {16, 16};
    } else if (type == "mat2") {
        return {32, 16}; // 2 vec4s in std140
    } else if (type == "mat3") {
        return {48, 16}; // 3 vec4s in std140
    } else if (type == "mat4") {
        return {64, 16}; // 4 vec4s in std140
    }
    return {16, 16};
}

GLenum typeStringToGLenum(const std::string& type) {
    if (type == "float") return GL_FLOAT;
    if (type == "vec2") return GL_FLOAT_VEC2;
    if (type == "vec3") return GL_FLOAT_VEC3;
    if (type == "vec4") return GL_FLOAT_VEC4;
    if (type == "int") return GL_INT;
    if (type == "ivec2") return GL_INT_VEC2;
    if (type == "ivec3") return GL_INT_VEC3;
    if (type == "ivec4") return GL_INT_VEC4;
    if (type == "bool") return GL_BOOL;
    if (type == "mat2") return GL_FLOAT_MAT2;
    if (type == "mat3") return GL_FLOAT_MAT3;
    if (type == "mat4") return GL_FLOAT_MAT4;
    if (type == "sampler2D") return GL_SAMPLER_2D;
    if (type == "samplerCube") return GL_SAMPLER_CUBE;
    return GL_FLOAT;
}

} // namespace

std::unordered_map<std::string, std::vector<uint32_t>> WebGLVkShaderCompiler::s_spirvCache;

TranslatedShader WebGLVkShaderCompiler::translateToVulkanGLSL(const std::string& glslSource, GLenum shaderType) {
    TranslatedShader result;
    std::istringstream input(glslSource);
    std::ostringstream output;
    std::string line;

    bool isVertex = (shaderType == GL_VERTEX_SHADER);
    bool hasVersion = (glslSource.find("#version") != std::string::npos);

    // Prepend Vulkan GLSL #version 450
    output << "#version 450\n";

    if (!isVertex) {
        output << "#define texture2D texture\n";
        output << "#define textureCube texture\n";
    }

    uint32_t nextAttrLoc = 0;
    uint32_t nextVaryingLoc = 0;
    uint32_t nextSamplerBinding = 0;

    std::vector<std::string> pushConstantMembers;
    uint32_t currentOffset = 0;
    int nextUniformLoc = 0;

    bool hasFragColorOut = false;

    while (std::getline(input, line)) {
        // Strip #version line if present in source
        if (line.find("#version") != std::string::npos) {
            continue;
        }

        // Strip standalone precision statements
        {
            auto trimmed = line;
            auto pos = trimmed.find_first_not_of(" \t");
            if (pos != std::string::npos) trimmed = trimmed.substr(pos);
            if (trimmed.rfind("precision ", 0) == 0 && trimmed.find(';') != std::string::npos) {
                continue;
            }
        }

        // Strip inline precision qualifiers: highp, mediump, lowp
        {
            std::string stripped;
            stripped.reserve(line.size());
            size_t i = 0;
            while (i < line.size()) {
                bool replaced = false;
                for (const char* q : {"highp ", "mediump ", "lowp "}) {
                    size_t len = strlen(q);
                    if (line.compare(i, len, q) == 0) {
                        if (i == 0 || line[i-1] == ' ' || line[i-1] == '\t' ||
                            line[i-1] == '(' || line[i-1] == ',') {
                            i += len;
                            replaced = true;
                            break;
                        }
                    }
                }
                if (!replaced) {
                    stripped += line[i++];
                }
            }
            line = stripped;
        }

        // Check for attributes / inputs: attribute / in
        std::regex attrRegex(R"(^\s*(?:attribute|in)\s+(\w+)\s+(\w+)\s*;)");
        std::smatch match;
        if (isVertex && std::regex_search(line, match, attrRegex)) {
            std::string type = match[1].str();
            std::string name = match[2].str();
            uint32_t loc = nextAttrLoc++;
            result.attributeLocations[name] = loc;
            output << "layout(location = " << loc << ") in " << type << " " << name << ";\n";
            continue;
        }

        // Check for varyings
        std::regex varyingRegex(R"(^\s*varying\s+(\w+)\s+(\w+)\s*;)");
        if (std::regex_search(line, match, varyingRegex)) {
            std::string type = match[1].str();
            std::string name = match[2].str();
            uint32_t loc = nextVaryingLoc++;
            if (isVertex) {
                output << "layout(location = " << loc << ") out " << type << " " << name << ";\n";
            } else {
                output << "layout(location = " << loc << ") in " << type << " " << name << ";\n";
            }
            continue;
        }

        // Check for WebGL2 vertex out or fragment in: out/in without layout
        if (isVertex) {
            std::regex outRegex(R"(^\s*out\s+(\w+)\s+(\w+)\s*;)");
            if (std::regex_search(line, match, outRegex)) {
                std::string type = match[1].str();
                std::string name = match[2].str();
                uint32_t loc = nextVaryingLoc++;
                output << "layout(location = " << loc << ") out " << type << " " << name << ";\n";
                continue;
            }
        } else {
            std::regex inRegex(R"(^\s*in\s+(\w+)\s+(\w+)\s*;)");
            if (std::regex_search(line, match, inRegex)) {
                std::string type = match[1].str();
                std::string name = match[2].str();
                uint32_t loc = nextVaryingLoc++;
                output << "layout(location = " << loc << ") in " << type << " " << name << ";\n";
                continue;
            }

            std::regex fragOutRegex(R"(^\s*out\s+(\w+)\s+(\w+)\s*;)");
            if (std::regex_search(line, match, fragOutRegex)) {
                std::string type = match[1].str();
                std::string name = match[2].str();
                output << "layout(location = 0) out " << type << " " << name << ";\n";
                hasFragColorOut = true;
                continue;
            }
        }

        // Check for uniforms
        std::regex uniformRegex(R"(^\s*uniform\s+(\w+)\s+(\w+)\s*;)");
        if (std::regex_search(line, match, uniformRegex)) {
            std::string type = match[1].str();
            std::string name = match[2].str();

            if (type == "sampler2D" || type == "samplerCube") {
                uint32_t binding = nextSamplerBinding++;
                result.samplerBindings[name] = binding;
                output << "layout(binding = " << binding << ") uniform " << type << " " << name << ";\n";

                VkUniformInfo uinfo;
                uinfo.name = name;
                uinfo.location = nextUniformLoc++;
                uinfo.type = typeStringToGLenum(type);
                uinfo.offset = 0;
                uinfo.size = 0;
                uinfo.count = 1;
                result.uniforms.push_back(uinfo);
            } else {
                auto [size, align] = getUniformSizeAndAlign(type);
                currentOffset = alignTo(currentOffset, align);

                VkUniformInfo uinfo;
                uinfo.name = name;
                uinfo.location = nextUniformLoc++;
                uinfo.type = typeStringToGLenum(type);
                uinfo.offset = currentOffset;
                uinfo.size = size;
                uinfo.count = 1;
                result.uniforms.push_back(uinfo);

                pushConstantMembers.push_back("    " + type + " " + name + ";\n");
                currentOffset += size;
            }
            continue;
        }

        output << line << "\n";
    }

    if (!isVertex && !hasFragColorOut) {
        // ES 1.00 gl_FragColor support
        output << "layout(location = 0) out vec4 bro_FragColor;\n";
        output << "#define gl_FragColor bro_FragColor\n";
    }

    // Insert Push Constants uniform block if any non-sampler uniforms exist
    if (!pushConstantMembers.empty()) {
        currentOffset = alignTo(currentOffset, 16);
        result.pushConstantSize = currentOffset;

        std::string uniformBlock = "layout(push_constant) uniform WebGLUniforms {\n";
        for (const auto& member : pushConstantMembers) {
            uniformBlock += member;
        }
        uniformBlock += "};\n";

        // Insert uniform block right after #version
        std::string fullSource = output.str();
        size_t verPos = fullSource.find('\n');
        if (verPos != std::string::npos) {
            fullSource.insert(verPos + 1, uniformBlock);
        } else {
            fullSource = uniformBlock + fullSource;
        }
        result.source = fullSource;
    } else {
        result.source = output.str();
    }

    return result;
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
