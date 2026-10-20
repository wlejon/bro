#pragma once

#include "scene/scene_node.h"

namespace bro::scene {

/// Camera scene node (Godot Camera3D analog). Carries only PROJECTION
/// parameters — the VIEW comes from the node's world transform: the camera
/// looks down its local -Z with local +Y up, so parenting a camera under a
/// vehicle or character makes it inherit that motion like any other node
/// (use SceneNode::lookAt to aim it).
///
/// Activate with SceneGraph::setActiveCamera(). While active, the graph
/// derives its view + projection matrices from this node's world matrix once
/// per tick (right after animations/tweens, so the audio listener and JS see
/// the fresh view) and again at the top of render() (so transforms mutated
/// from JS between tick and render are honored) — a tweened or parented
/// camera is therefore frame-accurate with no per-frame JS. Every consumer of
/// camera state (CSM cascade fitting, frustum culling, billboards, soft
/// particles, DoF, skybox, unproject/picking, the bound audio listener)
/// reads those derived matrices, so all of them follow the active camera.
///
/// Precedence: the LAST camera call wins. setActiveCamera() overrides the
/// imperative view; any subsequent SceneGraph::setCamera*/setCameraQuat/
/// setCameraOrtho call deactivates the active camera node and installs the
/// imperative view. Destroying the active node keeps the last derived view
/// and reads back as "no active camera".
///
/// Aspect: aspect() <= 0 (the default) makes the projection follow the
/// canvas aspect through resizes, mirroring setCamera's omitted-aspect
/// behavior; setAspect(>0) pins it for fixed-aspect / cinematic cameras.
class CameraNode : public SceneNode {
public:
    explicit CameraNode(const std::string& name = "") : SceneNode(name) {}

    Type type() const override { return Type::Camera; }

    /// Perspective (default) vs orthographic projection.
    bool perspective() const { return perspective_; }
    void setPerspective(bool p) { perspective_ = p; }

    /// Vertical field of view in radians (perspective mode).
    float fovRadians() const { return fovY_; }
    void setFovRadians(float rad) { if (rad > 0.0f) fovY_ = rad; }

    /// Vertical field of view in degrees (perspective mode).
    float fovDegrees() const { return fovY_ * (180.0f / 3.14159265358979323846f); }
    void setFovDegrees(float deg) { if (deg > 0.0f) fovY_ = deg * (3.14159265358979323846f / 180.0f); }

    /// Alias for vertical FOV in radians.
    float fovY() const { return fovRadians(); }
    void setFovY(float f) { setFovRadians(f); }

    /// Full view height in world units (orthographic mode); width follows
    /// the aspect. Matches setCamera's `size` option.
    float orthoHeight() const { return orthoHeight_; }
    void setOrthoHeight(float h) { if (h > 0.0f) orthoHeight_ = h; }

    float nearZ() const { return nearZ_; }
    float farZ() const { return farZ_; }
    void setNearZ(float n) { if (n > 0.0f) nearZ_ = n; }
    void setFarZ(float f) { if (f > 0.0f) farZ_ = f; }

    /// Explicit width/height aspect; <= 0 (default) follows the canvas.
    float aspect() const { return aspect_; }
    void setAspect(float a) { aspect_ = a; }

private:
    bool perspective_ = true;
    float fovY_ = 1.0471976f;      // 60 degrees
    float orthoHeight_ = 10.0f;
    float nearZ_ = 0.1f;
    float farZ_ = 1000.0f;
    float aspect_ = 0.0f;          // <= 0 -> follow canvas
};

} // namespace bro::scene
