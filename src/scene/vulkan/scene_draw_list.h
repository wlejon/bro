#pragma once

// What the frame draws, gathered once before any pass runs: the nodes shown
// (visible with every ancestor, attached to the root, in tree order — what
// every node-gathering pass iterates), every shown mesh prepared as a MeshDraw (camera-culled ones included, flagged — the
// shadow and probe passes still draw them), split into the opaque and
// back-to-front translucent lists and the unlit overlay (unlit meshes draw
// after tonemapping, so their authored colours come out exactly).

#include "scene/vulkan/scene_mesh_drawer.h"

#include <vector>

namespace bro::scene {
class SceneNode;
}

namespace bro::scene::vk {

struct SceneFrame;

struct SceneDrawLists {
    std::vector<SceneNode*> nodes;         // shown nodes, tree (pre)order
    std::vector<MeshDraw> meshes;          // every prepared mesh, in tree order
    std::vector<uint32_t> opaque;          // indices into meshes (not culled)
    std::vector<uint32_t> translucent;     // indices, far to near (not culled)
    std::vector<uint32_t> overlay;         // unlit meshes, tree order (not culled)
};

/// Fill frame.lists from the graph and count the forward-pass culling stats.
void buildDrawLists(SceneFrame& frame);

}  // namespace bro::scene::vk
