#pragma once

// The scene's meshes in the HDR scope, from this frame's draw lists
// (scene_draw_list.h), drawn by SceneMeshDrawer:
//
//   PassOpaque       every opaque mesh. Always runs, so the frame's first
//                    HDR scope (which clears) always exists. Carries the
//                    indirect-light attachment while SSAO is on.
//   PassTranslucent  alpha-blended meshes, far to near, after decals.
//   PassOverlay      after tonemapping, into the LDR frame: the unlit meshes
//                    (so their authored colours are not tonemapped), depth
//                    tested against the scene without writing it, alpha
//                    blended, unfogged; then the editor's transform handles
//                    (SceneGraph's gizmo provider) on top of everything.
//                    Handles that stop being provided have their GPU meshes
//                    released.

#include "scene/vulkan/scene_pass.h"

#include <cstdint>
#include <vector>

namespace bro::scene::vk {

class PassOpaque final : public ScenePass {
public:
    const char* name() const override { return "opaque"; }
    bool setup(SceneGpu&) override { return true; }
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

class PassTranslucent final : public ScenePass {
public:
    const char* name() const override { return "translucent"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

class PassOverlay final : public ScenePass {
public:
    const char* name() const override { return "overlay"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    std::vector<uint32_t> gizmoIds_;   // the handles drawn last frame, sorted
};

}  // namespace bro::scene::vk
