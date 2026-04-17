#pragma once

#include "scene/scene_node.h"

#include <functional>
#include <memory>
#include <vector>

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

    // --- Phase-2+ stubs (kept in the API shape but no-op for now) --------

    /// Hovered handle (driven by picking in phase 2). Affects visual colour.
    void setHovered(GizmoAxis axis);
    GizmoAxis hovered() const { return hovered_; }

private:
    /// Lazily create the three arrow mesh-nodes (owned by the manager, NOT
    /// registered with any SceneGraph).
    void ensureTranslateMeshes();

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

    GizmoConfig config_;
    scene::Vec3 position_{0, 0, 0};
    scene::Quat orientation_ = scene::Quat::identity();
    GizmoAxis hovered_ = GizmoAxis::None;

    // Translate arrows. Three base nodes (one per axis). These live outside
    // any SceneGraph — we hand pointers to them to the graph's render pass
    // for drawing, but do not register them in any node table.
    ArrowGeom arrow_;
    std::unique_ptr<scene::MeshNode> arrowX_;
    std::unique_ptr<scene::MeshNode> arrowY_;
    std::unique_ptr<scene::MeshNode> arrowZ_;
    bool arrowsBuilt_ = false;
};

} // namespace bro::engine
