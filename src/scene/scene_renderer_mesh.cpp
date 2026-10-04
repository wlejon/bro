#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "webgl/webgl_types.h"
#include "scene/skinned_mesh_node.h"
#include "canvas/canvas_scene.h"
#include "scene/vulkan/scene_vk_custom_shader.h"
#include "util/log.h"

namespace bro::scene {

void SceneRenderer::queryShadeLocs(GLuint /*prog*/, ShadeLocs& /*s*/) {
}

void SceneRenderer::uploadShadeMapForDraw(const ShadeMapProvider* /*provider*/,
                                          const ShadeLocs& /*L*/, int& /*cachedHas*/) {
}

void SceneRenderer::queryMeshUniformLocs(GLuint /*prog*/, MeshDrawLocs& /*d*/,
                                         MeshProgramLocs& /*l*/) {
}

void SceneRenderer::ensureMeshPipeline() {
}

void SceneRenderer::ensureSkinnedMeshPipeline() {
}

bool SceneRenderer::compileCustomShader(CustomShaderTarget target,
                                        const std::string& /*key*/,
                                        const std::string& vertexChunk,
                                        const std::string& fragmentChunk,
                                        std::string& errOut) {
    if (defaultVulkanContext_) {
        return vk::SceneVkCustomShader::validateCustomShader(target, vertexChunk, fragmentChunk, errOut);
    }
    errOut = "custom shaders require GPU rendering (no Vulkan context)";
    return false;
}

SceneRenderer::CustomProgramEntry* SceneRenderer::ensureCustomProgram(
        CustomShaderTarget /*target*/, const std::string& /*key*/,
        const std::string& /*vertexChunk*/, const std::string& /*fragmentChunk*/,
        std::string* /*errOut*/) {
    return nullptr;
}

void SceneRenderer::uploadUserUniforms(
        GLuint /*prog*/, std::unordered_map<std::string, GLint>& /*cache*/,
        const CustomShaderState* /*st*/) {
}

void SceneRenderer::uploadUserTextures(
        GLuint /*prog*/, std::unordered_map<std::string, GLint>& /*cache*/,
        MeshNode* /*mesh*/) {
}

void SceneRenderer::renderMeshNode(MeshNode* /*mesh*/, const MeshDrawLocs& /*L*/) {
}

}  // namespace bro::scene
