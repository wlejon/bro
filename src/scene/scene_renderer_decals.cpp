#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/decal_node.h"
#include "util/log.h"

namespace bro::scene {

void SceneRenderer::ensureDecalPipeline() {
}

void SceneRenderer::renderDecalPass(const std::vector<LightNode*>& /*lights*/) {
}

}  // namespace bro::scene
