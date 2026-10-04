#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/reflection_probe_node.h"
#include "scene/skinned_mesh_node.h"
#include "util/log.h"

namespace bro::scene {

void SceneRenderer::queryProbeLocs(GLuint /*prog*/, ProbeLocs& /*p*/) {
}

void SceneRenderer::collectFrameProbes() {
}

void SceneRenderer::uploadProbeForDraw(SceneNode* /*node*/, const ProbeLocs& /*P*/) {
}

void SceneRenderer::updateReflectionProbes(const std::vector<LightNode*>& /*lights*/) {
}

void SceneRenderer::renderProbeSceneOpaque(const std::vector<LightNode*>& /*lights*/) {
}

}  // namespace bro::scene
