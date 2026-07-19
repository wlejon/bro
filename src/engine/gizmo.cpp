#include "engine/gizmo.h"

#include "scene/mesh_node.h"
#include "scene/scene_graph.h"

#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>

namespace bro::engine {

using scene::MeshNode;
using bromath::Quat;
using bromath::Vec3;

namespace {
constexpr float kPi = 3.14159265358979323846f;

// The screen-facing rotate ring sits outside the three axis rings.
constexpr float kViewRingScale = 1.25f;

// Plane quads are translucent so the geometry behind them stays readable.
constexpr float kPlaneAlpha = 0.55f;

inline bool isPlaneAxis(GizmoAxis a) {
    return a == GizmoAxis::XY || a == GizmoAxis::YZ || a == GizmoAxis::XZ;
}

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float vlen_(const Vec3& v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
inline Vec3  vnorm_(const Vec3& v) {
    float l = vlen_(v); return l > 1e-9f ? Vec3(v.x/l, v.y/l, v.z/l) : Vec3(1,0,0);
}
} // namespace

GizmoManager::GizmoManager() {
    for (int i = 0; i < CB_COUNT; ++i) callbacks_[i] = JS_UNDEFINED;
    callbacksInited_ = true;
}

GizmoManager::~GizmoManager() {
    clearCallbacks();
}

void GizmoManager::clearCallbacks() {
    if (!callbacksInited_ || !jsCtx_) return;
    for (int i = 0; i < CB_COUNT; ++i) {
        if (!JS_IsUndefined(callbacks_[i])) {
            JS_FreeValue(jsCtx_, callbacks_[i]);
            callbacks_[i] = JS_UNDEFINED;
        }
    }
    jsCtx_ = nullptr;
}

void GizmoManager::setPosition(float x, float y, float z) {
    position_.x = x; position_.y = y; position_.z = z;
}

void GizmoManager::setHovered(GizmoAxis axis) {
    if (hovered_ == axis) return;
    hovered_ = axis;

    // Swap the visual for the hovered handle's mesh node: it becomes the
    // hover color while every other handle reverts to its base axis color.
    auto apply = [&](MeshNode* n, const float (&base)[4], GizmoAxis axisId) {
        if (!n) return;
        if (hovered_ == axisId || (isDragging() && dragAxis_ == axisId)) {
            n->setColor(config_.colorHover[0], config_.colorHover[1],
                        config_.colorHover[2], config_.colorHover[3]);
        } else {
            n->setColor(base[0], base[1], base[2], base[3]);
        }
    };
    apply(arrowX_.get(), config_.colorX, GizmoAxis::X);
    apply(arrowY_.get(), config_.colorY, GizmoAxis::Y);
    apply(arrowZ_.get(), config_.colorZ, GizmoAxis::Z);
    apply(ringX_.get(),  config_.colorX, GizmoAxis::X);
    apply(ringY_.get(),  config_.colorY, GizmoAxis::Y);
    apply(ringZ_.get(),  config_.colorZ, GizmoAxis::Z);
    apply(scaleX_.get(), config_.colorX, GizmoAxis::X);
    apply(scaleY_.get(), config_.colorY, GizmoAxis::Y);
    apply(scaleZ_.get(), config_.colorZ, GizmoAxis::Z);
    // Plane quads take the colour of the axis they are NORMAL to (XY is
    // normal to Z, XZ to Y, YZ to X) at the translucent alpha they were built
    // with — reusing the opaque axis colour here would make an unhovered
    // plane snap to solid the first time the pointer left it.
    const float planeZ[4] = { config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], kPlaneAlpha };
    const float planeX[4] = { config_.colorX[0], config_.colorX[1], config_.colorX[2], kPlaneAlpha };
    const float planeY[4] = { config_.colorY[0], config_.colorY[1], config_.colorY[2], kPlaneAlpha };
    static const float kViewBase[4] = { 0.85f, 0.85f, 0.85f, 0.9f };
    apply(planeXY_.get(), planeZ, GizmoAxis::XY);
    apply(planeYZ_.get(), planeX, GizmoAxis::YZ);
    apply(planeXZ_.get(), planeY, GizmoAxis::XZ);
    apply(ringView_.get(), kViewBase, GizmoAxis::View);

    fireHoverChange();
}

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
// Arrow mesh — direct port of gizmo.js buildArrowMesh.
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
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
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
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
        uint32_t va = v(0.0f, shaftRadius * std::sin(a0), shaftRadius * std::cos(a0), -1.0f, 0.0f, 0.0f);
        uint32_t vb = v(0.0f, shaftRadius * std::sin(a1), shaftRadius * std::cos(a1), -1.0f, 0.0f, 0.0f);
        indices.push_back(backCenter); indices.push_back(vb); indices.push_back(va);
    }

    uint32_t baseCenter = v(shaftLen, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
        uint32_t va = v(shaftLen, tipRadius * std::sin(a0), tipRadius * std::cos(a0), -1.0f, 0.0f, 0.0f);
        uint32_t vb = v(shaftLen, tipRadius * std::sin(a1), tipRadius * std::cos(a1), -1.0f, 0.0f, 0.0f);
        indices.push_back(baseCenter); indices.push_back(va); indices.push_back(vb);
    }

    const float tipApexX = shaftLen + tipLen;
    const float slantHyp = std::sqrt(tipLen*tipLen + tipRadius*tipRadius);
    const float slantNX  = tipRadius / (slantHyp != 0.0f ? slantHyp : 1.0f);
    const float slantNR  = tipLen    / (slantHyp != 0.0f ? slantHyp : 1.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
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
// Local helpers: ring torus + scale handle (shaft + cube). Declared inside
// the TU to keep the header focussed on the public API.
// ---------------------------------------------------------------------------

static void buildRingMesh(float majorR, float tubeR, int majorSegs, int minorSegs,
                          bromesh::MeshData& out) {
    // Torus around the +X axis: center circle lies in the YZ plane.
    out.clear();
    const int M = majorSegs, N = minorSegs;
    out.positions.reserve((M + 1) * (N + 1) * 3);
    out.normals.reserve((M + 1) * (N + 1) * 3);
    out.indices.reserve(M * N * 6);

    for (int i = 0; i <= M; ++i) {
        float u = static_cast<float>(i) / M;
        float au = u * kPi * 2.0f;
        float cu = std::cos(au), su = std::sin(au);
        // Center of this tube ring slice in YZ plane: (0, majorR*su, majorR*cu).
        for (int j = 0; j <= N; ++j) {
            float vv = static_cast<float>(j) / N;
            float av = vv * kPi * 2.0f;
            float cv = std::cos(av), sv = std::sin(av);
            // Tube-local frame: tangent along the ring (derivative of center),
            // normal points outward from the tube center.
            //   centerDir = (0, cu, -su) (tangent along ring)
            //   outRadial = (0, su, cu)  (from torus center to ring center)
            // Tube point = center + tubeR * (cv * outRadial + sv * +X)
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
    // 6 faces × 4 verts each, flat-shaded per face.
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
    // Same shaft as the translate arrow (cylinder along +X) but terminated
    // with a cube at x = shaftLen instead of a cone.
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
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
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
    // Back cap
    uint32_t backCenter = v(0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < seg; ++i) {
        float a0 = (static_cast<float>(i)     / seg) * kPi * 2.0f;
        float a1 = (static_cast<float>(i + 1) / seg) * kPi * 2.0f;
        uint32_t va = v(0.0f, shaftRadius * std::sin(a0), shaftRadius * std::cos(a0), -1, 0, 0);
        uint32_t vb = v(0.0f, shaftRadius * std::sin(a1), shaftRadius * std::cos(a1), -1, 0, 0);
        I.push_back(backCenter); I.push_back(vb); I.push_back(va);
    }

    // Cube cap sits centered at (shaftLen + cubeSize/2, 0, 0).
    appendCube(P, N, I, shaftLen + cubeSize * 0.5f, 0.0f, 0.0f, cubeSize);
}

// ---------------------------------------------------------------------------
// Lazy mesh setup per mode.
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

// Quad spanning two of the three canonical planes, offset into the positive
// quadrant. Emitted with both windings: these are unlit, camera-facing-
// agnostic handles that must stay visible from either side of the plane.
static void buildPlaneQuadMesh(int normalAxis, float offset, float size,
                               bromesh::MeshData& out) {
    out.clear();
    const float a = offset, b = offset + size;
    // Corners in the plane, ordered around the quad.
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

    // Coloured by the axis each plane is NORMAL to, which is the convention
    // every DCC uses: the blue quad moves in XY, i.e. the plane Z points out of.
    planeXY_->setColor(config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], kPlaneAlpha);
    planeYZ_->setColor(config_.colorX[0], config_.colorX[1], config_.colorX[2], kPlaneAlpha);
    planeXZ_->setColor(config_.colorY[0], config_.colorY[1], config_.colorY[2], kPlaneAlpha);
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

    // Screen-facing ring, drawn slightly larger so it reads as an outer band
    // rather than fighting the three axis rings for the same pixels.
    bromesh::MeshData vm;
    buildRingMesh(ring_.majorRadius * kViewRingScale, ring_.tubeRadius,
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

    // Uniform center cube (white).
    bromesh::MeshData cube;
    appendCube(cube.positions, cube.normals, cube.indices,
               0.0f, 0.0f, 0.0f, scaleGeom_.cubeSize);
    scaleCenter_ = std::make_unique<MeshNode>("gizmo-scale-center");
    scaleCenter_->setMesh(cube);
    scaleCenter_->setColor(0.9f, 0.9f, 0.9f, 1.0f);
    scaleCenter_->setUnlit(true);

    scaleBuilt_ = true;
}

// ---------------------------------------------------------------------------
// Axis basis (world vs local).
// ---------------------------------------------------------------------------

void GizmoManager::resolveAxes(Vec3& ax, Vec3& ay, Vec3& az) const {
    if (config_.space == GizmoSpace::World) {
        ax = Vec3(1, 0, 0); ay = Vec3(0, 1, 0); az = Vec3(0, 0, 1);
        return;
    }
    // Rotate the standard basis by the target's orientation.
    const Quat& q = orientation_;
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    ax = Vec3(1 - 2*(yy + zz), 2*(xy + wz),     2*(xz - wy));
    ay = Vec3(2*(xy - wz),     1 - 2*(xx + zz), 2*(yz + wx));
    az = Vec3(2*(xz + wy),     2*(yz - wx),     1 - 2*(xx + yy));
}

// ---------------------------------------------------------------------------
// Screen-stable scale.
// ---------------------------------------------------------------------------

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

    // World units covered by one pixel of canvas height.
    //
    // Under perspective this grows with distance to the pivot. Under an
    // orthographic camera it does NOT — the projection's half-height fixes it,
    // and P11 IS 1/halfHeight — so folding `dist` in made the gizmo scale with
    // camera dolly, which under ortho changes nothing on screen. Zooming an
    // ortho view (which changes halfHeight) then failed to resize it at all.
    // Both errors compound into handles the wrong size for what is drawn, and
    // since every pick radius below multiplies by this scale, the grab regions
    // swell until neighbouring axes overlap and a drag catches the wrong one.
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

// ---------------------------------------------------------------------------
// Per-frame mesh update.
// ---------------------------------------------------------------------------

std::vector<MeshNode*> GizmoManager::meshesForRender(scene::SceneGraph* graph) {
    if (!config_.visible) return {};

    // Cached pivot may be refreshed from a JS callback before rendering so
    // the gizmo follows a moving target without the app having to call
    // setPosition each frame.
    refreshFromCallbacks();

    float s = screenStableScale(graph);
    currentScale_ = s;

    // Remember which way the camera lies so the screen-facing ring can be
    // oriented, and so picking (which runs outside any render pass) agrees
    // with what was drawn.
    if (graph) {
        Vec3 toEye = graph->cameraEye() - position_;
        if (vlen_(toEye) > 1e-6f) viewDir_ = vnorm_(toEye);
    }

    Vec3 axX, axY, axZ;
    resolveAxes(axX, axY, axZ);

    // Convert axis vector to quaternion that rotates +X onto it.
    auto rotateFromXTo = [](const Vec3& target) -> Quat {
        Vec3 from(1, 0, 0);
        Vec3 t = vnorm_(target);
        float dot = from.x*t.x + from.y*t.y + from.z*t.z;
        if (dot > 0.99999f) return bromath::qidentity();
        if (dot < -0.99999f) {
            // 180°: rotate about any axis perpendicular to +X. Use +Y.
            return bromath::qaxisAngle(Vec3(0, 1, 0), kPi);
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
        // The plane quads are modelled in the canonical XY/YZ/XZ planes, and
        // resolveAxes() produces exactly the identity basis (world space) or
        // the target's orientation (local space) — so the gizmo's own
        // orientation is the correct rotation for them, no per-axis fitting.
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
        // The view ring faces the camera, so it is placed against viewDir_
        // rather than an axis of the gizmo's basis.
        place(ringView_.get(), viewDir_);
        out = { ringX_.get(), ringY_.get(), ringZ_.get(), ringView_.get() };
        break;
    case GizmoMode::Scale:
        ensureScaleMeshes();
        place(scaleX_.get(), axX);
        place(scaleY_.get(), axY);
        place(scaleZ_.get(), axZ);
        // Center handle — no axis rotation.
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

// ---------------------------------------------------------------------------
// Math helpers.
// ---------------------------------------------------------------------------

GizmoManager::RaySegResult
GizmoManager::closestRayToSegment(const Vec3& rayO, const Vec3& rayD,
                                  const Vec3& A, const Vec3& B) {
    Vec3 u = B - A;
    Vec3 w = rayO - A;
    float a = rayD.x*rayD.x + rayD.y*rayD.y + rayD.z*rayD.z;
    float b = rayD.x*u.x + rayD.y*u.y + rayD.z*u.z;
    float c = u.x*u.x + u.y*u.y + u.z*u.z;
    float d = rayD.x*w.x + rayD.y*w.y + rayD.z*w.z;
    float e = u.x*w.x + u.y*w.y + u.z*w.z;
    float denom = a*c - b*b;
    float rayT, segT;
    if (std::fabs(denom) < 1e-9f) {
        segT = 0.5f;
        rayT = (b * segT - d) / (a != 0 ? a : 1.0f);
    } else {
        rayT = (b*e - c*d) / denom;
        segT = (a*e - b*d) / denom;
        segT = clamp01(segT);
        rayT = (b * segT - d) / (a != 0 ? a : 1.0f);
    }
    Vec3 segP = A + u * segT;
    Vec3 rayP = rayO + rayD * rayT;
    Vec3 diff = segP - rayP;
    float dist = vlen_(diff);
    return { rayT, segT, dist, segP };
}

bool GizmoManager::rayVsAxisParam(const Vec3& rayO, const Vec3& rayD,
                                  const Vec3& pivot, const Vec3& axisDir,
                                  float& outParam) {
    // Closest point on the infinite axis line to the ray; outParam is t such
    // that the closest point = pivot + t * axisDir.
    //
    // Returns false when the ray is (near) parallel to the axis, where the
    // solution is unbounded. That case has to be distinguishable from a real
    // answer: this used to return 0.0f, but 0 is a perfectly valid parameter —
    // it names the pivot — so a drag that started well-conditioned and then
    // grazed parallel would read t=0 and jump the object back to its pivot in
    // a single frame. Callers skip the frame instead.
    Vec3 w = rayO - pivot;
    Vec3 u = axisDir;
    float a = rayD.x*rayD.x + rayD.y*rayD.y + rayD.z*rayD.z;
    float b = rayD.x*u.x + rayD.y*u.y + rayD.z*u.z;
    float c = u.x*u.x + u.y*u.y + u.z*u.z;
    float d = rayD.x*w.x + rayD.y*w.y + rayD.z*w.z;
    float e = u.x*w.x + u.y*w.y + u.z*w.z;
    float denom = a*c - b*b;
    // Relative test: with both vectors normalized denom is sin²(angle), so this
    // is a fixed angular cutoff rather than a magnitude-dependent one.
    if (!std::isfinite(denom) || std::fabs(denom) < 1e-7f * (a * c)) return false;
    outParam = (a*e - b*d) / denom;
    return std::isfinite(outParam);
}

bool GizmoManager::rayVsPlane(const Vec3& rayO, const Vec3& rayD,
                              const Vec3& pivot, const Vec3& normal,
                              Vec3& outPoint) {
    float denom = rayD.x*normal.x + rayD.y*normal.y + rayD.z*normal.z;
    if (std::fabs(denom) < 1e-6f) return false;
    Vec3 diff = pivot - rayO;
    float t = (diff.x*normal.x + diff.y*normal.y + diff.z*normal.z) / denom;
    if (t < 0) return false;
    outPoint = rayO + rayD * t;
    return true;
}

Quat GizmoManager::quatAxisAngle(const Vec3& axis, float radians) {
    return bromath::qaxisAngle(vnorm_(axis), radians);
}

// Where the cursor ray meets the plane through the pivot that faces the
// camera. Cursor motion is tracked here because this plane is the one plane
// that can never turn edge-on, so the point moves smoothly with the mouse no
// matter how the gizmo is oriented.
bool GizmoManager::viewPlanePoint(const Vec3& rayO, const Vec3& rayD,
                                  const Vec3& pivot, Vec3& outPoint) const {
    return rayVsPlane(rayO, rayD, pivot, viewDir_, outPoint);
}

// ---------------------------------------------------------------------------
// Picking.
// ---------------------------------------------------------------------------

GizmoManager::PickResult
GizmoManager::pick(const Vec3& rayO, const Vec3& rayD) {
    PickResult out;
    if (!config_.visible) return out;

    Vec3 axX, axY, axZ;
    resolveAxes(axX, axY, axZ);
    Vec3 axes[3] = { axX, axY, axZ };
    GizmoAxis axisIds[3] = { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };

    // Candidates are ranked by how close the cursor is to the handle, NOT by
    // which handle is nearest the camera.
    //
    // Depth ordering is what a solid-geometry picker wants, but these handles
    // are thin sticks that all radiate from one point, so within a couple of
    // handle-widths of the pivot every arm is inside the pick radius at once.
    // Ranking those by depth hands the pick to whichever arm happens to lean
    // toward the viewer — scanning straight along the visible +X arrow from a
    // +Z camera picked "x z z x x x…", a Z hole punched through the middle of
    // X. Worse, the winner changes as the camera orbits, so the same cursor
    // position over the same picture grabs a different axis from a different
    // viewpoint. Ranking by distance-to-cursor picks what the user is pointing
    // at; depth only breaks exact ties, where it is the sensible answer.
    float bestDist = 1e30f;
    float bestT    = 1e30f;
    auto consider = [&](float dist, float rayT, GizmoAxis axis,
                        const Vec3& axisDir, const Vec3& hitPoint) {
        if (dist > bestDist + 1e-6f) return;
        if (std::fabs(dist - bestDist) <= 1e-6f && rayT >= bestT) return;
        bestDist = dist;
        bestT    = rayT;
        out.axis = axis;
        out.axisDir = axisDir;
        out.rayT = rayT;
        out.hitPoint = hitPoint;
    };

    switch (config_.mode) {
    case GizmoMode::Translate: {
        float armLen  = arrow_.length() * currentScale_;
        float pickRad = arrow_.tipRadius * currentScale_ * 2.4f;
        for (int i = 0; i < 3; ++i) {
            Vec3 tip = position_ + axes[i] * armLen;
            auto r = closestRayToSegment(rayO, rayD, position_, tip);
            if (r.rayT < 0) continue;
            if (r.dist > pickRad) continue;
            consider(r.dist, r.rayT, axisIds[i], axes[i], r.segPoint);
        }
        // Plane quads. A hit anywhere inside the quad counts as distance 0 —
        // it is a filled surface, not a line, so it should win over an arm
        // whose centreline merely passes nearby.
        {
            const GizmoAxis planeIds[3] = { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ };
            const float lo = plane_.offset * currentScale_;
            const float hi = plane_.outer() * currentScale_;
            for (GizmoAxis pid : planeIds) {
                Vec3 u, v, n;
                if (!planeBasis(pid, axX, axY, axZ, u, v, n)) continue;
                Vec3 hit;
                if (!rayVsPlane(rayO, rayD, position_, n, hit)) continue;
                Vec3 rel = hit - position_;
                float du = rel.x*u.x + rel.y*u.y + rel.z*u.z;
                float dv = rel.x*v.x + rel.y*v.y + rel.z*v.z;
                if (du < lo || du > hi || dv < lo || dv > hi) continue;
                float t = (hit - rayO).x*rayD.x + (hit - rayO).y*rayD.y + (hit - rayO).z*rayD.z;
                if (t < 0) continue;
                consider(0.0f, t, pid, n, hit);
            }
        }
        break;
    }
    case GizmoMode::Scale: {
        float armLen  = scaleGeom_.length() * currentScale_;
        float pickRad = scaleGeom_.cubeSize * 0.7f * currentScale_;
        for (int i = 0; i < 3; ++i) {
            Vec3 tip = position_ + axes[i] * armLen;
            auto r = closestRayToSegment(rayO, rayD, position_, tip);
            if (r.rayT < 0) continue;
            if (r.dist > pickRad) continue;
            consider(r.dist, r.rayT, axisIds[i], axes[i], r.segPoint);
        }
        // Center uniform-scale cube (picks as a sphere of ~cubeSize). It sits
        // at the pivot where all three arms also qualify, so it competes on
        // the same distance-to-cursor footing as everything else.
        {
            float rad = scaleGeom_.cubeSize * 0.9f * currentScale_;
            Vec3 diff = position_ - rayO;
            float tProj = diff.x*rayD.x + diff.y*rayD.y + diff.z*rayD.z;
            if (tProj > 0) {
                Vec3 closest = rayO + rayD * tProj;
                Vec3 dd = closest - position_;
                float d = vlen_(dd);
                if (d < rad)
                    consider(d, tProj, GizmoAxis::Center, Vec3(1, 1, 1), closest);
            }
        }
        break;
    }
    case GizmoMode::Rotate: {
        float majorR = ring_.majorRadius * currentScale_;
        float tube   = ring_.tubeRadius  * currentScale_ * 3.0f; // generous pick band
        for (int i = 0; i < 3; ++i) {
            Vec3 normal = axes[i];
            Vec3 hit;
            if (!rayVsPlane(rayO, rayD, position_, normal, hit)) continue;
            Vec3 rel = hit - position_;
            float r = vlen_(rel);
            float bandDist = std::fabs(r - majorR);
            if (bandDist > tube) continue;
            float t = (hit - rayO).x * rayD.x + (hit - rayO).y * rayD.y + (hit - rayO).z * rayD.z;
            if (t < 0) continue;
            // Distance from the ring's circle, so where two rings cross the
            // cursor takes the one it is actually nearest to.
            consider(bandDist, t, axisIds[i], normal, hit);
        }
        // Screen-facing ring, on the same distance-from-the-circle footing.
        {
            float viewR = majorR * kViewRingScale;
            Vec3 hit;
            if (rayVsPlane(rayO, rayD, position_, viewDir_, hit)) {
                Vec3 rel = hit - position_;
                float bandDist = std::fabs(vlen_(rel) - viewR);
                float t = (hit - rayO).x*rayD.x + (hit - rayO).y*rayD.y + (hit - rayO).z*rayD.z;
                if (bandDist <= tube && t >= 0)
                    consider(bandDist, t, GizmoAxis::View, viewDir_, hit);
            }
        }
        break;
    }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Drag lifecycle.
// ---------------------------------------------------------------------------

void GizmoManager::beginDrag(const PickResult& hit,
                             const Vec3& rayO, const Vec3& rayD) {
    if (hit.axis == GizmoAxis::None) return;
    dragAxis_    = hit.axis;
    dragPivot_   = position_;
    dragAxisDir_ = hit.axisDir;
    dragLastPoint_ = hit.hitPoint;
    dragLastScale_ = Vec3(1, 1, 1);

    // A plane handle drags in its own plane regardless of mode, so it is
    // resolved before the per-mode axis math.
    if (isPlaneAxis(dragAxis_)) {
        dragNormal_ = hit.axisDir;   // pick stored the plane normal here
        dragLastPoint_ = hit.hitPoint;
        setHovered(dragAxis_);
        fireBegin();
        return;
    }

    switch (config_.mode) {
    case GizmoMode::Translate:
    case GizmoMode::Scale: {
        // A degenerate grab leaves the reference at 0; updateDrag re-seeds it
        // on the first frame that yields a bounded parameter, so the drag
        // simply does nothing until the view is usable rather than lurching.
        dragParamValid_ = rayVsAxisParam(rayO, rayD, dragPivot_, dragAxisDir_,
                                         dragRefParam_);
        if (!dragParamValid_) dragRefParam_ = 0.0f;
        dragLastParam_ = dragRefParam_;
        break;
    }
    case GizmoMode::Rotate: {
        // Rotation follows how far the cursor travels ALONG the ring, divided
        // by the ring's radius — the arc-length definition of an angle.
        //
        // It used to be the angle subtended at the pivot, which varies wildly
        // for identical mouse travel: a drag pointing at the pivot sweeps
        // almost no angle while the same drag across a ring whose ellipse
        // passes near the pivot sweeps an enormous one. Measured on one view,
        // 0.15 degrees against 24 for the same 12px gesture, and the sign
        // inverted between the near and far side of a single ring.
        //
        // The tangent at the grab point already encodes the direction a
        // positive turn about the normal moves the handle, so projecting it to
        // screen gives exactly the drag direction the user sees as "forwards"
        // — no separate sign correction, and none to get backwards.
        dragNormal_ = hit.axisDir;
        Vec3 rel = hit.hitPoint - dragPivot_;
        dragRadius_ = vlen_(rel);

        Vec3 tangent = bromath::vcross(dragNormal_, rel);
        // Flatten into the view plane: only the visible part of that direction
        // can be dragged along.
        float along = tangent.x*viewDir_.x + tangent.y*viewDir_.y + tangent.z*viewDir_.z;
        tangent = tangent - viewDir_ * along;

        dragParamValid_ = (dragRadius_ > 1e-5f) && (vlen_(tangent) > 1e-5f) &&
                          viewPlanePoint(rayO, rayD, dragPivot_, dragLastPoint_);
        dragTangent_ = dragParamValid_ ? vnorm_(tangent) : Vec3(0, 0, 0);
        break;
    }
    }
    // Lock hover to the dragging axis so the "active" color highlight
    // kicks in immediately and hover re-picking is suppressed until drag
    // ends (see input_handling mousemove path).
    setHovered(dragAxis_);
    fireBegin();
}

bool GizmoManager::updateDrag(const Vec3& rayO, const Vec3& rayD,
                              Vec3& outTranslate, Quat& outRotate, Vec3& outScale) {
    outTranslate = Vec3(0, 0, 0);
    outRotate    = bromath::qidentity();
    outScale     = Vec3(1, 1, 1);
    if (!isDragging()) return false;

    // Plane drag: keep the grabbed point under the cursor. The delta is just
    // how far the ray/plane intersection moved, which needs no reference
    // parameter and stays well conditioned until the plane is edge-on (where
    // rayVsPlane declines and the frame is skipped).
    if (isPlaneAxis(dragAxis_)) {
        Vec3 hit;
        if (!rayVsPlane(rayO, rayD, dragPivot_, dragNormal_, hit)) return true;
        outTranslate = hit - dragLastPoint_;
        dragLastPoint_ = hit;
        fireTranslate(outTranslate);
        return true;
    }

    switch (config_.mode) {
    case GizmoMode::Translate: {
        float t;
        if (!rayVsAxisParam(rayO, rayD, dragPivot_, dragAxisDir_, t))
            return true;  // consumed, but no motion this frame
        if (!dragParamValid_) {   // first usable frame after a degenerate grab
            dragParamValid_ = true;
            dragRefParam_ = t;
            dragLastParam_ = t;
            return true;
        }
        float dt = t - dragLastParam_;
        dragLastParam_ = t;
        outTranslate = dragAxisDir_ * dt;
        break;
    }
    case GizmoMode::Scale: {
        float t;
        if (!rayVsAxisParam(rayO, rayD, dragPivot_, dragAxisDir_, t))
            return true;
        if (!dragParamValid_) {
            dragParamValid_ = true;
            dragRefParam_ = t;
            dragLastParam_ = t;
            return true;
        }
        // Ratio-style scale: factor = (currentRefDist + delta) / refDist.
        // Uses axis-projection distance from pivot, avoids negative zero-crossings.
        float ref = dragRefParam_;
        if (std::fabs(ref) < 1e-4f) ref = (ref < 0 ? -1e-4f : 1e-4f);
        float factor = t / ref;
        if (dragAxis_ == GizmoAxis::Center) {
            outScale = Vec3(factor, factor, factor);
        } else if (dragAxis_ == GizmoAxis::X) {
            outScale = Vec3(factor, 1, 1);
        } else if (dragAxis_ == GizmoAxis::Y) {
            outScale = Vec3(1, factor, 1);
        } else if (dragAxis_ == GizmoAxis::Z) {
            outScale = Vec3(1, 1, factor);
        }
        // Deliver as per-frame delta (current/last) so listeners compose.
        Vec3 delta(outScale.x / dragLastScale_.x,
                   outScale.y / dragLastScale_.y,
                   outScale.z / dragLastScale_.z);
        dragLastScale_ = outScale;
        outScale = delta;
        break;
    }
    case GizmoMode::Rotate: {
        if (!dragParamValid_) return true;   // degenerate grab; nothing to do
        Vec3 now;
        if (!viewPlanePoint(rayO, rayD, dragPivot_, now)) return true;
        Vec3 delta = now - dragLastPoint_;
        dragLastPoint_ = now;
        // Arc travelled along the ring = the part of the cursor's motion that
        // runs with the tangent. Motion across the tangent is the user pulling
        // off the ring, and correctly turns it not at all.
        float arc = delta.x*dragTangent_.x + delta.y*dragTangent_.y
                  + delta.z*dragTangent_.z;
        outRotate = quatAxisAngle(dragNormal_, arc / dragRadius_);
        break;
    }
    }

    // Dispatch to JS. The engine input layer only calls updateDrag when the
    // user is actively dragging the gizmo, so consuming the event is safe.
    if (config_.mode == GizmoMode::Translate) fireTranslate(outTranslate);
    else if (config_.mode == GizmoMode::Rotate) fireRotate(outRotate);
    else if (config_.mode == GizmoMode::Scale)  fireScale(outScale);
    return true;
}

void GizmoManager::endDrag() {
    if (!isDragging()) return;
    fireEnd(true);
    dragAxis_ = GizmoAxis::None;
}

// ---------------------------------------------------------------------------
// JS callbacks.
// ---------------------------------------------------------------------------

void GizmoManager::setCallback(int slot, JSValue fn) {
    if (slot < 0 || slot >= CB_COUNT || !jsCtx_) return;
    if (!JS_IsUndefined(callbacks_[slot])) {
        JS_FreeValue(jsCtx_, callbacks_[slot]);
    }
    callbacks_[slot] = JS_DupValue(jsCtx_, fn);
}

void GizmoManager::refreshFromCallbacks() {
    if (!jsCtx_) return;

    // Position callback.
    if (JS_IsFunction(jsCtx_, callbacks_[CB_Position])) {
        JSValue g = JS_GetGlobalObject(jsCtx_);
        JSValue r = JS_Call(jsCtx_, callbacks_[CB_Position], g, 0, nullptr);
        JS_FreeValue(jsCtx_, g);
        if (JS_IsException(r)) {
            JS_FreeValue(jsCtx_, r);
        } else {
            // Accept [x,y,z] array or {x,y,z} object.
            auto read = [&](JSValue v, float& out) {
                if (JS_IsNumber(v)) {
                    double d; JS_ToFloat64(jsCtx_, &d, v);
                    out = static_cast<float>(d);
                }
            };
            JSValue v0 = JS_GetPropertyUint32(jsCtx_, r, 0);
            if (JS_IsNumber(v0)) {
                float x=0, y=0, z=0;
                read(v0, x);
                JSValue v1 = JS_GetPropertyUint32(jsCtx_, r, 1);
                JSValue v2 = JS_GetPropertyUint32(jsCtx_, r, 2);
                read(v1, y); read(v2, z);
                JS_FreeValue(jsCtx_, v1); JS_FreeValue(jsCtx_, v2);
                position_.x = x; position_.y = y; position_.z = z;
            } else if (JS_IsObject(r)) {
                JSValue vx = JS_GetPropertyStr(jsCtx_, r, "x");
                JSValue vy = JS_GetPropertyStr(jsCtx_, r, "y");
                JSValue vz = JS_GetPropertyStr(jsCtx_, r, "z");
                float x = position_.x, y = position_.y, z = position_.z;
                read(vx, x); read(vy, y); read(vz, z);
                JS_FreeValue(jsCtx_, vx); JS_FreeValue(jsCtx_, vy); JS_FreeValue(jsCtx_, vz);
                position_.x = x; position_.y = y; position_.z = z;
            }
            JS_FreeValue(jsCtx_, v0);
            JS_FreeValue(jsCtx_, r);
        }
    }

    // Orientation callback (local-space only).
    if (config_.space == GizmoSpace::Local &&
        JS_IsFunction(jsCtx_, callbacks_[CB_Orientation])) {
        JSValue g = JS_GetGlobalObject(jsCtx_);
        JSValue r = JS_Call(jsCtx_, callbacks_[CB_Orientation], g, 0, nullptr);
        JS_FreeValue(jsCtx_, g);
        if (!JS_IsException(r)) {
            auto readN = [&](JSValue v, float d) -> float {
                if (JS_IsNumber(v)) {
                    double x; JS_ToFloat64(jsCtx_, &x, v);
                    return static_cast<float>(x);
                }
                return d;
            };
            JSValue v0 = JS_GetPropertyUint32(jsCtx_, r, 0);
            if (JS_IsNumber(v0)) {
                JSValue v1 = JS_GetPropertyUint32(jsCtx_, r, 1);
                JSValue v2 = JS_GetPropertyUint32(jsCtx_, r, 2);
                JSValue v3 = JS_GetPropertyUint32(jsCtx_, r, 3);
                orientation_.x = readN(v0, 0);
                orientation_.y = readN(v1, 0);
                orientation_.z = readN(v2, 0);
                orientation_.w = readN(v3, 1);
                JS_FreeValue(jsCtx_, v1); JS_FreeValue(jsCtx_, v2); JS_FreeValue(jsCtx_, v3);
            }
            JS_FreeValue(jsCtx_, v0);
        }
        JS_FreeValue(jsCtx_, r);
    }
}

namespace {
inline void callCb(JSContext* ctx, JSValue fn, int argc, JSValue* argv) {
    if (!JS_IsFunction(ctx, fn)) return;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue r = JS_Call(ctx, fn, g, argc, argv);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, r);
}
} // namespace

void GizmoManager::fireTranslate(const Vec3& d) {
    if (!jsCtx_) return;
    JSValue args[3] = {
        JS_NewFloat64(jsCtx_, d.x),
        JS_NewFloat64(jsCtx_, d.y),
        JS_NewFloat64(jsCtx_, d.z),
    };
    callCb(jsCtx_, callbacks_[CB_Translate], 3, args);
    for (auto& a : args) JS_FreeValue(jsCtx_, a);
}

void GizmoManager::fireRotate(const Quat& q) {
    if (!jsCtx_) return;
    JSValue args[4] = {
        JS_NewFloat64(jsCtx_, q.x),
        JS_NewFloat64(jsCtx_, q.y),
        JS_NewFloat64(jsCtx_, q.z),
        JS_NewFloat64(jsCtx_, q.w),
    };
    callCb(jsCtx_, callbacks_[CB_Rotate], 4, args);
    for (auto& a : args) JS_FreeValue(jsCtx_, a);
}

void GizmoManager::fireScale(const Vec3& s) {
    if (!jsCtx_) return;
    JSValue args[3] = {
        JS_NewFloat64(jsCtx_, s.x),
        JS_NewFloat64(jsCtx_, s.y),
        JS_NewFloat64(jsCtx_, s.z),
    };
    callCb(jsCtx_, callbacks_[CB_Scale], 3, args);
    for (auto& a : args) JS_FreeValue(jsCtx_, a);
}

void GizmoManager::fireBegin() {
    if (!jsCtx_) return;
    callCb(jsCtx_, callbacks_[CB_BeginDrag], 0, nullptr);
}
void GizmoManager::fireEnd(bool /*committed*/) {
    if (!jsCtx_) return;
    callCb(jsCtx_, callbacks_[CB_EndDrag], 0, nullptr);
}
void GizmoManager::fireHoverChange() {
    if (!jsCtx_) return;
    callCb(jsCtx_, callbacks_[CB_HoverChange], 0, nullptr);
}

} // namespace bro::engine
