#pragma once

#include "scene/scene_node.h"

#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <quickjs.h>
}

namespace bro::scene { class SceneGraph; class MeshNode; }

namespace bro::engine {

/// Gizmo modification mode — which handle set to render and pick against.
enum class GizmoMode : uint8_t {
    Translate = 0,
    Rotate    = 1,
    Scale     = 2,
};

/// Axis / handle identifier returned from picking.
enum class GizmoAxis : uint8_t {
    None   = 0,
    X      = 1,
    Y      = 2,
    Z      = 3,
    // Phase-5 additions (declared now to keep the enum stable):
    XY     = 4,
    YZ     = 5,
    XZ     = 6,
    Center = 7,   // screen-space translate / uniform scale
    View   = 8,   // screen-space rotate ring
};

/// Axis-space basis for the handles.
enum class GizmoSpace : uint8_t {
    World = 0,
    Local = 1,
};

/// Visual configuration — all fields have sensible defaults.
struct GizmoConfig {
    bool visible = false;
    GizmoMode mode = GizmoMode::Translate;
    GizmoSpace space = GizmoSpace::World;

    // Target pixel height for the handles on screen. Actual world scale is
    // recomputed per-frame from camera distance / fovY.
    float targetPixelSize = 80.0f;

    // Colors (RGBA 0-1). Hover / active swap colors on the hovered handle.
    float colorX[4]     = {0.906f, 0.298f, 0.235f, 1.0f};  // #e74c3c
    float colorY[4]     = {0.153f, 0.682f, 0.376f, 1.0f};  // #27ae60
    float colorZ[4]     = {0.204f, 0.596f, 0.859f, 1.0f};  // #3498db
    float colorHover[4] = {1.000f, 0.820f, 0.400f, 1.0f};  // #ffd166
    float colorActive[4]= {1.000f, 1.000f, 1.000f, 1.0f};

    float emissive       = 0.55f;
    float emissiveHover  = 1.40f;

    // Depth behavior. When true, handles always win the depth test (drawn on
    // top of scene geometry). Matches DCC tool convention.
    bool alwaysOnTop = true;
};

/// Engine-owned gizmo manager. Renders translate / rotate / scale handles
/// inside the scene's 3D pipeline (after the billboard pass) and — in later
/// phases — runs picking and drag interaction against them.
///
/// Phase 1 scope (this commit):
///   * Translate arrow meshes only.
///   * Configured position + visibility from JS; screen-stable sizing driven
///     from the scene camera each frame.
///   * No picking, no drag, no rotate/scale. Those arrive in later phases.
class GizmoManager {
public:
    GizmoManager();
    ~GizmoManager();

    GizmoManager(const GizmoManager&) = delete;
    GizmoManager& operator=(const GizmoManager&) = delete;

    // --- JS-facing API (called from gizmo_bindings.cpp) -------------------

    void show() { config_.visible = true; }
    void hide() { config_.visible = false; }
    bool visible() const { return config_.visible; }

    void setMode(GizmoMode m) { config_.mode = m; }
    GizmoMode mode() const { return config_.mode; }

    void setSpace(GizmoSpace s) { config_.space = s; }
    GizmoSpace space() const { return config_.space; }

    /// World-space pivot (attach point). Apps call this whenever the target's
    /// origin changes — or pass a target object in later phases.
    void setPosition(float x, float y, float z);
    const scene::Vec3& position() const { return position_; }

    /// Orientation used when space==Local. Ignored for world-space.
    void setOrientation(const scene::Quat& q) { orientation_ = q; }

    GizmoConfig& config() { return config_; }
    const GizmoConfig& config() const { return config_; }

    // --- Rendering --------------------------------------------------------

    /// Collect the mesh-nodes that should be drawn for the current mode.
    /// Returns an empty list when the gizmo is hidden or uninitialised.
    /// Called by SceneGraph during the render pass. Also updates the per-
    /// frame transforms (position + screen-stable scale + axis orientation)
    /// using the graph's current camera so the list is render-ready.
    std::vector<scene::MeshNode*> meshesForRender(scene::SceneGraph* graph);

    /// Release GPU resources (arrow mesh uploads). Call before GL shutdown.
    void releaseGL();

    // --- Picking + drag ---------------------------------------------------

    struct PickResult {
        GizmoAxis axis = GizmoAxis::None;
        float rayT = 0.0f;              // parameter along the ray at the hit
        scene::Vec3 axisDir{0, 0, 0};    // world-space axis direction
        scene::Vec3 hitPoint{0, 0, 0};   // world-space point on the handle
    };

    /// Ray-vs-current-mode pick. Returns axis=None if no handle is hit.
    /// Axes / rings / boxes are tested with a geometry-appropriate pick
    /// radius derived from the current screen-stable scale.
    PickResult pick(const scene::Vec3& rayOrigin, const scene::Vec3& rayDir);

    /// Begin a drag along the picked handle. Captures the pivot + the
    /// initial ray parameter / angle so subsequent updateDrag() calls can
    /// produce cumulative world deltas.
    void beginDrag(const PickResult& hit,
                   const scene::Vec3& rayOrigin, const scene::Vec3& rayDir);

    /// Update a drag with a new ray and return the per-frame delta for
    /// the active mode via out-params. Returns true if the gizmo is
    /// currently dragging (and therefore consumed this mouse event).
    ///   translate: outTranslate = world-space delta since last call
    ///   rotate:    outRotate    = axis-angle quaternion rotation applied
    ///                             since last call (world space)
    ///   scale:     outScale     = per-axis multiplicative factor
    bool updateDrag(const scene::Vec3& rayOrigin, const scene::Vec3& rayDir,
                    scene::Vec3& outTranslate,
                    scene::Quat& outRotate,
                    scene::Vec3& outScale);

    void endDrag();
    bool isDragging() const { return dragAxis_ != GizmoAxis::None; }
    GizmoAxis draggingAxis() const { return dragAxis_; }

    /// Hover state — driven by the engine input handler each mousemove.
    void setHovered(GizmoAxis axis);
    GizmoAxis hovered() const { return hovered_; }

    // --- JS callbacks -----------------------------------------------------
    //
    // Stored as owned JSValues; the bindings layer is responsible for
    // calling setJSContext() at install time and for freeing any previous
    // callback values when they are replaced. Fire* methods are called by
    // the engine during drag updates.

    void setJSContext(JSContext* ctx) { jsCtx_ = ctx; }
    JSContext* jsContext() const { return jsCtx_; }

    void setCallback(int slot, JSValue fn);

    // Slot IDs — mirrored in gizmo_bindings.cpp.
    enum CallbackSlot : int {
        CB_Position    = 0,   // () -> [x,y,z]           - called each frame to read pivot
        CB_Orientation = 1,   // () -> [x,y,z,w]          - (local-space only)
        CB_BeginDrag   = 2,   // ({mode, axis}) -> void
        CB_Translate   = 3,   // (dx,dy,dz) -> bool?      - true suppresses default
        CB_Rotate      = 4,   // (qx,qy,qz,qw) -> bool?
        CB_Scale       = 5,   // (sx,sy,sz) -> bool?
        CB_EndDrag     = 6,   // ({mode, axis, committed}) -> void
        CB_HoverChange = 7,   // (axis|null) -> void
        CB_COUNT       = 8,
    };

    /// Invoke the position/orientation callback if set, and update the
    /// cached pivot/orientation used for rendering + picking this frame.
    void refreshFromCallbacks();

    /// Apply a per-frame translate delta to the attached target.
    /// Calls CB_Translate if set; if the JS callback returns a truthy
    /// value the engine treats the drag as "consumed" and does not fall
    /// back to the default setPosition behavior.
    void fireTranslate(const scene::Vec3& worldDelta);
    void fireRotate(const scene::Quat& worldRot);
    void fireScale(const scene::Vec3& factor);
    void fireBegin();
    void fireEnd(bool committed);
    void fireHoverChange();

private:
    /// Build a +X-aligned arrow MeshData (shaft cylinder + cone tip + caps).
    /// Mirrors apps/scene-editor/gizmo.js::buildArrowMesh so the visuals
    /// carry over 1:1.
    struct ArrowGeom {
        float shaftLen    = 0.85f;
        float shaftRadius = 0.025f;
        float tipLen      = 0.30f;
        float tipRadius   = 0.085f;
        int   segments    = 14;
        float length() const { return shaftLen + tipLen; }
    };
    static void buildArrowMeshData(const ArrowGeom& g,
                                   /*out*/ std::vector<float>& positions,
                                   /*out*/ std::vector<float>& normals,
                                   /*out*/ std::vector<uint32_t>& indices);

    /// Camera-distance → uniform scale, using the JS formula:
    ///   worldSize = distance * 2 * tan(fov/2) / canvasHeight * targetPx
    /// Returns 1.0 as a fallback if the graph has no camera/canvas yet.
    float screenStableScale(scene::SceneGraph* graph) const;

    /// Axis basis in world space for picking + drag math, respecting
    /// world/local space. X = (1,0,0) world-space or rotated by orientation_
    /// in local-space mode.
    void resolveAxes(scene::Vec3& ax, scene::Vec3& ay, scene::Vec3& az) const;

    /// Closest parameter t on the infinite line (pivot + t * axisDir)
    /// to the ray. Used for translate / scale drag math.
    static float rayVsAxisParam(const scene::Vec3& rayO, const scene::Vec3& rayD,
                                const scene::Vec3& pivot, const scene::Vec3& axisDir);

    /// Closest-distance-from-ray-to-finite-segment test (picks against
    /// arrows / scale handles). Returns dist + rayT + segT.
    struct RaySegResult { float rayT; float segT; float dist; scene::Vec3 segPoint; };
    static RaySegResult closestRayToSegment(const scene::Vec3& rayO, const scene::Vec3& rayD,
                                            const scene::Vec3& A, const scene::Vec3& B);

    /// Intersect ray with the plane through `pivot` with normal `axis`.
    /// Returns false if ray is parallel to the plane. Used for rotate
    /// angle computation + plane-handle picking.
    static bool rayVsPlane(const scene::Vec3& rayO, const scene::Vec3& rayD,
                           const scene::Vec3& pivot, const scene::Vec3& normal,
                           scene::Vec3& outPoint);

    void ensureTranslateMeshes();
    void ensureRotateMeshes();
    void ensureScaleMeshes();

    GizmoConfig config_;
    scene::Vec3 position_{0, 0, 0};
    scene::Quat orientation_ = scene::Quat::identity();
    GizmoAxis hovered_ = GizmoAxis::None;
    float currentScale_ = 1.0f;   // last screen-stable scale (for pick radii)

    // Translate arrows.
    ArrowGeom arrow_;
    std::unique_ptr<scene::MeshNode> arrowX_;
    std::unique_ptr<scene::MeshNode> arrowY_;
    std::unique_ptr<scene::MeshNode> arrowZ_;
    bool arrowsBuilt_ = false;

    // Rotate rings — tori around each axis.
    struct RingGeom {
        float majorRadius = 1.0f;
        float tubeRadius  = 0.025f;
        int   majorSegs   = 48;
        int   minorSegs   = 8;
    };
    RingGeom ring_;
    std::unique_ptr<scene::MeshNode> ringX_;
    std::unique_ptr<scene::MeshNode> ringY_;
    std::unique_ptr<scene::MeshNode> ringZ_;
    bool ringsBuilt_ = false;

    // Scale boxes — arrow-like shafts capped with a cube instead of a cone.
    struct ScaleGeom {
        float shaftLen    = 0.85f;
        float shaftRadius = 0.025f;
        float cubeSize    = 0.12f;
        int   segments    = 14;
        float length() const { return shaftLen + cubeSize; }
    };
    ScaleGeom scaleGeom_;
    std::unique_ptr<scene::MeshNode> scaleX_;
    std::unique_ptr<scene::MeshNode> scaleY_;
    std::unique_ptr<scene::MeshNode> scaleZ_;
    std::unique_ptr<scene::MeshNode> scaleCenter_;
    bool scaleBuilt_ = false;

    // --- Drag state -------------------------------------------------------
    GizmoAxis dragAxis_ = GizmoAxis::None;
    scene::Vec3 dragPivot_{0, 0, 0};
    scene::Vec3 dragAxisDir_{1, 0, 0};
    scene::Vec3 dragNormal_{0, 0, 1};     // plane normal (for rotate / plane handles)
    float       dragRefParam_ = 0.0f;     // axis-line parameter at grab-time
    float       dragRefAngle_ = 0.0f;     // rotate ring angle at grab-time
    scene::Vec3 dragLastPoint_{0, 0, 0};
    float       dragLastParam_ = 0.0f;
    float       dragLastAngle_ = 0.0f;
    scene::Vec3 dragLastScale_{1, 1, 1};

    // --- JS callbacks -----------------------------------------------------
    JSContext* jsCtx_ = nullptr;
    JSValue callbacks_[CB_COUNT];
    bool callbacksInited_ = false;

    /// Rotate axis-angle by `q` about `axisDir` through angle `delta`.
    static scene::Quat quatAxisAngle(const scene::Vec3& axis, float radians);
};

} // namespace bro::engine
