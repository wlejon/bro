#pragma once

#include <bromath/vec.h>
#include <bromath/quat.h>
#include <bromath/mat.h>

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace bro::scene {

/// Base class for all scene graph nodes.
/// Provides hierarchical transforms (position, rotation, scale) with
/// cached world matrix and dirty propagation.
class SceneNode {
public:
    explicit SceneNode(const std::string& name = "");
    virtual ~SceneNode();

    SceneNode(const SceneNode&) = delete;
    SceneNode& operator=(const SceneNode&) = delete;

    // --- Identity ---
    uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    void setName(const std::string& n) { name_ = n; }

    // --- Transform (local space) ---
    const bromath::Vec3& position() const { return position_; }
    const bromath::Quat& rotation() const { return rotation_; }
    const bromath::Vec3& scale() const { return scale_; }

    void setPosition(float x, float y, float z = 0);
    void setPosition(const bromath::Vec3& pos);
    void setRotation(const bromath::Quat& q);
    /// Convenience: set rotation from Euler angles (radians).
    void setRotationEuler(float rx, float ry, float rz);
    /// Convenience: set rotation around Z axis (2D rotation).
    void setRotationZ(float radians);
    void setScale(float sx, float sy, float sz = 1);
    void setScale(const bromath::Vec3& s);
    /// Set 2D scale (sx, sy), preserving current scale.z.
    void setScale2D(float sx, float sy);
    /// Partial-array scale assignment (e.g. setting scale [x, y] preserves z).
    void setScalePartial(const float* values, size_t count);

    // --- Visibility ---
    bool visible() const { return visible_; }
    void setVisible(bool v) {
        if (v != visible_) { visible_ = v; ++changeGeneration_; }
    }

    // --- Visibility range (camera-distance gate) ---
    // Optional world-space distance window: while set, the node only renders
    // when begin <= d < end, where d is the distance from the camera eye to
    // the node's world origin (worldMatrix translation). `margin` adds
    // hysteresis around both edges — once shown the node stays shown until d
    // leaves [begin - margin, end + margin); once hidden it stays hidden
    // until d enters [begin + margin, end - margin) — so a camera hovering
    // exactly on a boundary doesn't flicker. margin 0 (default) is a hard
    // switch. Evaluated once per frame by SceneGraph::updateVisibilityGates()
    // (render-time only: raycast/picking of 3D geometry is NOT gated).
    //
    // The gate NEVER mutates the user-visible `visible` flag — the two are
    // independent and every render walk checks renderVisible(), the AND of
    // both. Like `visible`, a closed gate prunes the walk at this node, so
    // the subtree is gated with it (children with their own ranges evaluate
    // them relative to their own world origins while the parent is shown).
    bool hasVisibilityRange() const { return hasVisibilityRange_; }
    void setVisibilityRange(float begin, float end, float margin = 0.0f) {
        hasVisibilityRange_ = true;
        rangeBegin_ = begin;
        rangeEnd_ = end;
        rangeMargin_ = margin < 0.0f ? 0.0f : margin;
        // Re-evaluated (with hysteresis from the current state) next frame.
    }
    void clearVisibilityRange() {
        hasVisibilityRange_ = false;
        rangeGateOpen_ = true;
    }
    float visibilityRangeBegin() const { return rangeBegin_; }
    float visibilityRangeEnd() const { return rangeEnd_; }
    float visibilityRangeMargin() const { return rangeMargin_; }

    /// Current distance-gate state (true when no range is set).
    bool rangeGateOpen() const { return rangeGateOpen_; }

    /// What every render/cull path checks: the user flag AND the distance
    /// gate. Both are required; neither writes the other.
    bool renderVisible() const { return visible_ && rangeGateOpen_; }

    /// Per-frame gate evaluation with hysteresis (see setVisibilityRange).
    /// Called by SceneGraph::updateVisibilityGates() with this frame's camera
    /// distance; no-op when no range is set.
    void updateRangeGate(float d) {
        if (!hasVisibilityRange_) return;
        const float m = rangeMargin_;
        if (rangeGateOpen_) {
            if (d < rangeBegin_ - m || d >= rangeEnd_ + m) rangeGateOpen_ = false;
        } else {
            if (d >= rangeBegin_ + m && d < rangeEnd_ - m) rangeGateOpen_ = true;
        }
    }

    // --- Change generation ---
    /// Monotonic per-node mutation counter. Bumped by every transform set
    /// (markDirty, including parent-transform propagation), visibility
    /// toggle, and content mutation in subclasses (mesh swap, instance
    /// updates, custom-shader install). Consumers snapshot (id, generation)
    /// pairs and compare later: a mismatch means "this node may render
    /// differently now" — the shadow-tile cache keys on it. Never reset.
    uint64_t changeGeneration() const { return changeGeneration_; }
    /// For subclasses/owners mutating renderable content outside the
    /// transform setters (e.g. setMesh, setInstances).
    void bumpChangeGeneration() { ++changeGeneration_; }

    // --- Hierarchy ---
    SceneNode* parent() const { return parent_; }
    const std::vector<SceneNode*>& children() const { return children_; }

    void addChild(SceneNode* child);
    void removeChild(SceneNode* child);
    void removeFromParent();

    // --- World transform ---
    const bromath::Mat4& localMatrix() const;
    const bromath::Mat4& worldMatrix() const;

    /// Convert a point from local space to world space.
    bromath::Vec3 localToWorld(const bromath::Vec3& local) const;

    /// Orient this node so its local -Z axis points from its world position
    /// toward `worldTarget` with local +Y as close to `up` as possible — the
    /// camera convention (CameraNode looks down -Z), also handy for spot
    /// rigs and turrets. Writes the LOCAL rotation, compensating for
    /// ancestor rotations by composing rotation() up the parent chain
    /// (exact for pure TRS hierarchies; ancestor non-uniform scale is
    /// ignored). No-op when the target coincides with the node position.
    void lookAt(const bromath::Vec3& worldTarget,
                const bromath::Vec3& up = {0, 1, 0});

    /// Traverse this node and all descendants depth-first.
    void traverse(const std::function<void(SceneNode*)>& fn);

    /// Mark this node (and descendants) as needing world matrix recomputation.
    void markDirty();
    bool isDirty() const { return dirty_; }

    // --- Rendering hook ---
    /// Called by SceneGraph during render traversal. Override in renderable nodes.
    virtual void onRender(class SceneGraph& graph) {}

    /// Called by SceneGraph::tickAnimations(dt) once per engine frame, before
    /// JS callbacks run, on every node in the tree (independent of visibility).
    /// Override in nodes that need time-based simulation (sprite frame
    /// advance, particle integration, etc.). Default: no-op.
    virtual void onTick(float /*dtSec*/) {}

    // --- Type tag for downcasting ---
    enum class Type : uint8_t { Base, Shape, Sprite, Physics, Mesh, Html, Light, Particles, InstancedMesh, GaussianSplat, Particles3D, Camera, Decal, ReflectionProbe };
    virtual Type type() const { return Type::Base; }

    // --- World anchor + billboard (Shape/Sprite/Html) ---
    // When hasWorldAnchor is true, the node renders as a camera-facing billboard
    // in the mesh FBO (depth-tested against 3D geometry) instead of the 2D canvas.
    enum class BillboardMode : uint8_t { Full, YLock };

    bool hasWorldAnchor() const { return hasWorldAnchor_; }
    const bromath::Vec3& worldAnchor() const { return worldAnchor_; }
    void setWorldAnchor(const bromath::Vec3& a) { worldAnchor_ = a; hasWorldAnchor_ = true; }
    void clearWorldAnchor() { hasWorldAnchor_ = false; }

    BillboardMode billboardMode() const { return billboardMode_; }
    void setBillboardMode(BillboardMode m) { billboardMode_ = m; }

    // --- JS wrapper cache ---
    // Opaque handle to the JS object wrapping this node, so every crossing
    // hands back the SAME object and `===`, Set, Map and indexOf behave. The
    // reference is BORROWED, never owned: the wrapper's finalizer clears it,
    // so a non-null value is always a live object. Storing it here rather than
    // in a side table keeps the lookup free and dies naturally with the node.
    // Deliberately a void* — the scene layer knows nothing about QuickJS.
    void* jsWrapper() const { return jsWrapper_; }
    void setJsWrapper(void* w) { jsWrapper_ = w; }

private:
    void updateLocalMatrix() const;
    void updateWorldMatrix() const;

    void* jsWrapper_ = nullptr;
    uint32_t id_;
    std::string name_;

    bromath::Vec3 position_;
    bromath::Quat rotation_;
    bromath::Vec3 scale_{1, 1, 1};
    bool visible_ = true;
    uint64_t changeGeneration_ = 1;

    // Visibility-range gate (see setVisibilityRange). rangeGateOpen_ is the
    // hysteresis state updated once per frame; true whenever no range is set.
    bool hasVisibilityRange_ = false;
    bool rangeGateOpen_ = true;
    float rangeBegin_ = 0.0f;
    float rangeEnd_ = 0.0f;
    float rangeMargin_ = 0.0f;

    SceneNode* parent_ = nullptr;
    std::vector<SceneNode*> children_;

    mutable bromath::Mat4 localMatrix_;
    mutable bromath::Mat4 worldMatrix_;
    mutable bool localDirty_ = true;
    mutable bool dirty_ = true;

    bromath::Vec3 worldAnchor_;
    bool hasWorldAnchor_ = false;
    BillboardMode billboardMode_ = BillboardMode::Full;

    static uint32_t s_nextId;
};

} // namespace bro::scene
