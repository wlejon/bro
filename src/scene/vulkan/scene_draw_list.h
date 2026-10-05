#pragma once

// What the frame draws, gathered once before any pass runs: every visible
// mesh prepared as a MeshDraw (camera-culled ones included, flagged — the
// shadow and probe passes still draw them), split into the opaque and
// back-to-front translucent lists and the unlit overlay (unlit meshes draw
// after tonemapping, so their authored colours come out exactly), plus the
// clipmap terrains, which draw through their own pipeline.

#include "scene/vulkan/scene_mesh_drawer.h"

#include <vector>

namespace bro::scene {
class MeshNode;
}

namespace bro::scene::vk {

struct SceneFrame;

struct SceneDrawLists {
    std::vector<MeshDraw> meshes;          // every prepared mesh, in graph order
    std::vector<uint32_t> opaque;          // indices into meshes (not culled)
    std::vector<uint32_t> translucent;     // indices, far to near (not culled)
    std::vector<uint32_t> overlay;         // unlit meshes, graph order (not culled)
    std::vector<MeshNode*> terrains;       // clipmap terrains (not culled)
};

/// Fill frame.lists from the graph and count the forward-pass culling stats.
void buildDrawLists(SceneFrame& frame);

}  // namespace bro::scene::vk
