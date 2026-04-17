#include "engine/gizmo.h"

#include "scene/mesh_node.h"
#include "scene/scene_graph.h"

#include <bromesh/mesh_data.h>

#include <cmath>

namespace bro::engine {

using scene::MeshNode;
using scene::Quat;
using scene::Vec3;

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

GizmoManager::GizmoManager() = default;
GizmoManager::~GizmoManager() = default;

void GizmoManager::setPosition(float x, float y, float z) {
    position_.x = x; position_.y = y; position_.z = z;
}

void GizmoManager::setHovered(GizmoAxis axis) {
    hovered_ = axis;
    // Phase-2+ will swap color / emissive on the hovered arrow here. Leaving
    // as a state-setter for now so the JS binding can plug in without
    // revisiting the API.
}

void GizmoManager::releaseGL() {
    if (arrowX_) arrowX_->releaseGL();
    if (arrowY_) arrowY_->releaseGL();
    if (arrowZ_) arrowZ_->releaseGL();
}

// ---------------------------------------------------------------------------
// Arrow mesh construction — direct port of apps/scene-editor/gizmo.js
// buildArrowMesh. Flat-shaded quads + cap fans along +X.
// ---------------------------------------------------------------------------

void GizmoManager::buildArrowMeshData(const ArrowGeom& g,
                                      std::vector<float>& positions,
                                      std::vector<float>& normals,
                                      std::vector<uint32_t>& indices) {
    positions.clear();
    normals.clear();
    indices.clear();

    const int   seg         = g.segments;
    const float shaftLen    = g.shaftLen;
    const float shaftRadius = g.shaftRadius;
    const float tipLen      = g.tipLen;
    const float tipRadius   = g.tipRadius;

    auto v = [&](float px, float py, float pz,
                 float nx, float ny, float nz) -> uint32_t {
        uint32_t idx = static_cast<uint32_t>(positions.size() / 3);
        positions.push_back(px); positions.push_back(py); positions.push_back(pz);
        normals.push_back(nx);   normals.push_back(ny);   normals.push_back(nz);
        return idx;
    };

    // Shaft sides — per-quad flat normals.
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);
        float ny = (s0 + s1) * 0.5f;
        float nz = (c0 + c1) * 0.5f;
        float nl = std::sqrt(ny * ny + nz * nz);
        if (nl == 0.0f) nl = 1.0f;
        ny /= nl; nz /= nl;
        uint32_t a = v(0.0f,     shaftRadius * s0, shaftRadius * c0, 0.0f, ny, nz);
        uint32_t b = v(0.0f,     shaftRadius * s1, shaftRadius * c1, 0.0f, ny, nz);
        uint32_t c = v(shaftLen, shaftRadius * s1, shaftRadius * c1, 0.0f, ny, nz);
        uint32_t d = v(shaftLen, shaftRadius * s0, shaftRadius * c0, 0.0f, ny, nz);
        indices.push_back(a); indices.push_back(b); indices.push_back(c);
        indices.push_back(a); indices.push_back(c); indices.push_back(d);
    }

    // Shaft back cap (-X).
    uint32_t backCenter = v(0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
        uint32_t va = v(0.0f, shaftRadius * std::sin(a0), shaftRadius * std::cos(a0), -1.0f, 0.0f, 0.0f);
        uint32_t vb = v(0.0f, shaftRadius * std::sin(a1), shaftRadius * std::cos(a1), -1.0f, 0.0f, 0.0f);
        indices.push_back(backCenter); indices.push_back(vb); indices.push_back(va);
    }

    // Cone base ring (faces -X) at x=shaftLen with tipRadius.
    uint32_t baseCenter = v(shaftLen, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
        uint32_t va = v(shaftLen, tipRadius * std::sin(a0), tipRadius * std::cos(a0), -1.0f, 0.0f, 0.0f);
        uint32_t vb = v(shaftLen, tipRadius * std::sin(a1), tipRadius * std::cos(a1), -1.0f, 0.0f, 0.0f);
        indices.push_back(baseCenter); indices.push_back(va); indices.push_back(vb);
    }

    // Cone side. Normals split along the axis + radial components.
    const float tipApexX = shaftLen + tipLen;
    const float slantHyp = std::sqrt(tipLen * tipLen + tipRadius * tipRadius);
    const float slantNX  = tipRadius / (slantHyp != 0.0f ? slantHyp : 1.0f);
    const float slantNR  = tipLen    / (slantHyp != 0.0f ? slantHyp : 1.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);
        float ny = (s0 + s1) * 0.5f;
        float nz = (c0 + c1) * 0.5f;
        float nl = std::sqrt(ny * ny + nz * nz);
        if (nl == 0.0f) nl = 1.0f;
        float nyU = ny / nl;
        float nzU = nz / nl;
        uint32_t apex = v(tipApexX, 0.0f, 0.0f,
                          slantNX, nyU * slantNR, nzU * slantNR);
        uint32_t ba   = v(shaftLen, tipRadius * s0, tipRadius * c0,
                          slantNX, nyU * slantNR, nzU * slantNR);
        uint32_t bb   = v(shaftLen, tipRadius * s1, tipRadius * c1,
                          slantNX, nyU * slantNR, nzU * slantNR);
        indices.push_back(apex); indices.push_back(ba); indices.push_back(bb);
    }
}

// ---------------------------------------------------------------------------
// Mesh node setup — three standalone MeshNodes, one per axis, sharing the
// same arrow geometry but rotated to land on +X / +Y / +Z.
// ---------------------------------------------------------------------------

void GizmoManager::ensureTranslateMeshes() {
    if (arrowsBuilt_) return;

    std::vector<float> positions, normals;
    std::vector<uint32_t> indices;
    buildArrowMeshData(arrow_, positions, normals, indices);

    bromesh::MeshData md;
    md.positions = positions;
    md.normals   = normals;
    md.indices   = indices;

    arrowX_ = std::make_unique<MeshNode>("gizmo-x");
    arrowY_ = std::make_unique<MeshNode>("gizmo-y");
    arrowZ_ = std::make_unique<MeshNode>("gizmo-z");

    arrowX_->setMesh(md);
    arrowY_->setMesh(md);
    arrowZ_->setMesh(md);

    arrowX_->setColor(config_.colorX[0], config_.colorX[1], config_.colorX[2], config_.colorX[3]);
    arrowY_->setColor(config_.colorY[0], config_.colorY[1], config_.colorY[2], config_.colorY[3]);
    arrowZ_->setColor(config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], config_.colorZ[3]);

    arrowX_->setEmissive(config_.emissive);
    arrowY_->setEmissive(config_.emissive);
    arrowZ_->setEmissive(config_.emissive);

    // Axis orientations. Mesh is built along +X.
    // Y arrow: rotate +X onto +Y  → +π/2 around Z.
    // Z arrow: rotate +X onto +Z  → -π/2 around Y.
    arrowX_->setRotation(Quat::identity());
    arrowY_->setRotation(Quat::fromAxisAngle(Vec3(0, 0, 1),  kPi * 0.5f));
    arrowZ_->setRotation(Quat::fromAxisAngle(Vec3(0, 1, 0), -kPi * 0.5f));

    arrowsBuilt_ = true;
}

// ---------------------------------------------------------------------------
// Screen-stable scale — inverse pinhole projection from the JS gizmo.
// ---------------------------------------------------------------------------

float GizmoManager::screenStableScale(scene::SceneGraph* graph) const {
    if (!graph) return 1.0f;
    int ch = graph->canvasHeight();
    if (ch <= 0) return 1.0f;

    // Eye → pivot distance (world units).
    Vec3 eye = graph->cameraEye();
    Vec3 d   = position_ - eye;
    float dist = d.length();
    if (dist < 1e-4f) dist = 1e-4f;

    // Recover tan(fovY/2) from the projection matrix. For Mat4::perspective,
    // m[1][1] = 1 / tan(fovY/2). Orthographic matrices also populate m[1][1]
    // but with a different meaning — we fall back to a distance-independent
    // scale there (rough but visually usable).
    const auto& P = graph->projectionMatrix();
    float m11 = P.m[1][1];
    if (!std::isfinite(m11) || m11 <= 0.0f) return 1.0f;
    float tanHalfFov = 1.0f / m11;

    float worldPerPixel = (dist * 2.0f * tanHalfFov) / static_cast<float>(ch);
    float world = config_.targetPixelSize * worldPerPixel;

    // Divide by the authored arrow length so that a custom-length geom still
    // yields ~targetPx on screen.
    float baseLen = arrow_.length();
    if (baseLen <= 0.0f) baseLen = 1.0f;
    return world / baseLen;
}

// ---------------------------------------------------------------------------
// Per-frame update + returns the list of meshes to render.
// ---------------------------------------------------------------------------

std::vector<MeshNode*> GizmoManager::meshesForRender(scene::SceneGraph* graph) {
    if (!config_.visible) return {};
    if (config_.mode != GizmoMode::Translate) {
        // Rotate / scale handled in later phases. Fail-safe: draw nothing
        // rather than a half-built handle set.
        return {};
    }

    ensureTranslateMeshes();

    float s = screenStableScale(graph);

    auto place = [&](MeshNode* n) {
        n->setPosition(position_);
        n->setScale(s, s, s);
    };
    place(arrowX_.get());
    place(arrowY_.get());
    place(arrowZ_.get());

    return { arrowX_.get(), arrowY_.get(), arrowZ_.get() };
}

} // namespace bro::engine
