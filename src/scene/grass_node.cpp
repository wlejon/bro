#include "scene/grass_node.h"

#include "scene/scene_graph.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

namespace {

// A tapered strip of `seg` segments with a pointed tip: x across
// (-0.5..0.5), y along (0 root .. 1 tip), z the face. The normals lean out
// to the edges so a flat card shades like a folded blade.
bromesh::MeshData bladeMesh(int seg) {
    bromesh::MeshData m;
    for (int k = 0; k <= seg; ++k) {
        const float y = static_cast<float>(k) / static_cast<float>(seg);
        if (k == seg) {
            m.positions.insert(m.positions.end(), {0.0f, 1.0f, 0.0f});
            m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
            m.uvs.insert(m.uvs.end(), {0.5f, 1.0f});
            break;
        }
        const float w = 0.5f * std::pow(1.0f - y, 0.7f) + 0.02f;
        m.positions.insert(m.positions.end(), {-w, y, 0.0f, w, y, 0.0f});
        m.normals.insert(m.normals.end(), {-0.3f, 0.0f, 1.0f, 0.3f, 0.0f, 1.0f});
        m.uvs.insert(m.uvs.end(), {0.0f, y, 1.0f, y});
    }
    for (int k = 0; k < seg - 1; ++k) {
        const uint32_t a = static_cast<uint32_t>(k * 2);
        m.indices.insert(m.indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    const uint32_t t = static_cast<uint32_t>(seg * 2);
    m.indices.insert(m.indices.end(), {t - 2, t - 1, t});
    return m;
}

constexpr float kRingCell[GrassNode::kRings] = {0.06f, 0.2f, 0.55f};
constexpr float kRingEdge[GrassNode::kRings + 1] = {0.0f, 26.0f, 70.0f, 190.0f};   // at range 190

}  // namespace

GrassNode::GrassNode(const std::string& name) : MeshNode(name) {
    setMesh(bladeMesh(4));
    farBlade_ = bladeMesh(2);
    setTwoSided(true);
    setRoughness(0.7f);
    setMetallic(0.0f);
    setReceivesShadow(true);
    setCastsShadow(false);
    pushMap_.assign(static_cast<size_t>(kPushCells) * kPushCells * 4, 0.0f);
    setCustomShaderTexture(kPushTexture, kPushCells, kPushCells, pushMap_.data(), false, false, false, 4);
}

GrassNode::~GrassNode() = default;

void GrassNode::setParams(const Params& p) {
    params_ = p;
    params_.height = std::max(p.height, 0.01f);
    params_.width = std::max(p.width, 0.0f);
    params_.density = std::clamp(p.density, 0.05f, 4.0f);
    params_.range = std::clamp(p.range, 5.0f, 2000.0f);
    params_.minNormalY = std::clamp(p.minNormalY, 0.0f, 1.0f);
    params_.bulk = std::clamp(p.bulk, 0.0f, 1.0f);
    params_.variety = std::clamp(p.variety, 0.0f, 1.0f);
    params_.wind = std::max(p.wind, 0.0f);
    bumpChangeGeneration();
}

void GrassNode::paramsToArray(float (&o)[kParamCount]) const {
    const Params& p = params_;
    const float v[kParamCount] = {p.height,  p.width,  p.density,   p.range,     p.minNormalY, p.lush[0],
                                  p.lush[1], p.lush[2], p.dry[0],   p.dry[1],    p.dry[2],     p.bulk,
                                  p.clumpFreq, p.rootShade, p.sheen, p.variety, p.wind};
    std::copy(std::begin(v), std::end(v), o);
}

void GrassNode::paramsFromArray(const double* v, size_t n) {
    float a[kParamCount];
    paramsToArray(a);
    for (size_t i = 0; i < n && i < static_cast<size_t>(kParamCount); ++i) {
        if (!std::isnan(v[i])) a[i] = static_cast<float>(v[i]);
    }
    Params p;
    p.height = a[0];
    p.width = a[1];
    p.density = a[2];
    p.range = a[3];
    p.minNormalY = a[4];
    std::copy(a + 5, a + 8, p.lush);
    std::copy(a + 8, a + 11, p.dry);
    p.bulk = a[11];
    p.clumpFreq = a[12];
    p.rootShade = a[13];
    p.sheen = a[14];
    p.variety = a[15];
    p.wind = a[16];
    setParams(p);
}

GrassNode::Ring GrassNode::ring(int i) const {
    Ring r;
    i = std::clamp(i, 0, kRings - 1);
    const float scale = params_.range / kRingEdge[kRings];
    const float cellScale = 1.0f / std::sqrt(params_.density);
    r.cell = kRingCell[i] * cellScale;
    r.inner = kRingEdge[i] * scale;
    r.outer = kRingEdge[i + 1] * scale;
    const auto band = [](float edge) { return std::max(3.0f, edge * 0.12f); };
    r.bandIn = i == 0 ? 0.0f : band(r.inner);
    r.bandOut = band(r.outer);
    r.width = params_.width + (i == 0 ? 0.016f : kRingCell[i] * 0.2f);
    return r;
}

void GrassNode::setField(const float* rgba, int cols, int rows, float x0, float z0, float cell) {
    if (!rgba || cols < 2 || rows < 2 || !(cell > 0.0f)) {
        field_.clear();
        fieldCols_ = fieldRows_ = 0;
        setCustomShaderTexture(kFieldTexture, 0, 0, nullptr);
        return;
    }
    field_.assign(rgba, rgba + static_cast<size_t>(cols) * rows * 4);
    fieldCols_ = cols;
    fieldRows_ = rows;
    fieldX0_ = x0;
    fieldZ0_ = z0;
    fieldCell_ = cell;
    setCustomShaderTexture(kFieldTexture, cols, rows, field_.data(), false, false, false, 4);
}

float GrassNode::fieldHeight(float x, float z, float fallback) const {
    if (!hasField()) return fallback;
    const float fx = (x - fieldX0_) / fieldCell_, fz = (z - fieldZ0_) / fieldCell_;
    if (fx < 0.0f || fz < 0.0f || fx > static_cast<float>(fieldCols_ - 1) || fz > static_cast<float>(fieldRows_ - 1))
        return fallback;
    const int i = std::min(static_cast<int>(fx), fieldCols_ - 2), j = std::min(static_cast<int>(fz), fieldRows_ - 2);
    const float u = fx - static_cast<float>(i), v = fz - static_cast<float>(j);
    const auto h = [&](int a, int b) { return field_[(static_cast<size_t>(b) * fieldCols_ + a) * 4]; };
    const float h0 = h(i, j) + (h(i + 1, j) - h(i, j)) * u;
    const float h1 = h(i, j + 1) + (h(i + 1, j + 1) - h(i, j + 1)) * u;
    return h0 + (h1 - h0) * v;
}

void GrassNode::setMover(uint32_t nodeId, float radius, float strength, float trample) {
    auto it = std::find_if(movers_.begin(), movers_.end(), [&](const Mover& m) { return m.nodeId == nodeId; });
    if (!(radius > 0.0f)) {
        if (it != movers_.end()) movers_.erase(it);
        return;
    }
    if (it == movers_.end()) {
        movers_.push_back({});
        it = movers_.end() - 1;
        it->nodeId = nodeId;
    }
    it->radius = radius;
    it->strength = std::max(strength, 0.0f);
    it->trample = std::clamp(trample, 0.0f, 1.0f);
}

void GrassNode::push(float x, float z, float radius, float vx, float vz, float strength, float trample) {
    if (!(radius > 0.0f) || pushes_.size() >= 256) return;
    pushes_.push_back({x, z, radius, vx, vz, std::max(strength, 0.0f), std::clamp(trample, 0.0f, 1.0f)});
}

void GrassNode::recentre(float eyeX, float eyeZ) {
    const int ox = static_cast<int>(std::floor(eyeX / kPushCell)) - kPushCells / 2;
    const int oz = static_cast<int>(std::floor(eyeZ / kPushCell)) - kPushCells / 2;
    if (pushPlaced_ && std::abs(ox - pushOx_) <= kPushCells / 8 && std::abs(oz - pushOz_) <= kPushCells / 8) return;
    const int dx = ox - pushOx_, dz = oz - pushOz_;
    pushOx_ = ox;
    pushOz_ = oz;
    const bool wasPlaced = pushPlaced_;
    pushPlaced_ = true;
    if (!pushUsed_ || !wasPlaced) return;   // an all-zero map stays all zero
    std::vector<float> moved(pushMap_.size(), 0.0f);
    for (int j = 0; j < kPushCells; ++j) {
        const int sj = j + dz;
        if (sj < 0 || sj >= kPushCells) continue;
        for (int i = 0; i < kPushCells; ++i) {
            const int si = i + dx;
            if (si < 0 || si >= kPushCells) continue;
            std::copy_n(&pushMap_[(static_cast<size_t>(sj) * kPushCells + si) * 4], 4,
                        &moved[(static_cast<size_t>(j) * kPushCells + i) * 4]);
        }
    }
    pushMap_.swap(moved);
    setCustomShaderTexture(kPushTexture, kPushCells, kPushCells, pushMap_.data(), false, false, false, 4);
}

void GrassNode::stampDisc(float x, float z, float radius, float vx, float vz, float strength, float trample,
                          float dt, float now, int& minI, int& minJ, int& maxI, int& maxJ) {
    const int i0 = std::max(static_cast<int>(std::floor((x - radius) / kPushCell)) - pushOx_, 0);
    const int i1 = std::min(static_cast<int>(std::floor((x + radius) / kPushCell)) - pushOx_, kPushCells - 1);
    const int j0 = std::max(static_cast<int>(std::floor((z - radius) / kPushCell)) - pushOz_, 0);
    const int j1 = std::min(static_cast<int>(std::floor((z + radius) / kPushCell)) - pushOz_, kPushCells - 1);
    if (i0 > i1 || j0 > j1) return;
    const float stampTime = std::max(now, 1e-3f);
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const float cx = (static_cast<float>(pushOx_ + i) + 0.5f) * kPushCell;
            const float cz = (static_cast<float>(pushOz_ + j) + 0.5f) * kPushCell;
            const float ddx = cx - x, ddz = cz - z;
            const float d = std::sqrt(ddx * ddx + ddz * ddz);
            if (d > radius) continue;
            float k = 1.0f - d / radius;
            k *= k;
            float* c = &pushMap_[(static_cast<size_t>(j) * kPushCells + i) * 4];
            // What the cell holds now, decayed since its last stamp (the
            // shader's grassInfluence).
            float bx = 0.0f, bz = 0.0f, tr = 0.0f;
            const float age = now - c[3];
            if (c[3] > 0.0f && age >= 0.0f) {
                const float e = std::exp(-age * kBendRecovery);
                bx = c[0] * e;
                bz = c[1] * e;
                tr = std::max(c[2] - age * kTrampleRecovery, 0.0f);
            }
            const float nx = d > 1e-3f ? ddx / d : 0.0f, nz = d > 1e-3f ? ddz / d : 0.0f;
            const float s = k * strength * dt * 12.0f;
            bx += (nx * 0.7f + vx * 0.25f) * s;
            bz += (nz * 0.7f + vz * 0.25f) * s;
            const float bl = std::sqrt(bx * bx + bz * bz);
            if (bl > 1.0f) {
                bx /= bl;
                bz /= bl;
            }
            c[0] = bx;
            c[1] = bz;
            c[2] = std::max(tr, k * trample);
            c[3] = stampTime;
        }
    }
    minI = std::min(minI, i0);
    minJ = std::min(minJ, j0);
    maxI = std::max(maxI, i1);
    maxJ = std::max(maxJ, j1);
    pushUsed_ = true;
}

void GrassNode::stamp(SceneGraph& graph, float now, float eyeX, float eyeZ) {
    recentre(eyeX, eyeZ);
    // A second render in the same frame (or a paused clock) stamps nothing
    // and keeps the pushes for the next step.
    if (lastNow_ >= 0.0f && now == lastNow_) return;
    const float dt = lastNow_ < 0.0f ? 0.0f : now - lastNow_;
    lastNow_ = now;
    if (!(dt > 0.0f) || dt > 1.0f) {
        // The first frame, a jump or a clock reset: nothing moves through the
        // grass this frame; the movers start their paths from here.
        for (Mover& m : movers_) m.seen = false;
        pushes_.clear();
        return;
    }
    int minI = kPushCells, minJ = kPushCells, maxI = -1, maxJ = -1;
    for (auto it = movers_.begin(); it != movers_.end();) {
        SceneNode* n = graph.resolveNode(it->nodeId);
        if (!n) {
            it = movers_.erase(it);
            continue;
        }
        const bromath::Mat4& w = n->worldMatrix();
        const float x = w.at(0, 3), z = w.at(2, 3);
        Mover& m = *it++;
        if (!m.seen) {
            m.seen = true;
            m.lastX = x;
            m.lastZ = z;
            m.vx = m.vz = 0.0f;
        }
        const float ex = x - m.lastX, ez = z - m.lastZ;
        const float len = std::sqrt(ex * ex + ez * ez);
        // Smoothed velocity: a jittery path still bends the grass along it.
        m.vx += (ex / dt - m.vx) * 0.3f;
        m.vz += (ez / dt - m.vz) * 0.3f;
        if (len > 50.0f) {   // a teleport, not a path
            m.lastX = x;
            m.lastZ = z;
            continue;
        }
        // Along the path travelled this frame, so a fast mover leaves no gaps.
        const int steps = std::clamp(static_cast<int>(std::ceil(len / std::max(m.radius * 0.5f, kPushCell))), 1, 32);
        for (int s = 1; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            stampDisc(m.lastX + ex * t, m.lastZ + ez * t, m.radius, m.vx, m.vz, m.strength, m.trample,
                      dt / static_cast<float>(steps), now, minI, minJ, maxI, maxJ);
        }
        m.lastX = x;
        m.lastZ = z;
    }
    for (const Push& p : pushes_)
        stampDisc(p.x, p.z, p.radius, p.vx, p.vz, p.strength, p.trample, dt, now, minI, minJ, maxI, maxJ);
    pushes_.clear();
    if (maxI < minI || maxJ < minJ) return;

    const int w = maxI - minI + 1, h = maxJ - minJ + 1;
    std::vector<float> rect(static_cast<size_t>(w) * h * 4);
    for (int j = 0; j < h; ++j) {
        std::copy_n(&pushMap_[(static_cast<size_t>(minJ + j) * kPushCells + minI) * 4], static_cast<size_t>(w) * 4,
                    &rect[static_cast<size_t>(j) * w * 4]);
    }
    updateCustomShaderTexture(kPushTexture, minI, minJ, w, h, rect.data());
}

}  // namespace bro::scene
