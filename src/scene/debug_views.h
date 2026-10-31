#pragma once

// Debug views (SceneRenderer::setDebugView): named views a custom shader
// module answers. A module tests for one with SCENE_DEBUG(name), which is
// replaced with a test of the camera's view id against the name's id here,
// and outputs a value in `debug` (mesh.bk, volume.bk). The ids are the
// process's, given in the order names are first met; 0 is no view.

#include <string>
#include <vector>

namespace bro::scene::debug_views {

/// The id of `name`, given it on first use (thread-safe).
int idOf(const std::string& name);
/// The id of `name`, or 0 when no chunk has named it.
int find(const std::string& name);
/// The name of view `id` ("" for 0 or one never given).
std::string nameOf(int id);
/// Every name given so far, in id order.
std::vector<std::string> names();

/// `source` with each SCENE_DEBUG(name) replaced by a test of the camera's
/// view id (`viewExpr`: scene::debug_view() in a brokernel module, scene.bk)
/// against name's.
std::string expand(const std::string& source, const std::string& viewExpr = "scene::debug_view()");

} // namespace bro::scene::debug_views
