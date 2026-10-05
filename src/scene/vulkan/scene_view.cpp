#include "scene/vulkan/scene_view.h"

#include "scene/depth_policy.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"

#include <cstring>

namespace bro::scene::vk {

bromath::Mat4 toVulkanClip(const bromath::Mat4& proj) {
    bromath::Mat4 out = proj;
    for (int c = 0; c < 4; ++c) out.at(1, c) = -out.at(1, c);
    return out;
}

SceneView SceneView::make(const bromath::Mat4& view, const bromath::Mat4& projection,
                          const bromath::Vec3& eye, float nearZ, float farZ,
                          bool perspective, uint32_t width, uint32_t height) {
    SceneView v;
    v.view = view;
    v.proj = toVulkanClip(projection);
    v.viewProj = bromath::mmul(v.proj, view);
    v.invView = bromath::minverse(view);
    v.invProj = bromath::minverse(v.proj);
    v.invViewProj = bromath::minverse(v.viewProj);
    v.frustum = makeFrustum(v.viewProj);
    v.eye = eye;
    v.nearZ = nearZ;
    v.farZ = farZ;
    v.perspective = perspective;
    v.width = width;
    v.height = height;
    return v;
}

SceneView SceneView::fromCamera(const SceneGraph& graph, uint32_t width, uint32_t height) {
    return make(graph.viewMatrix(), graph.projectionMatrix(), graph.cameraEye(),
                graph.cameraNearZ(), graph.cameraFarZ(), graph.cameraIsPerspective(), width, height);
}

SceneCameraUniforms SceneView::uniforms(const SceneRenderer* fog) const {
    SceneCameraUniforms cam{};
    std::memcpy(cam.view, view.data, sizeof(cam.view));
    std::memcpy(cam.proj, proj.data, sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.data, sizeof(cam.viewProj));
    std::memcpy(cam.invView, invView.data, sizeof(cam.invView));
    std::memcpy(cam.invProj, invProj.data, sizeof(cam.invProj));
    cam.eyePos[0] = eye.x;
    cam.eyePos[1] = eye.y;
    cam.eyePos[2] = eye.z;
    cam.viewport[0] = static_cast<float>(width);
    cam.viewport[1] = static_cast<float>(height);
    cam.viewport[2] = nearZ;
    cam.viewport[3] = farZ;
    if (fog) {
        cam.fogParams[0] = fog->fogStart();
        cam.fogParams[1] = fog->fogEnd();
        cam.fogParams[2] = fog->fogDensity();
        cam.fogParams[3] = fog->fogStartDist();
        cam.fogColor[0] = fog->fogColor()[0];
        cam.fogColor[1] = fog->fogColor()[1];
        cam.fogColor[2] = fog->fogColor()[2];
        cam.fogColor[3] = fog->fogHeightFalloff();
    }
    return cam;
}

}  // namespace bro::scene::vk
