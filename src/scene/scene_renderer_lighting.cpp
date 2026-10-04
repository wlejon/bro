#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include <algorithm>
#include <vector>

namespace bro::scene {

void SceneRenderer::collectLights(std::vector<LightNode*>& out) const {
    out.clear();
    for (auto& [id, node] : graph_.nodes_) {
        if (!node->renderVisible()) continue;
        if (node->type() != SceneNode::Type::Light) continue;
        SceneNode* p = node.get();
        while (p && p->parent()) p = p->parent();
        if (p != graph_.root_.get()) continue;
        out.push_back(static_cast<LightNode*>(node.get()));
        if (out.size() >= 32) break;
    }
}

void SceneRenderer::uploadLights(const std::vector<LightNode*>& /*lights*/,
                                 const MeshProgramLocs& /*locs*/) {
}

}  // namespace bro::scene
