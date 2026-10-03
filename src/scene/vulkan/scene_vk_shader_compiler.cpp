#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "util/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>


namespace bro::scene::vk {

namespace {

static const uint32_t kSpvMeshVert[] =
#include "mesh.vert.spv.h"
;
static const uint32_t kSpvMeshInstancedVert[] =
#include "mesh_instanced.vert.spv.h"
;
static const uint32_t kSpvMeshSkinnedVert[] =
#include "mesh_skinned.vert.spv.h"
;
static const uint32_t kSpvMeshFrag[] =
#include "mesh.frag.spv.h"
;
static const uint32_t kSpvShadowVert[] =
#include "shadow.vert.spv.h"
;
static const uint32_t kSpvShadowInstancedVert[] =
#include "shadow_instanced.vert.spv.h"
;
static const uint32_t kSpvShadowSkinnedVert[] =
#include "shadow_skinned.vert.spv.h"
;
static const uint32_t kSpvShadowFrag[] =
#include "shadow.frag.spv.h"
;
static const uint32_t kSpvSkyboxVert[] =
#include "skybox.vert.spv.h"
;
static const uint32_t kSpvEnvironmentFrag[] =
#include "environment.frag.spv.h"
;
static const uint32_t kSpvPostFxVert[] =
#include "postfx.vert.spv.h"
;
static const uint32_t kSpvTonemapFrag[] =
#include "tonemap.frag.spv.h"
;
static const uint32_t kSpvBloomFrag[] =
#include "bloom.frag.spv.h"
;
static const uint32_t kSpvFxaaFrag[] =
#include "fxaa.frag.spv.h"
;

std::mutex s_compilerMutex;

template<size_t N>
std::vector<uint32_t> arrayToVector(const uint32_t (&arr)[N]) {
    return std::vector<uint32_t>(arr, arr + N);
}

} // namespace

std::unordered_map<std::string, std::vector<uint32_t>> SceneVkShaderCompiler::s_compileCache;

bool SceneVkShaderCompiler::hasGlslc() {
    static int cached = -1;
    if (cached != -1) return cached == 1;
    int res = std::system("which glslc > /dev/null 2>&1");
    cached = (res == 0) ? 1 : 0;
    return cached == 1;
}

std::vector<uint32_t> SceneVkShaderCompiler::compileGlsl(
    const std::string& glslSource,
    VkShaderStageFlagBits stage,
    const std::string& entryPoint,
    const std::vector<std::string>& defines) {

    std::lock_guard<std::mutex> lock(s_compilerMutex);

    std::string stageStr;
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:   stageStr = "vertex"; break;
        case VK_SHADER_STAGE_FRAGMENT_BIT: stageStr = "fragment"; break;
        case VK_SHADER_STAGE_COMPUTE_BIT:  stageStr = "compute"; break;
        default:
            LOG_ERROR("SceneVkShaderCompiler: Unsupported shader stage");
            return {};
    }

    std::ostringstream cacheKeyStream;
    cacheKeyStream << stageStr << "|" << entryPoint << "|";
    for (const auto& d : defines) cacheKeyStream << d << ";";
    cacheKeyStream << "|" << glslSource;
    std::string cacheKey = cacheKeyStream.str();

    auto it = s_compileCache.find(cacheKey);
    if (it != s_compileCache.end()) {
        return it->second;
    }

    if (!hasGlslc()) {
        LOG_ERROR("SceneVkShaderCompiler: glslc not found on system PATH");
        return {};
    }

    int inPipe[2];
    int outPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) {
        LOG_ERROR("SceneVkShaderCompiler: pipe() failed");
        return {};
    }

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR("SceneVkShaderCompiler: fork() failed");
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        return {};
    }

    if (pid == 0) {
        // Child process
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);

        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);

        std::vector<const char*> args;
        args.push_back("glslc");
        std::string stageArg = "-fshader-stage=" + stageStr;
        args.push_back(stageArg.c_str());
        std::string entryArg = "-fentry-point=" + entryPoint;
        args.push_back(entryArg.c_str());

        std::vector<std::string> defineArgs;
        for (const auto& d : defines) {
            defineArgs.push_back("-D" + d);
            args.push_back(defineArgs.back().c_str());
        }

        args.push_back("-");     // read GLSL from stdin
        args.push_back("-o");
        args.push_back("-");     // write SPIR-V to stdout
        args.push_back(nullptr);

        execvp("glslc", const_cast<char* const*>(args.data()));
        _exit(127);
    }

    // Parent process
    close(inPipe[0]);
    close(outPipe[1]);

    // Feed GLSL to child stdin
    size_t totalWritten = 0;
    while (totalWritten < glslSource.size()) {
        ssize_t written = write(inPipe[1], glslSource.data() + totalWritten,
                                glslSource.size() - totalWritten);
        if (written <= 0) break;
        totalWritten += static_cast<size_t>(written);
    }
    close(inPipe[1]);

    // Read SPIR-V binary from child stdout
    std::vector<uint8_t> output;
    uint8_t buffer[4096];
    ssize_t bytesRead = 0;
    while ((bytesRead = read(outPipe[0], buffer, sizeof(buffer))) > 0) {
        output.insert(output.end(), buffer, buffer + bytesRead);
    }
    close(outPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && output.size() >= 4 && (output.size() % 4 == 0)) {
        size_t wordCount = output.size() / 4;
        std::vector<uint32_t> spirv(wordCount);
        std::memcpy(spirv.data(), output.data(), output.size());

        if (spirv[0] == 0x07230203) {
            s_compileCache[cacheKey] = spirv;
            return spirv;
        } else {
            LOG_ERROR("SceneVkShaderCompiler: Invalid SPIR-V magic number");
        }
    } else {
        LOG_ERROR("SceneVkShaderCompiler: glslc execution failed (status=%d)", status);
    }

    return {};
}

const std::vector<uint32_t>& SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader shader) {
    static const std::vector<uint32_t> s_meshVert = arrayToVector(kSpvMeshVert);
    static const std::vector<uint32_t> s_meshInstVert = arrayToVector(kSpvMeshInstancedVert);
    static const std::vector<uint32_t> s_meshSkinnedVert = arrayToVector(kSpvMeshSkinnedVert);
    static const std::vector<uint32_t> s_meshFrag = arrayToVector(kSpvMeshFrag);
    static const std::vector<uint32_t> s_shadowVert = arrayToVector(kSpvShadowVert);
    static const std::vector<uint32_t> s_shadowInstVert = arrayToVector(kSpvShadowInstancedVert);
    static const std::vector<uint32_t> s_shadowSkinnedVert = arrayToVector(kSpvShadowSkinnedVert);
    static const std::vector<uint32_t> s_shadowFrag = arrayToVector(kSpvShadowFrag);
    static const std::vector<uint32_t> s_skyboxVert = arrayToVector(kSpvSkyboxVert);
    static const std::vector<uint32_t> s_envFrag = arrayToVector(kSpvEnvironmentFrag);
    static const std::vector<uint32_t> s_postFxVert = arrayToVector(kSpvPostFxVert);
    static const std::vector<uint32_t> s_tonemapFrag = arrayToVector(kSpvTonemapFrag);
    static const std::vector<uint32_t> s_bloomFrag = arrayToVector(kSpvBloomFrag);
    static const std::vector<uint32_t> s_fxaaFrag = arrayToVector(kSpvFxaaFrag);
    static const std::vector<uint32_t> s_empty;

    switch (shader) {
        case BuiltinSceneShader::MeshVert:          return s_meshVert;
        case BuiltinSceneShader::MeshInstancedVert: return s_meshInstVert;
        case BuiltinSceneShader::MeshSkinnedVert:   return s_meshSkinnedVert;
        case BuiltinSceneShader::MeshFrag:          return s_meshFrag;
        case BuiltinSceneShader::ShadowVert:        return s_shadowVert;
        case BuiltinSceneShader::ShadowInstancedVert: return s_shadowInstVert;
        case BuiltinSceneShader::ShadowSkinnedVert: return s_shadowSkinnedVert;
        case BuiltinSceneShader::ShadowFrag:        return s_shadowFrag;
        case BuiltinSceneShader::SkyboxVert:        return s_skyboxVert;
        case BuiltinSceneShader::EnvironmentFrag:   return s_envFrag;
        case BuiltinSceneShader::PostFxVert:        return s_postFxVert;
        case BuiltinSceneShader::TonemapFrag:       return s_tonemapFrag;
        case BuiltinSceneShader::BloomFrag:         return s_bloomFrag;
        case BuiltinSceneShader::FxaaFrag:          return s_fxaaFrag;
        default: return s_empty;
    }
}

VkShaderModule SceneVkShaderCompiler::createBuiltinModule(VkDevice device, BuiltinSceneShader shader) {
    const auto& spirv = getBuiltinSpirv(shader);
    if (spirv.empty()) return VK_NULL_HANDLE;
    return createModule(device, spirv);
}

VkShaderModule SceneVkShaderCompiler::createModule(VkDevice device, const uint32_t* code, size_t sizeBytes) {
    return SceneVkShaderModule::create(device, code, sizeBytes);
}

VkShaderModule SceneVkShaderCompiler::createModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    return SceneVkShaderModule::create(device, spirv);
}

void SceneVkShaderCompiler::destroyModule(VkDevice device, VkShaderModule module) {
    SceneVkShaderModule::destroy(device, module);
}

} // namespace bro::scene::vk
