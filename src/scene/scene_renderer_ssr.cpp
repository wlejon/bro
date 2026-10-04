#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "util/log.h"

namespace bro::scene {

void SceneRenderer::ensureSSRPipeline() {
}

void SceneRenderer::ensureSSRFBO() {
}

void SceneRenderer::destroySSRFBO() {
    ssrSourceTex_ = 0;
    ssrFBO_ = 0;
    ssrWidth_ = ssrHeight_ = 0;
}

void SceneRenderer::runSSRPass() {
}

}  // namespace bro::scene
