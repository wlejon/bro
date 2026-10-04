#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace bro::scene {

void SceneRenderer::queryInstancedUniformLocs(GLuint /*prog*/, InstancedDrawLocs& /*d*/,
                                              MeshProgramLocs& /*l*/) {
}

void SceneRenderer::ensureInstancedMeshPipeline() {
}

void SceneRenderer::ensureFoliageScatterPipeline() {
}

void SceneRenderer::ensureTubePipeline() {
}

void SceneRenderer::ensureTubeDepthPipeline() {
}

void SceneRenderer::renderInstancedMeshNode(InstancedMeshNode* /*mesh*/,
                                            const InstancedDrawLocs& /*L*/) {
}

}  // namespace bro::scene
