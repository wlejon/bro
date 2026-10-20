#include "scene/vulkan/frame/scene_view.h"

#include "scene/depth_policy.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

bromath::Mat4 toVulkanClip(const bromath::Mat4& proj) {
    bromath::Mat4 out = proj;
    for (int c = 0; c < 4; ++c) out.at(1, c) = -out.at(1, c);
    return out;
}

bromath::Mat4 eyeRelative(const bromath::Mat4& world, const bromath::Vec3& eye) {
    bromath::Mat4 out = world;
    out.at(0, 3) -= eye.x;
    out.at(1, 3) -= eye.y;
    out.at(2, 3) -= eye.z;
    return out;
}

bromath::Mat4 rebased(const bromath::Mat4& m, const bromath::Vec3& offset) {
    if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) return m;
    return bromath::mmul(m, bromath::mtranslate(offset));
}

SceneView SceneView::make(const bromath::Mat4& view, const bromath::Mat4& projection,
                          const bromath::Vec3& eye, float nearZ, float farZ,
                          bool perspective, uint32_t width, uint32_t height) {
    SceneView v;
    v.view = view;
    v.proj = toVulkanClip(projection);
    v.viewProj = bromath::mmul(v.proj, view);
    v.frustum = makeFrustum(v.viewProj);
    // The eye maps to the view origin, so view * T(eye) is the view with its
    // translation dropped — built that way rather than by multiplying, which
    // would round the large -R*eye term back in.
    v.relView = view;
    v.relView.at(0, 3) = 0.0f;
    v.relView.at(1, 3) = 0.0f;
    v.relView.at(2, 3) = 0.0f;
    v.relViewProj = bromath::mmul(v.proj, v.relView);
    v.invRelView = bromath::minverse(v.relView);
    v.invProj = bromath::minverse(v.proj);
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
    std::memcpy(cam.view, relView.data, sizeof(cam.view));
    std::memcpy(cam.proj, proj.data, sizeof(cam.proj));
    std::memcpy(cam.viewProj, relViewProj.data, sizeof(cam.viewProj));
    std::memcpy(cam.invView, invRelView.data, sizeof(cam.invView));
    std::memcpy(cam.invProj, invProj.data, sizeof(cam.invProj));
    cam.eyeWorld[0] = eye.x;
    cam.eyeWorld[1] = eye.y;
    cam.eyeWorld[2] = eye.z;
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
        const float* windDir = fog->windDir();
        cam.wind[0] = windDir[0];
        cam.wind[1] = windDir[1];
        cam.wind[2] = windDir[2];
        cam.wind[3] = fog->windStrength();
        cam.windParams[0] = fog->windTime();
        cam.windParams[1] = fog->windFrequency();
        // The eye's share of the sway phase (windDelta in scene_camera.glsl),
        // reduced in double so a far eye does not round the phase away.
        const double phase = 0.3 * static_cast<double>(eye.x) + 0.5 * static_cast<double>(eye.z);
        cam.windParams[2] = static_cast<float>(std::fmod(phase, 2.0 * 3.14159265358979323846));
        cam.windParams[3] = fog->windClock();
        const WindFieldState wf = fog->windField();
        cam.windField[0] = wf.dirX;
        cam.windField[1] = wf.dirZ;
        cam.windField[2] = wf.base;
        cam.windField[3] = wf.gustSpeed;
        cam.windGust[0] = wf.gust;
        cam.windGust[1] = wf.invAlong;
        cam.windGust[2] = wf.invAcross;

        // The plant movers: each one's trail, ages on the wind clock.
        static_assert(PlantMovers::kMovers * 2 <= 8 && PlantMovers::kMovers * PlantMovers::kTrail <= 80,
                      "the camera block's plant mover arrays");
        const PlantMovers& pm = fog->plantMovers();
        const double now = fog->windTimeExact();
        const double span = PlantMovers::kTrail * PlantMovers::kStep;
        int n = 0;
        for (const PlantMovers::Track& t : pm.tracks()) {
            if (n >= PlantMovers::kMovers) break;
            const float cx = t.body.x - eye.x, cz = t.body.z - eye.z;
            float reach = 0.0f;
            int k = 0;
            for (const PlantMovers::Sample& s : t.trail) {
                const double age = now - s.time;
                if (age < 0.0 || age >= span) continue;
                float* o = cam.plantTrail[n * PlantMovers::kTrail + k++];
                o[0] = s.x - eye.x;
                o[1] = s.z - eye.z;
                o[2] = static_cast<float>(age);
                const double u = std::clamp((age - 0.6 * span) / (0.4 * span), 0.0, 1.0);
                o[3] = static_cast<float>(1.0 - u * u * (3.0 - 2.0 * u));
                reach = std::max(reach, std::hypot(o[0] - cx, o[1] - cz));
            }
            for (; k < PlantMovers::kTrail; ++k) cam.plantTrail[n * PlantMovers::kTrail + k][3] = 0.0f;
            float* a = cam.plantMovers[n * 2];
            a[0] = cx;
            a[1] = cz;
            a[2] = reach + 1.6f * t.body.radius;   // scene_vegetation.glsl's VEG_MOVER_REACH
            a[3] = t.body.radius;
            float* b = cam.plantMovers[n * 2 + 1];
            b[0] = t.body.height;
            b[1] = t.body.strength;
            ++n;
        }
        cam.plantMoverInfo[0] = static_cast<float>(n);
        cam.plantMoverInfo[1] = static_cast<float>(PlantMovers::kTrail);
    }
    return cam;
}

}  // namespace bro::scene::vk
