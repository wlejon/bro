#include "engine/gizmo.h"
#include "engine/gizmo_internal.h"

#include "scene/mesh_node.h"
#include "scene/scene_graph.h"

#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>

namespace bro::engine {

using scene::MeshNode;
using bromath::Quat;
using bromath::Vec3;

void GizmoManager::releaseGL() {
    auto r = [](MeshNode* n){ if (n) n->releaseGL(); };
    r(arrowX_.get()); r(arrowY_.get()); r(arrowZ_.get());
    r(ringX_.get());  r(ringY_.get());  r(ringZ_.get());
    r(scaleX_.get()); r(scaleY_.get()); r(scaleZ_.get());
    r(scaleCenter_.get());
    r(planeXY_.get()); r(planeYZ_.get()); r(planeXZ_.get());
    r(ringView_.get());
}

// ---------------------------------------------------------------------------
// Arrow mesh
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

    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kGizmoPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kGizmoPi * 2.0f;
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);
        float ny = (s0 + s1) * 0.5f;
        float nz = (c0 + c1) * 0.5f;
        float nl = std::sqrt(ny*ny + nz*nz);
        if (nl == 0.0f) nl = 1.0f;
        ny /= nl; nz /= nl;
        uint32_t a = v(0.0f,     shaftRadius * s0, shaftRadius * c0, 0.0f, ny, nz);
        uint32_t b = v(0.0f,     shaftRadius * s1, shaftRadius * c1, 0.0f, ny, nz);
        uint32_t c = v(shaftLen, shaftRadius * s1, shaftRadius * c1, 0.0f, ny, nz);
        uint32_t d = v(shaftLen, shaftRadius * s0, shaftRadius * c0, 0.0f, ny, nz);
        indices.push_back(a); indices.push_back(b); indices.push_back(c);
        indices.push_back(a); indices.push_back(c); indices.push_back(d);
    }

    uint32_t backCenter = v(0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kGizmoPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kGizmoPi * 2.0f;
        uint32_t va = v(0.0f, shaftRadius * std::sin(a0), shaftRadius * std::cos(a0), -1.0f, 0.0f, 0.0f);
        uint32_t vb = v(0.0f, shaftRadius * std::sin(a1), shaftRadius * std::cos(a1), -1.0f, 0.0f, 0.0f);
        indices.push_back(backCenter); indices.push_back(vb); indices.push_back(va);
    }

    uint32_t baseCenter = v(shaftLen, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kGizmoPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kGizmoPi * 2.0f;
        uint32_t va = v(shaftLen, tipRadius * std::sin(a0), tipRadius * std::cos(a0), -1.0f, 0.0f, 0.0f);
        uint32_t vb = v(shaftLen, tipRadius * std::sin(a1), tipRadius * std::cos(a1), -1.0f, 0.0f, 0.0f);
        indices.push_back(baseCenter); indices.push_back(va); indices.push_back(vb);
    }

    const float tipApexX = shaftLen + tipLen;
    const float slantHyp = std::sqrt(tipLen*tipLen + tipRadius*tipRadius);
    const float slantNX  = tipRadius / (slantHyp != 0.0f ? slantHyp : 1.0f);
    const float slantNR  = tipLen    / (slantHyp != 0.0f ? slantHyp : 1.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kGizmoPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kGizmoPi * 2.0f;
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);
        float ny = (s0 + s1) * 0.5f;
        float nz = (c0 + c1) * 0.5f;
        float nl = std::sqrt(ny*ny + nz*nz);
        if (nl == 0.0f) nl = 1.0f;
        float nyU = ny / nl, nzU = nz / nl;
        uint32_t apex = v(tipApexX, 0.0f, 0.0f, slantNX, nyU*slantNR, nzU*slantNR);
        uint32_t ba   = v(shaftLen, tipRadius * s0, tipRadius * c0,
                          slantNX, nyU*slantNR, nzU*slantNR);
        uint32_t bb   = v(shaftLen, tipRadius * s1, tipRadius * c1,
                          slantNX, nyU*slantNR, nzU*slantNR);
        indices.push_back(apex); indices.push_back(ba); indices.push_back(bb);
    }
}

// ---------------------------------------------------------------------------
// Ring torus + scale handle helpers
// ---------------------------------------------------------------------------

static void buildRingMesh(float majorR, float tubeR, int majorSegs, int minorSegs,
                          bromesh::MeshData& out) {
    out.clear();
    const int M = majorSegs, N = minorSegs;
    out.positions.reserve((M + 1) * (N + 1) * 3);
    out.normals.reserve((M + 1) * (N + 1) * 3);
    out.indices.reserve(M * N * 6);

    for (int i = 0; i <= M; ++i) {
        float u = static_cast<float>(i) / M;
        float au = u * kGizmoPi * 2.0f;
        float cu = std::cos(au), su = std::sin(au);
        for (int j = 0; j <= N; ++j) {
            float vv = static_cast<float>(j) / N;
            float av = vv * kGizmoPi * 2.0f;
            float cv = std::cos(av), sv = std::sin(av);
            float ox = tubeR * sv;
            float oy = tubeR * cv * su;
            float oz = tubeR * cv * cu;
            float px = ox;
            float py = majorR * su + oy;
            float pz = majorR * cu + oz;
            float nx = sv;
            float ny = cv * su;
            float nz = cv * cu;
            float nl = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (nl == 0.0f) nl = 1.0f;
            out.positions.push_back(px);
            out.positions.push_back(py);
            out.positions.push_back(pz);
            out.normals.push_back(nx / nl);
            out.normals.push_back(ny / nl);
            out.normals.push_back(nz / nl);
        }
    }

    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            uint32_t a = static_cast<uint32_t>(i * (N + 1) + j);
            uint32_t b = static_cast<uint32_t>((i + 1) * (N + 1) + j);
            uint32_t c = static_cast<uint32_t>((i + 1) * (N + 1) + (j + 1));
            uint32_t d = static_cast<uint32_t>(i * (N + 1) + (j + 1));
            out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(c);
            out.indices.push_back(a); out.indices.push_back(c); out.indices.push_back(d);
        }
    }
}

static void appendCube(std::vector<float>& positions,
                       std::vector<float>& normals,
                       std::vector<uint32_t>& indices,
                       float cx, float cy, float cz, float s) {
    float h = s * 0.5f;
    struct Face { Vec3 n; Vec3 u; Vec3 v; };
    Face faces[6] = {
        {{ 1,0,0}, {0,1,0}, {0,0,1}},
        {{-1,0,0}, {0,1,0}, {0,0,1}},
        {{ 0,1,0}, {1,0,0}, {0,0,1}},
        {{ 0,-1,0},{1,0,0}, {0,0,1}},
        {{ 0,0, 1},{1,0,0}, {0,1,0}},
        {{ 0,0,-1},{1,0,0}, {0,1,0}},
    };
    for (auto& f : faces) {
        Vec3 center(cx + f.n.x * h, cy + f.n.y * h, cz + f.n.z * h);
        Vec3 corners[4] = {
            center - f.u * h - f.v * h,
            center + f.u * h - f.v * h,
            center + f.u * h + f.v * h,
            center - f.u * h + f.v * h,
        };
        uint32_t base = static_cast<uint32_t>(positions.size() / 3);
        for (auto& c : corners) {
            positions.push_back(c.x); positions.push_back(c.y); positions.push_back(c.z);
            normals.push_back(f.n.x); normals.push_back(f.n.y); normals.push_back(f.n.z);
        }
        indices.push_back(base); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base); indices.push_back(base + 2); indices.push_back(base + 3);
    }
}

static void buildScaleHandleMesh(float shaftLen, float shaftRadius,
                                 float cubeSize, int seg,
                                 bromesh::MeshData& out) {
    out.clear();
    std::vector<float>&    P = out.positions;
    std::vector<float>&    N = out.normals;
    std::vector<uint32_t>& I = out.indices;

    auto v = [&](float px, float py, float pz,
                 float nx, float ny, float nz) -> uint32_t {
        uint32_t idx = static_cast<uint32_t>(P.size() / 3);
        P.push_back(px); P.push_back(py); P.push_back(pz);
        N.push_back(nx); N.push_back(ny); N.push_back(nz);
        return idx;
    };

    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kGizmoPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kGizmoPi * 2.0f;
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);
        float ny = (s0 + s1) * 0.5f;
        float nz = (c0 + c1) * 0.5f;
        float nl = std::sqrt(ny*ny + nz*nz);
        if (nl == 0.0f) nl = 1.0f;
        ny /= nl; nz /= nl;
        uint32_t a = v(0.0f,     shaftRadius * s0, shaftRadius * c0, 0.0f, ny, nz);
        uint32_t b = v(0.0f,     shaftRadius * s1, shaftRadius * c1, 0.0f, ny, nz);
        uint32_t c = v(shaftLen, shaftRadius * s1, shaftRadius * c1, 0.0f, ny, nz);
        uint32_t d = v(shaftLen, shaftRadius * s0, shaftRadius * c0, 0.0f, ny, nz);
        I.push_back(a); I.push_back(b); I.push_back(c);
        I.push_back(a); I.push_back(c); I.push_back(d);
    }
    uint32_t backCenter = v(0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kGizmoPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kGizmoPi * 2.0f;
        uint32_t va = v(0.0f, shaftRadius * std::sin(a0), shaftRadius * std::cos(a0), -1, 0, 0);
        uint32_t vb = v(0.0f, shaftRadius * std::sin(a1), shaftRadius * std::cos(a1), -1, 0, 0);
        I.push_back(backCenter); I.push_back(vb); I.push_back(va);
    }

    appendCube(P, N, I, shaftLen + cubeSize * 0.5f, 0.0f, 0.0f, cubeSize);
}

void GizmoManager::ensureTranslateMeshes() {
    if (arrowsBuilt_) return;
    std::vector<float> positions, normals;
    std::vector<uint32_t> indices;
    buildArrowMeshData(arrow_, positions, normals, indices);
    bromesh::MeshData md;
    md.positions = positions;
    md.normals   = normals;
    md.indices   = indices;

    arrowX_ = std::make_unique<MeshNode>("gizmo-translate-x");
    arrowY_ = std::make_unique<MeshNode>("gizmo-translate-y");
    arrowZ_ = std::make_unique<MeshNode>("gizmo-translate-z");
    arrowX_->setMesh(md);
    arrowY_->setMesh(md);
    arrowZ_->setMesh(md);

    arrowX_->setColor(config_.colorX[0], config_.colorX[1], config_.colorX[2], config_.colorX[3]);
    arrowY_->setColor(config_.colorY[0], config_.colorY[1], config_.colorY[2], config_.colorY[3]);
    arrowZ_->setColor(config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], config_.colorZ[3]);
    arrowX_->setUnlit(true);
    arrowY_->setUnlit(true);
    arrowZ_->setUnlit(true);
    arrowsBuilt_ = true;
}

static void buildPlaneQuadMesh(int normalAxis, float offset, float size,
                               bromesh::MeshData& out) {
    out.clear();
    const float a = offset, b = offset + size;
    float c[4][3];
    auto set = [&](int i, float x, float y, float z) {
        c[i][0] = x; c[i][1] = y; c[i][2] = z;
    };
    float n[3] = {0, 0, 0};
    n[normalAxis] = 1.0f;
    if (normalAxis == 2) {          // XY quad
        set(0, a, a, 0); set(1, b, a, 0); set(2, b, b, 0); set(3, a, b, 0);
    } else if (normalAxis == 0) {   // YZ quad
        set(0, 0, a, a); set(1, 0, b, a); set(2, 0, b, b); set(3, 0, a, b);
    } else {                        // XZ quad
        set(0, a, 0, a); set(1, b, 0, a); set(2, b, 0, b); set(3, a, 0, b);
    }
    for (int i = 0; i < 4; ++i) {
        out.positions.push_back(c[i][0]);
        out.positions.push_back(c[i][1]);
        out.positions.push_back(c[i][2]);
        out.normals.push_back(n[0]);
        out.normals.push_back(n[1]);
        out.normals.push_back(n[2]);
    }
    const uint32_t idx[] = { 0, 1, 2,  0, 2, 3,   // front
                             0, 2, 1,  0, 3, 2 }; // back
    for (uint32_t i : idx) out.indices.push_back(i);
}

bool GizmoManager::planeBasis(GizmoAxis axis, const Vec3& ax, const Vec3& ay,
                              const Vec3& az, Vec3& u, Vec3& v, Vec3& normal) {
    switch (axis) {
    case GizmoAxis::XY: u = ax; v = ay; normal = az; return true;
    case GizmoAxis::YZ: u = ay; v = az; normal = ax; return true;
    case GizmoAxis::XZ: u = ax; v = az; normal = ay; return true;
    default: return false;
    }
}

void GizmoManager::ensurePlaneMeshes() {
    if (planesBuilt_) return;
    bromesh::MeshData xy, yz, xz;
    buildPlaneQuadMesh(2, plane_.offset, plane_.size, xy);
    buildPlaneQuadMesh(0, plane_.offset, plane_.size, yz);
    buildPlaneQuadMesh(1, plane_.offset, plane_.size, xz);

    planeXY_ = std::make_unique<MeshNode>("gizmo-plane-xy");
    planeYZ_ = std::make_unique<MeshNode>("gizmo-plane-yz");
    planeXZ_ = std::make_unique<MeshNode>("gizmo-plane-xz");
    planeXY_->setMesh(xy);
    planeYZ_->setMesh(yz);
    planeXZ_->setMesh(xz);

    planeXY_->setColor(config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], kGizmoPlaneAlpha);
    planeYZ_->setColor(config_.colorX[0], config_.colorX[1], config_.colorX[2], kGizmoPlaneAlpha);
    planeXZ_->setColor(config_.colorY[0], config_.colorY[1], config_.colorY[2], kGizmoPlaneAlpha);
    planeXY_->setUnlit(true);
    planeYZ_->setUnlit(true);
    planeXZ_->setUnlit(true);
    planesBuilt_ = true;
}

void GizmoManager::ensureRotateMeshes() {
    if (ringsBuilt_) return;
    bromesh::MeshData md;
    buildRingMesh(ring_.majorRadius, ring_.tubeRadius, ring_.majorSegs, ring_.minorSegs, md);
    ringX_ = std::make_unique<MeshNode>("gizmo-rotate-x");
    ringY_ = std::make_unique<MeshNode>("gizmo-rotate-y");
    ringZ_ = std::make_unique<MeshNode>("gizmo-rotate-z");
    ringX_->setMesh(md);
    ringY_->setMesh(md);
    ringZ_->setMesh(md);
    ringX_->setColor(config_.colorX[0], config_.colorX[1], config_.colorX[2], config_.colorX[3]);
    ringY_->setColor(config_.colorY[0], config_.colorY[1], config_.colorY[2], config_.colorY[3]);
    ringZ_->setColor(config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], config_.colorZ[3]);
    ringX_->setUnlit(true);
    ringY_->setUnlit(true);
    ringZ_->setUnlit(true);

    bromesh::MeshData vm;
    buildRingMesh(ring_.majorRadius * kGizmoViewRingScale, ring_.tubeRadius,
                  ring_.majorSegs, ring_.minorSegs, vm);
    ringView_ = std::make_unique<MeshNode>("gizmo-rotate-view");
    ringView_->setMesh(vm);
    ringView_->setColor(0.85f, 0.85f, 0.85f, 0.9f);
    ringView_->setUnlit(true);
    ringsBuilt_ = true;
}

void GizmoManager::ensureScaleMeshes() {
    if (scaleBuilt_) return;
    bromesh::MeshData shaft;
    buildScaleHandleMesh(scaleGeom_.shaftLen, scaleGeom_.shaftRadius,
                         scaleGeom_.cubeSize, scaleGeom_.segments, shaft);
    scaleX_ = std::make_unique<MeshNode>("gizmo-scale-x");
    scaleY_ = std::make_unique<MeshNode>("gizmo-scale-y");
    scaleZ_ = std::make_unique<MeshNode>("gizmo-scale-z");
    scaleX_->setMesh(shaft);
    scaleY_->setMesh(shaft);
    scaleZ_->setMesh(shaft);
    scaleX_->setColor(config_.colorX[0], config_.colorX[1], config_.colorX[2], config_.colorX[3]);
    scaleY_->setColor(config_.colorY[0], config_.colorY[1], config_.colorY[2], config_.colorY[3]);
    scaleZ_->setColor(config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], config_.colorZ[3]);
    scaleX_->setUnlit(true);
    scaleY_->setUnlit(true);
    scaleZ_->setUnlit(true);

    bromesh::MeshData cube;
    appendCube(cube.positions, cube.normals, cube.indices,
               0.0f, 0.0f, 0.0f, scaleGeom_.cubeSize);
    scaleCenter_ = std::make_unique<MeshNode>("gizmo-scale-center");
    scaleCenter_->setMesh(cube);
    scaleCenter_->setColor(0.9f, 0.9f, 0.9f, 1.0f);
    scaleCenter_->setUnlit(true);

    scaleBuilt_ = true;
}

void GizmoManager::resolveAxes(Vec3& ax, Vec3& ay, Vec3& az) const {
    if (config_.space == GizmoSpace::World) {
        ax = Vec3(1, 0, 0); ay = Vec3(0, 1, 0); az = Vec3(0, 0, 1);
        return;
    }
    const Quat& q = orientation_;
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    ax = Vec3(1 - 2*(yy + zz), 2*(xy + wz),     2*(xz - wy));
    ay = Vec3(2*(xy - wz),     1 - 2*(xx + zz), 2*(yz + wx));
    az = Vec3(2*(xz + wy),     2*(yz - wx),     1 - 2*(xx + yy));
}

float GizmoManager::screenStableScale(scene::SceneGraph* graph) const {
    if (!graph) return 1.0f;
    int ch = graph->canvasHeight();
    if (ch <= 0) return 1.0f;
    Vec3 eye = graph->cameraEye();
    Vec3 d   = position_ - eye;
    float dist = vlen_(d);
    if (dist < 1e-4f) dist = 1e-4f;
    const auto& P = graph->projectionMatrix();
    float m11 = P.at(1, 1);
    if (!std::isfinite(m11) || m11 <= 0.0f) return 1.0f;

    float worldPerPixel;
    if (graph->cameraIsPerspective()) {
        float tanHalfFov = 1.0f / m11;
        worldPerPixel = (dist * 2.0f * tanHalfFov) / static_cast<float>(ch);
    } else {
        float halfH = 1.0f / m11;
        worldPerPixel = (2.0f * halfH) / static_cast<float>(ch);
    }
    float world = config_.targetPixelSize * worldPerPixel;
    float baseLen = arrow_.length();
    if (baseLen <= 0.0f) baseLen = 1.0f;
    return world / baseLen;
}

std::vector<MeshNode*> GizmoManager::meshesForRender(scene::SceneGraph* graph) {
    if (!config_.visible) return {};

    refreshFromCallbacks();

    float s = screenStableScale(graph);
    currentScale_ = s;

    if (graph) {
        Vec3 toEye = graph->cameraEye() - position_;
        if (vlen_(toEye) > 1e-6f) viewDir_ = vnorm_(toEye);

        const auto& V = graph->viewMatrix();
        const auto& P = graph->projectionMatrix();
        cam_.eye     = graph->cameraEye();
        cam_.right   = Vec3(V.at(0, 0), V.at(0, 1), V.at(0, 2));
        cam_.up      = Vec3(V.at(1, 0), V.at(1, 1), V.at(1, 2));
        cam_.forward = Vec3(-V.at(2, 0), -V.at(2, 1), -V.at(2, 2));
        cam_.p00 = P.at(0, 0);
        cam_.p11 = P.at(1, 1);
        cam_.canvasW = graph->canvasWidth();
        cam_.canvasH = graph->canvasHeight();
        cam_.aspect = (cam_.canvasH > 0)
                          ? static_cast<float>(cam_.canvasW) / static_cast<float>(cam_.canvasH)
                          : 1.0f;
        cam_.perspective = graph->cameraIsPerspective();
        cam_.valid = std::isfinite(cam_.p00) && std::isfinite(cam_.p11) &&
                     cam_.p00 != 0.0f && cam_.p11 != 0.0f &&
                     cam_.canvasW > 0 && cam_.canvasH > 0;
    }

    Vec3 axX, axY, axZ;
    resolveAxes(axX, axY, axZ);

    auto rotateFromXTo = [](const Vec3& target) -> Quat {
        Vec3 from(1, 0, 0);
        Vec3 t = vnorm_(target);
        float dot = from.x*t.x + from.y*t.y + from.z*t.z;
        if (dot > 0.99999f) return bromath::qidentity();
        if (dot < -0.99999f) {
            return bromath::qaxisAngle(Vec3(0, 1, 0), kGizmoPi);
        }
        Vec3 axis = bromath::vcross(from, t);
        float len = vlen_(axis);
        if (len > 1e-9f) axis = Vec3(axis.x/len, axis.y/len, axis.z/len);
        float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
        return bromath::qaxisAngle(axis, angle);
    };

    auto place = [&](MeshNode* n, const Vec3& axis) {
        if (!n) return;
        n->setPosition(position_);
        n->setScale(s, s, s);
        n->setRotation(rotateFromXTo(axis));
    };

    std::vector<MeshNode*> out;
    switch (config_.mode) {
    case GizmoMode::Translate: {
        ensureTranslateMeshes();
        ensurePlaneMeshes();
        place(arrowX_.get(), axX);
        place(arrowY_.get(), axY);
        place(arrowZ_.get(), axZ);
        Quat axisSpace = (config_.space == GizmoSpace::World)
                             ? bromath::qidentity() : orientation_;
        for (MeshNode* n : { planeXY_.get(), planeYZ_.get(), planeXZ_.get() }) {
            if (!n) continue;
            n->setPosition(position_);
            n->setScale(s, s, s);
            n->setRotation(axisSpace);
        }
        out = { arrowX_.get(), arrowY_.get(), arrowZ_.get(),
                planeXY_.get(), planeYZ_.get(), planeXZ_.get() };
        break;
    }
    case GizmoMode::Rotate:
        ensureRotateMeshes();
        place(ringX_.get(), axX);
        place(ringY_.get(), axY);
        place(ringZ_.get(), axZ);
        place(ringView_.get(), viewDir_);
        out = { ringX_.get(), ringY_.get(), ringZ_.get(), ringView_.get() };
        break;
    case GizmoMode::Scale:
        ensureScaleMeshes();
        place(scaleX_.get(), axX);
        place(scaleY_.get(), axY);
        place(scaleZ_.get(), axZ);
        if (scaleCenter_) {
            scaleCenter_->setPosition(position_);
            scaleCenter_->setScale(s, s, s);
            scaleCenter_->setRotation(bromath::qidentity());
        }
        out = { scaleX_.get(), scaleY_.get(), scaleZ_.get(), scaleCenter_.get() };
        break;
    }
    return out;
}

} // namespace bro::engine
