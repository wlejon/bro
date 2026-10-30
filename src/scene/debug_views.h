#pragma once

// Debug views (SceneRenderer::setDebugView): named views a custom shader
// chunk answers. A chunk tests for one with SCENE_DEBUG(name), which the
// splice (scene_vk_custom_shader.cpp) replaces with a test of the camera's
// view id against the name's id here, and shows a value with
// sceneDebugOut(rgb) (mesh.frag, volume.frag). The ids are the process's,
// given in the order names are first met; 0 is no view.

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
/// view id (sceneDebugView(), scene_camera.glsl) against name's.
std::string expand(const std::string& source);

} // namespace bro::scene::debug_views
