// native_scene_camera.cpp — Camera setup, math, view/projection matrices, and raycast native bindings.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"
#include <string_view>

namespace bro::bronze_host {

namespace {

static thread_local double tl_mat4Buf[16];
static thread_local double tl_vec3Buf[3];

bool isOrthoMode(bool given, const char* mode) {
    if (!given || !mode) return false;
    std::string_view m(mode);
    return m == "orthographic" || m == "ortho";
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

double bro_scene_SceneGraph_cameraX_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->cameraX() : 0.0;
}

void bro_scene_SceneGraph_cameraX_set(void* self, double v) {
    auto* g = graphOf(self);
    if (g) g->setCameraPosition(static_cast<float>(v), g->cameraY());
}

double bro_scene_SceneGraph_cameraY_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->cameraY() : 0.0;
}

void bro_scene_SceneGraph_cameraY_set(void* self, double v) {
    auto* g = graphOf(self);
    if (g) g->setCameraPosition(g->cameraX(), static_cast<float>(v));
}

double bro_scene_SceneGraph_cameraZoom_get(void* self) {
    auto* g = graphOf(self);
    return g ? g->cameraZoom() : 1.0;
}

void bro_scene_SceneGraph_cameraZoom_set(void* self, double v) {
    auto* g = graphOf(self);
    if (g) g->setCameraZoom(static_cast<float>(v));
}

void* bro_scene_SceneGraph_activeCamera_get(void* self) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* cam = g->activeCamera();
    return cam ? wrapNode(cam, g) : nullptr;
}

void bro_scene_SceneGraph_activeCamera_set(void* self, void* v) {
    auto* g = graphOf(self);
    auto* cam = nodeOf(v);
    if (g && cam && cam->type() == scene::SceneNode::Type::Camera) {
        g->setActiveCamera(static_cast<scene::CameraNode*>(cam));
    } else if (g && !v) {
        g->setActiveCamera(nullptr);
    }
}

void bro_scene_SceneGraph_viewMatrix_get(void* self, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!g) { copyBuffer<double>(nullptr, 0, out); return; }
    const auto& m = g->viewMatrix();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            tl_mat4Buf[col * 4 + row] = m.at(row, col);
        }
    }
    copyBuffer(tl_mat4Buf, 16, out);
}

void bro_scene_SceneGraph_projectionMatrix_get(void* self, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!g) { copyBuffer<double>(nullptr, 0, out); return; }
    const auto& m = g->projectionMatrix();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            tl_mat4Buf[col * 4 + row] = m.at(row, col);
        }
    }
    copyBuffer(tl_mat4Buf, 16, out);
}

void bro_scene_SceneGraph_cameraEye_get(void* self, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!g) { copyBuffer<double>(nullptr, 0, out); return; }
    const auto& e = g->cameraEye();
    tl_vec3Buf[0] = e.x;
    tl_vec3Buf[1] = e.y;
    tl_vec3Buf[2] = e.z;
    copyBuffer(tl_vec3Buf, 3, out);
}

// setCamera({fov, near, far, aspect, eye|position, target|lookAt, up,
//            quaternion, mode, size}) — the imperative view. quaternion
// wins over target/up/mode; mode "ortho[graphic]" uses size (view height)
// in place of fov.
void bro_scene_SceneGraph_setCamera(void* self, bool opts_fov_given, double opts_fov,
                                   bool opts_near_given, double opts_near,
                                   bool opts_far_given, double opts_far,
                                   const double* opts_eye, uint32_t opts_eye_len,
                                   const double* opts_target, uint32_t opts_target_len,
                                   const double* opts_up, uint32_t opts_up_len,
                                   bool opts_aspect_given, double opts_aspect,
                                   const double* opts_quaternion, uint32_t opts_quaternion_len,
                                   bool opts_mode_given, const char* opts_mode,
                                   bool opts_size_given, double opts_size) {
    auto* g = graphOf(self);
    if (!g) return;
    float fov = opts_fov_given ? static_cast<float>(opts_fov * 3.141592653589793 / 180.0) : (60.0f * 3.141592653589793f / 180.0f);
    float nearP = opts_near_given ? static_cast<float>(opts_near) : 0.1f;
    float farP = opts_far_given ? static_cast<float>(opts_far) : 1000.0f;
    bromath::Vec3 eye{0.0f, 5.0f, -10.0f};
    bromath::Vec3 target{0.0f, 0.0f, 0.0f};
    bromath::Vec3 up{0.0f, 1.0f, 0.0f};
    if (opts_eye && opts_eye_len >= 3) {
        eye = {static_cast<float>(opts_eye[0]), static_cast<float>(opts_eye[1]), static_cast<float>(opts_eye[2])};
    }
    if (opts_target && opts_target_len >= 3) {
        target = {static_cast<float>(opts_target[0]), static_cast<float>(opts_target[1]), static_cast<float>(opts_target[2])};
    }
    if (opts_up && opts_up_len >= 3) {
        up = {static_cast<float>(opts_up[0]), static_cast<float>(opts_up[1]), static_cast<float>(opts_up[2])};
    }

    // Aspect omitted (or <= 0) → derive from the current canvas and flag the
    // projection to auto-follow future canvas resizes (setCanvasSize rebuilds
    // it). An explicit aspect pins the projection and disables the follow.
    float aspect = opts_aspect_given ? static_cast<float>(opts_aspect) : 0.0f;
    const bool aspectFollowsCanvas = aspect <= 0.0f;
    if (aspectFollowsCanvas) {
        int cw = g->canvasWidth(), ch = g->canvasHeight();
        aspect = (cw > 0 && ch > 0) ? static_cast<float>(cw) / static_cast<float>(ch) : (4.0f / 3.0f);
    }
    g->setCameraAspectFollowsCanvas(aspectFollowsCanvas);

    if (opts_quaternion && opts_quaternion_len >= 4) {
        bromath::Quat q(static_cast<float>(opts_quaternion[0]), static_cast<float>(opts_quaternion[1]),
                        static_cast<float>(opts_quaternion[2]), static_cast<float>(opts_quaternion[3]));
        g->setCameraQuat(fov, aspect, nearP, farP, eye, bromath::qnorm(q));
    } else if (isOrthoMode(opts_mode_given, opts_mode)) {
        const float size = opts_size_given ? static_cast<float>(opts_size) : 10.0f;
        const float halfW = size * aspect * 0.5f;
        const float halfH = size * 0.5f;
        g->setCameraOrtho(-halfW, halfW, -halfH, halfH, nearP, farP, eye, target, up);
    } else {
        g->setCamera(fov, aspect, nearP, farP, eye, target, up);
    }
}

// createCamera({name, fov, near, far, aspect, mode, size, position|eye,
//               quaternion, lookAt|target, up, active}) → CameraNode. The
// node's WORLD transform is the view; only projection params live on it.
void* bro_scene_SceneGraph_createCamera(void* self, bool opts_fov_given, double opts_fov,
                                        bool opts_near_given, double opts_near,
                                        bool opts_far_given, double opts_far,
                                        const double* opts_eye, uint32_t opts_eye_len,
                                        const double* opts_target, uint32_t opts_target_len,
                                        const double* opts_up, uint32_t opts_up_len,
                                        bool opts_aspect_given, double opts_aspect,
                                        const double* opts_quaternion, uint32_t opts_quaternion_len,
                                        bool opts_mode_given, const char* opts_mode,
                                        bool opts_size_given, double opts_size) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* cam = g->createCamera();
    g->root()->addChild(cam);
    if (opts_fov_given) cam->setFovY(static_cast<float>(opts_fov * 3.14159265 / 180.0));
    if (opts_near_given) cam->setNearZ(static_cast<float>(opts_near));
    if (opts_far_given) cam->setFarZ(static_cast<float>(opts_far));
    if (opts_aspect_given) cam->setAspect(static_cast<float>(opts_aspect));
    if (opts_size_given) cam->setOrthoHeight(static_cast<float>(opts_size));
    if (opts_mode_given) cam->setPerspective(!isOrthoMode(opts_mode_given, opts_mode));
    if (opts_eye && opts_eye_len >= 3) {
        cam->setPosition(static_cast<float>(opts_eye[0]), static_cast<float>(opts_eye[1]), static_cast<float>(opts_eye[2]));
    }
    if (opts_quaternion && opts_quaternion_len >= 4) {
        bromath::Quat q(static_cast<float>(opts_quaternion[0]), static_cast<float>(opts_quaternion[1]),
                        static_cast<float>(opts_quaternion[2]), static_cast<float>(opts_quaternion[3]));
        cam->setRotation(bromath::qnorm(q));
    } else if (opts_target && opts_target_len >= 3) {
        bromath::Vec3 tgt{static_cast<float>(opts_target[0]), static_cast<float>(opts_target[1]), static_cast<float>(opts_target[2])};
        bromath::Vec3 up{0, 1, 0};
        if (opts_up && opts_up_len >= 3) up = {static_cast<float>(opts_up[0]), static_cast<float>(opts_up[1]), static_cast<float>(opts_up[2])};
        cam->lookAt(tgt, up);
    }
    return wrapNode(cam, g);
}

void bro_scene_SceneGraph_setActiveCamera(void* self, void* camera) {
    auto* g = graphOf(self);
    auto* n = nodeOf(camera);
    auto* cam = (n && n->type() == scene::SceneNode::Type::Camera) ? static_cast<scene::CameraNode*>(n) : nullptr;
    if (g) g->setActiveCamera(cam);
}

void bro_scene_SceneGraph_clearActiveCamera(void* self) {
    auto* g = graphOf(self);
    if (g) g->setActiveCamera(nullptr);
}

}  // extern "C"

namespace bro::bronze_host {

bool registerSceneCameraNatives(std::string* error) {
    using namespace natives;
    return fn("__bro_native.scene.SceneGraph_clearActiveCamera", (void*)&bro_scene_SceneGraph_clearActiveCamera, "void", {"__bro_native.scene.SceneGraph"}, error) &&
           fn("__bro_native.scene.SceneGraph_raycast_instance", (void*)&bro_scene_SceneGraph_raycast_instance, "i32", {}, error);
}

}  // namespace bro::bronze_host
