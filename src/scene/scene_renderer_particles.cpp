#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/particles3d_node.h"
#include "util/log.h"

namespace bro::scene {

void SceneRenderer::ensureParticlePipeline() {
}

void SceneRenderer::ensureSceneDepthCopy() {
}

void SceneRenderer::destroySceneDepthCopy() {
    sceneDepthCopyTex_ = 0;
    sceneDepthCopyFBO_ = 0;
    sceneDepthCopyWidth_ = sceneDepthCopyHeight_ = 0;
}

void SceneRenderer::renderParticles3DNodes() {
}

}  // namespace bro::scene
