#pragma once

#include "scene/scene_node.h"

#include <cstdint>

namespace bro::scene {

/// Local reflection probe (Godot ReflectionProbe analog). The probe volume is
/// the unit box [-0.5, 0.5]^3 in LOCAL space — the node's scale IS the box
/// size, matching the DecalNode convention — and the capture origin is the
/// node's world position (the box center).
///
/// Capturing renders the scene 6 times (cube faces, 90° FOV) from the origin
/// into an HDR cubemap with a mip chain (the GL renderer GGX-prefiltered the
/// chain; the Vulkan probe pass box-filters it for now). There is deliberately
/// NO per-frame auto mode — a capture costs 6 restricted scene renders plus a
/// prefilter, so probes update 'once' (on the first frame the probe is
/// visible) or 'manual' (only when requestCapture() / JS capture() asks).
///
/// Application is per-draw: a mesh whose bounds center lies inside the box
/// samples this probe's prefiltered chain instead of the global IBL specular,
/// parallax-corrected against the box when boxProjection is on, fading back
/// to the global environment over the `interior` margin near the box faces.
/// Specular only — diffuse ambient stays global (irradiance or flat ambient),
/// which is also Godot's ReflectionProbe default. The node holds settings and
/// capture state only; the cubemaps belong to the renderer's probe pass
/// (vulkan/pass_reflection_probe.*), keyed by node id. Captures draw opaque
/// geometry + skybox only (no SSR/probes/post).
class ReflectionProbeNode : public SceneNode {
public:
    explicit ReflectionProbeNode(const std::string& name = "");

    Type type() const override { return Type::ReflectionProbe; }

    enum class UpdateMode : uint8_t { Once, Manual };

    /// 'Once' (default): capture on the first rendered frame the probe is
    /// visible. 'Manual': never capture until requestCapture(). Switching to
    /// Once re-arms a capture if none has happened yet; switching to Manual
    /// cancels any pending automatic capture (call requestCapture() to ask
    /// explicitly).
    void setUpdateMode(UpdateMode m) {
        updateMode_ = m;
        if (m == UpdateMode::Once) {
            if (!hasData_) pendingCapture_ = true;
        } else {
            pendingCapture_ = false;
        }
    }
    UpdateMode updateMode() const { return updateMode_; }

    /// Cube face size in texels. Clamped to a power of two in [16, 1024];
    /// takes effect on the NEXT capture (the cubemap reallocates then).
    void setResolution(int r);
    int resolution() const { return resolution_; }

    /// Parallax-correct sampling against the box volume (default true — the
    /// point of a local probe). Sampling-time only; no recapture needed.
    void setBoxProjection(bool on) { boxProjection_ = on; }
    bool boxProjection() const { return boxProjection_; }

    /// Multiplier on the probe's specular contribution (default 1).
    void setIntensity(float i) { intensity_ = i < 0.0f ? 0.0f : i; }
    float intensity() const { return intensity_; }

    /// Interior blend margin in world units: within this distance of a box
    /// face the probe fades back to the global IBL. 0 (default) = hard edge.
    void setInterior(float m) { interior_ = m < 0.0f ? 0.0f : m; }
    float interior() const { return interior_; }

    /// Selection priority. A mesh binds the highest-priority probe whose box
    /// contains its bounds center; ties go to the smallest volume (the more
    /// local probe).
    void setPriority(int p) { priority_ = p; }
    int priority() const { return priority_; }

    /// Request a (re)capture on the next rendered frame the probe is visible.
    void requestCapture() { pendingCapture_ = true; }
    bool captureRequested() const { return pendingCapture_; }
    /// Renderer-side: drop a pending request after a failed capture so it
    /// doesn't retry (and re-log) every frame.
    void clearCaptureRequest() { pendingCapture_ = false; }

    /// True once a capture has completed — only then does the probe apply.
    bool hasData() const { return hasData_; }

    /// Called by the renderer after a successful capture.
    void markCaptured() {
        pendingCapture_ = false;
        hasData_ = true;
    }

private:
    UpdateMode updateMode_ = UpdateMode::Once;
    int resolution_ = 128;
    bool boxProjection_ = true;
    float intensity_ = 1.0f;
    float interior_ = 0.0f;
    int priority_ = 0;

    bool pendingCapture_ = true;   // Once mode arms on construction
    bool hasData_ = false;
};

} // namespace bro::scene
