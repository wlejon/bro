#include "scene/nodes/grass_node.h"

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

// `copies` of `blade` end to end: one instance draws them all, copy k being
// the instance's blade k (the shader finds k from the vertex index).
bromesh::MeshData repeatBlade(const bromesh::MeshData& blade, int copies) {
    bromesh::MeshData m;
    const uint32_t vc = static_cast<uint32_t>(blade.vertexCount());
    for (int c = 0; c < copies; ++c) {
        m.positions.insert(m.positions.end(), blade.positions.begin(), blade.positions.end());
        m.normals.insert(m.normals.end(), blade.normals.begin(), blade.normals.end());
        m.uvs.insert(m.uvs.end(), blade.uvs.begin(), blade.uvs.end());
        for (uint32_t i : blade.indices) m.indices.push_back(static_cast<uint32_t>(c) * vc + i);
    }
    return m;
}

// The rings at the default range (150 m; edges scale with `range`, cells
// with 1 / sqrt(density)). Blades a square metre x width is the cover: about
// 1400 x 4 mm near, 310 x 1.6 cm, 67 x 7.6 cm, 10 x 20 cm (before the width's
// growth with distance), so the sward stays about as covered as it recedes
// (the far ring half as much: its leaves are fans, scene_grass.glsl) while a
// ring's vertices a square metre fall four- to tenfold each step.
struct RingSpec {
    float cell;       // m
    float edge;       // outer edge (m)
    int blades;       // an instance
    int segments;
    float spread;     // x the cell
    float fan;        // rad
    float widthMul;   // x params.width
    float thinFrom;   // x the edge
    float minKeep;
};
constexpr RingSpec kRingSpec[GrassNode::kRings] = {
    {0.10f, 7.0f, 14, 4, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f},       // tufts of fine blades
    {0.16f, 20.0f, 8, 3, 0.5f, 0.35f, 4.0f, 0.5f, 0.5f},      // smaller tufts
    // Blades over the cell, as much cover (blades x width a square metre) as
    // the ring inside keeps at its outer edge, there thinned to half: half
    // as many again, twice as wide.
    {0.30f, 55.0f, 6, 2, 0.6f, 0.0f, 19.0f, 1.0f, 1.0f},
    // Far fans of leaves (scene_grass.glsl's fan cards): more cover than the
    // 9 x 10 cm it had (its gaps showed the turf, 4-5% darker than ring 2
    // over the same ground once the turf there read as the sward's depth),
    // in wider fans rather than more of them (every blade costs its vertices
    // whatever it covers).
    {0.7f, 150.0f, 4, 2, 0.6f, 0.0f, 62.0f, 1.0f, 1.0f},
};
constexpr float kRangeRef = 150.0f;

// A herb card: a strip of `seg` segments, x across (-0.5..0.5), y along (0
// .. 1), two vertices a row (scene_herb.glsl bends it into a leaf, a stem or
// a bloom's strip, or folds it into a bloom's disc).
bromesh::MeshData cardMesh(int seg) {
    bromesh::MeshData m;
    for (int k = 0; k <= seg; ++k) {
        const float y = static_cast<float>(k) / static_cast<float>(seg);
        m.positions.insert(m.positions.end(), {-0.5f, y, 0.0f, 0.5f, y, 0.0f});
        m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f});
        m.uvs.insert(m.uvs.end(), {0.0f, y, 1.0f, y});
    }
    for (int k = 0; k < seg; ++k) {
        const uint32_t a = static_cast<uint32_t>(k * 2);
        m.indices.insert(m.indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    return m;
}

// The herb rings: near, every card of a plant in five segments; out to the
// plants' range, half its cards (blooms and stems first, a share of the
// leaves widened) in two; then to the far edge three cards in two segments
// (a fern's largest fronds; a flower's bloom, stem and a leaf), the plants
// thinned with distance and widened to keep their cover.
struct HerbRingSpec {
    float edge;   // the outer edge, x the herbs' range (the far ring's is herbFar)
    int cards;
    int segments;
};
constexpr HerbRingSpec kHerbRingSpec[GrassNode::kHerbRings] = {
    {0.34f, 14, 5},
    {1.0f, 7, 2},
    {0.0f, 3, 1},   // the far plants: a few pixels each, one quad a card
};

}  // namespace

GrassNode::GrassNode(const std::string& name) : MeshNode(name) {
    setMesh(bladeMesh(4));
    for (int i = 0; i < kRings; ++i)
        ringMeshes_[i] = repeatBlade(bladeMesh(kRingSpec[i].segments), kRingSpec[i].blades);
    for (int i = 0; i < kHerbRings; ++i)
        herbMeshes_[i] = repeatBlade(cardMesh(kHerbRingSpec[i].segments), kHerbRingSpec[i].cards);
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
    params_.tufts = std::clamp(p.tufts, 0.0f, 1.0f);
    params_.flowering = std::clamp(p.flowering, 0.0f, 1.0f);
    params_.dead = std::clamp(p.dead, 0.0f, 1.0f);
    computeHerbMean();   // (what of the herbs shows over the grass: its height)
    bumpChangeGeneration();
}

void GrassNode::paramsToArray(float (&o)[kParamCount]) const {
    const Params& p = params_;
    const float v[kParamCount] = {p.height,  p.width,  p.density,   p.range,     p.minNormalY, p.lush[0],
                                  p.lush[1], p.lush[2], p.dry[0],   p.dry[1],    p.dry[2],     p.bulk,
                                  p.clumpFreq, p.rootShade, p.sheen, p.variety, p.wind,
                                  p.tufts, p.flowering, p.dead};
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
    p.tufts = a[17];
    p.flowering = a[18];
    p.dead = a[19];
    setParams(p);
}

GrassNode::Ring GrassNode::ring(int i) const {
    Ring r;
    i = std::clamp(i, 0, kRings - 1);
    const RingSpec& s = kRingSpec[i];
    const float scale = params_.range / kRangeRef;
    const float cellScale = 1.0f / std::sqrt(params_.density);
    r.cell = s.cell * cellScale;
    r.inner = i == 0 ? 0.0f : kRingSpec[i - 1].edge * scale;
    r.outer = s.edge * scale;
    const auto band = [](float edge) { return std::max(1.5f, edge * 0.12f); };
    r.bandIn = i == 0 ? 0.0f : band(r.inner);
    r.bandOut = band(r.outer);
    r.width = params_.width * s.widthMul;
    r.blades = s.blades;
    r.segments = s.segments;
    r.spread = s.spread * r.cell;
    r.fan = s.fan;
    r.thinFrom = s.thinFrom * r.outer;
    r.minKeep = s.minKeep;
    return r;
}

const bromesh::MeshData& GrassNode::ringMesh(int i) const { return ringMeshes_[std::clamp(i, 0, kRings - 1)]; }

GrassNode::HerbRing GrassNode::herbRing(int i) const {
    HerbRing r;
    i = std::clamp(i, 0, kHerbRings - 1);
    const auto band = [](float edge) { return std::max(1.0f, edge * 0.15f); };
    const auto edge = [&](int k) { return k == kHerbRings - 1 ? herbFar_ : kHerbRingSpec[k].edge * herbRange_; };
    r.outer = edge(i);
    r.inner = i == 0 ? 0.0f : edge(i - 1);
    r.bandIn = i == 0 ? 0.0f : band(r.inner);
    // The last ring hands over to the turf through the last quarter of the
    // far range (herbPlantsDrawn: the same band).
    r.bandOut = i == kHerbRings - 1 ? 0.25f * herbFar_ : band(r.outer);
    r.cards = kHerbRingSpec[i].cards;
    r.segments = kHerbRingSpec[i].segments;
    return r;
}

void GrassNode::setHerbs(const float* records, int count, float range, float far) {
    herbs_.clear();
    herbHeight_ = 0.0f;
    count = std::clamp(count, 0, kHerbMax);
    if (records && count > 0) {
        herbs_.assign(records, records + static_cast<size_t>(count) * kHerbRecordFloats);
        for (float& v : herbs_)
            if (!std::isfinite(v)) v = 0.0f;
        for (int s = 0; s < count; ++s) herbHeight_ = std::max(herbHeight_, herbs_[s * kHerbRecordFloats + 1]);
    }
    computeHerbMean();
    // (The grass gives the herbs room only while there are some.)
    if ((herbCount() > 0) != !fieldGpu_.empty()) uploadField();
    if (range > 0.0f) herbRange_ = std::clamp(range, 4.0f, 200.0f);
    if (far > 0.0f) herbFar_ = far;
    else if (range > 0.0f) herbFar_ = 3.0f * herbRange_;
    // The far ring is at least as wide as its hand-over band at either end.
    herbFar_ = std::clamp(herbFar_, herbRange_ * 1.4f, 800.0f);
    bumpChangeGeneration();
}

// scene_herb_mix.glsl's herbMixAt, averaged over every combination of the
// species' colonies (each species inside one with its patchCover's chance)
// rather than read from the noise at a point.
void GrassNode::computeHerbMean() {
    std::fill(std::begin(herbMean_), std::end(herbMean_), 0.0f);
    const int n = herbCount();
    if (n <= 0) return;
    const float cellArea = kHerbCell * kHerbCell;
    // Seen across, the grass hides what of the herbs stands lower than its
    // top (its blades arch: about 0.7 of their length up): a leaf's side as
    // far as its leaves' top rises over it, a bloom as it stands over it,
    // as the plants drawn among the blades are hidden.
    const float sward = 0.7f * params_.height;
    for (int step = 0; step < kHerbMeanSteps; ++step) {
        const float canopy = static_cast<float>(step) / (kHerbMeanSteps - 1);
        // A species' weight inside a colony, its ground per plant, its
        // chance of being inside one.
        // Its side silhouette per cell and its blooms' share of that, and
        // its leaves' top.
        float w[kHerbMax], a[kHerbMax], bs[kHerbMax], p[kHerbMax], sa[kHerbMax], sbs[kHerbMax];
        for (int i = 0; i < n; ++i) {
            const float* r = &herbs_[static_cast<size_t>(i) * kHerbRecordFloats];
            const float dl = (canopy - r[36]) / std::max(r[37], 0.05f);
            float light = std::exp(-dl * dl);
            if (light < 0.03f) light = 0.0f;
            const float spacing = r[48] > 0.0f ? r[48] : 1.6f * std::max(r[2], 0.02f);
            const float occupy = std::min(1.0f, (kHerbCell / spacing) * (kHerbCell / spacing));
            w[i] = r[38] > 0.0f ? r[38] * light * occupy : 0.0f;
            const float foot = 3.14159265f * r[2] * r[2] / cellArea;
            a[i] = w[i] * r[42] * foot;
            const float side = w[i] * std::max(r[49], 0.0f) * foot;
            const float sideBloom = side * std::clamp(r[50], 0.0f, 1.0f);
            const float leafOver = std::clamp(1.0f - sward / std::max(r[51], 0.01f), 0.0f, 1.0f);
            const float bloomOver = std::clamp((r[1] - 0.6f * sward) / (0.8f * sward), 0.0f, 1.0f);
            sa[i] = (side - sideBloom) * leafOver + sideBloom * bloomOver;
            bs[i] = std::clamp(r[41], 0.0f, 1.0f);
            sbs[i] = sa[i] > 0.0f ? sideBloom * bloomOver / sa[i] : 0.0f;
            p[i] = std::clamp(r[40], 0.0f, 1.0f);
        }
        double expo = 0.0, coverW = 0.0, bloomW = 0.0, present = 0.0, sideX = 0.0, sideBloom = 0.0, top = 0.0;
        double leaf[3] = {}, bloom[3] = {}, leafW = 0.0;
        for (uint32_t set = 0; set < (1u << n); ++set) {
            double prob = 1.0;
            float sum = 0.0f, area = 0.0f, leafA = 0.0f, bloomA = 0.0f, side = 0.0f, sideB = 0.0f, hgt = 0.0f;
            float lc[3] = {}, bc[3] = {};
            for (int i = 0; i < n; ++i) {
                const bool in = (set >> i) & 1u;
                prob *= in ? p[i] : 1.0f - p[i];
                if (!in || w[i] <= 0.0f) continue;
                const float* r = &herbs_[static_cast<size_t>(i) * kHerbRecordFloats];
                sum += w[i];
                area += a[i];
                side += sa[i];
                sideB += sa[i] * sbs[i];
                hgt += w[i] * r[51];
                leafA += a[i] * (1.0f - bs[i]);
                bloomA += a[i] * bs[i];
                for (int c = 0; c < 3; ++c) {
                    lc[c] += a[i] * (1.0f - bs[i]) * r[16 + c];
                    bc[c] += a[i] * bs[i] * r[28 + c];
                }
            }
            if (prob <= 0.0) continue;
            if (sum <= 0.0f) { expo += prob; continue; }
            // The cover at full density (exp of minus it, averaged: the
            // exponent stands for the mean cover, not the mean exponent).
            const float k = std::min(sum, 1.0f) / sum;
            const float x = k * area;
            const double cover = 1.0 - std::exp(-static_cast<double>(x));
            expo += prob * std::exp(-static_cast<double>(x));
            coverW += prob * cover;
            // Seen from the side, where any herbs grow (the colonies' share
            // of the ground caps what a grazing view can see of them).
            present += prob;
            sideX += prob * k * side;
            sideBloom += prob * k * sideB;
            top += prob * hgt / sum;
            if (area <= 0.0f) continue;
            const double share = bloomA / area;
            bloomW += prob * cover * share;
            leafW += prob * cover * (1.0 - share);
            for (int c = 0; c < 3; ++c) {
                if (leafA > 0.0f) leaf[c] += prob * cover * (1.0 - share) * lc[c] / leafA;
                if (bloomA > 0.0f) bloom[c] += prob * cover * share * bc[c] / bloomA;
            }
        }
        float* o = &herbMean_[step * kHerbMeanFloats];
        // From above: the effective exponent where they grow, so that
        // present x (1 - exp(-it)) is the mean cover.
        const double inside = present > 1e-6 ? std::min((1.0 - expo) / present, 0.999) : 0.0;
        o[0] = static_cast<float>(-std::log(1.0 - inside));
        for (int c = 0; c < 3; ++c) {
            o[1 + c] = leafW > 0.0 ? static_cast<float>(leaf[c] / leafW) : 0.0f;
            o[5 + c] = bloomW > 0.0 ? static_cast<float>(bloom[c] / bloomW) : 0.0f;
        }
        o[4] = coverW > 0.0 ? static_cast<float>(bloomW / coverW) : 0.0f;
        o[8] = present > 1e-6 ? static_cast<float>(sideX / present) : 0.0f;
        o[9] = sideX > 1e-9 ? static_cast<float>(sideBloom / sideX) : 0.0f;
        o[10] = static_cast<float>(present);
        o[11] = present > 1e-6 ? static_cast<float>(top / present) : 0.0f;
    }
}

namespace {

// scene_grass.glsl's grassHash and scene_camera.glsl's sceneWindHash /
// sceneWindNoise, on the CPU (the same unsigned wrap-around).
float grassHashCpu(int cx, int cy, int k) {
    uint32_t h = static_cast<uint32_t>(cx) * 374761393u + static_cast<uint32_t>(cy) * 668265263u +
                 static_cast<uint32_t>(k) * 2246822519u;
    h = (h ^ (h >> 15)) * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h) / 4294967296.0f;
}
float windHashCpu(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h) / 4294967296.0f;
}
float windNoiseCpu(float px, float py) {
    const float ix = std::floor(px), iy = std::floor(py);
    const float fx = px - ix, fy = py - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    const int x = static_cast<int>(ix), y = static_cast<int>(iy);
    const float a = windHashCpu(x, y), b = windHashCpu(x + 1, y), c = windHashCpu(x, y + 1), d = windHashCpu(x + 1, y + 1);
    const float ab = a + (b - a) * ux, cd = c + (d - c) * ux;
    return (ab + (cd - ab) * uy) * 2.0f - 1.0f;
}
float smoothstepCpu(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

std::vector<GrassNode::HerbPlant> GrassNode::herbPlants(float x, float z, float radius, size_t max) const {
    std::vector<HerbPlant> out;
    if (!hasField() || !hasHerbs() || !(radius > 0.0f)) return out;
    const int n = herbCount();
    // The floor's two channels at world (px, pz), bilinear as the GPU samples.
    const auto floorAt = [&](float px, float pz, float& canopy, float& density) {
        const float fx = std::clamp((px - fieldX0_) / fieldCell_, 0.0f, static_cast<float>(fieldCols_ - 1));
        const float fz = std::clamp((pz - fieldZ0_) / fieldCell_, 0.0f, static_cast<float>(fieldRows_ - 1));
        const int i = std::min(static_cast<int>(fx), fieldCols_ - 2), j = std::min(static_cast<int>(fz), fieldRows_ - 2);
        const float u = fx - static_cast<float>(i), v = fz - static_cast<float>(j);
        float r[2];
        for (int c = 0; c < 2; ++c) {
            const auto t = [&](int a, int b) { return floor_[(static_cast<size_t>(b) * fieldCols_ + a) * 2 + c]; };
            const float h0 = t(i, j) + (t(i + 1, j) - t(i, j)) * u;
            const float h1 = t(i, j + 1) + (t(i + 1, j + 1) - t(i, j + 1)) * u;
            r[c] = h0 + (h1 - h0) * v;
        }
        canopy = r[0];
        density = r[1];
    };
    // scene_herb_mix.glsl's herbWeight.
    const auto weight = [&](int i, float px, float pz, float canopy) {
        const float* r = &herbs_[static_cast<size_t>(i) * kHerbRecordFloats];
        if (r[38] <= 0.0f) return 0.0f;
        const float dl = (canopy - r[36]) / std::max(r[37], 0.05f);
        const float light = std::exp(-dl * dl);
        if (light < 0.03f) return 0.0f;
        const float per = std::max(r[39], 0.5f);
        const float qx = px / per + static_cast<float>(i) * 37.13f + 11.0f;
        const float qz = pz / per + static_cast<float>(i) * -19.71f + 3.0f;
        const float nz = 0.5f + 0.33f * windNoiseCpu(qx, qz) + 0.17f * windNoiseCpu(qx * 2.7f + 5.1f, qz * 2.7f + 5.1f);
        const float t = 0.7f + (0.3f - 0.7f) * std::clamp(r[40], 0.0f, 1.0f);
        const float colony = smoothstepCpu(t - 0.07f, t + 0.07f, nz);
        const float spacing = r[48] > 0.0f ? r[48] : 1.6f * std::max(r[2], 0.02f);
        const float occupy = std::min(1.0f, (kHerbCell / spacing) * (kHerbCell / spacing));
        return r[38] * light * colony * occupy;
    };
    const int c0 = static_cast<int>(std::floor((x - radius) / kHerbCell)) - 1;
    const int c1 = static_cast<int>(std::floor((x + radius) / kHerbCell)) + 1;
    const int r0 = static_cast<int>(std::floor((z - radius) / kHerbCell)) - 1;
    const int r1 = static_cast<int>(std::floor((z + radius) / kHerbCell)) + 1;
    std::vector<std::pair<float, HerbPlant>> found;
    for (int cy = r0; cy <= r1; ++cy) {
        for (int cx = c0; cx <= c1; ++cx) {
            const float px = (static_cast<float>(cx) + grassHashCpu(cx, cy, 101)) * kHerbCell;
            const float pz = (static_cast<float>(cy) + grassHashCpu(cx, cy, 102)) * kHerbCell;
            const float d2 = (px - x) * (px - x) + (pz - z) * (pz - z);
            if (d2 > radius * radius) continue;
            const float fx = (px - fieldX0_) / fieldCell_, fz = (pz - fieldZ0_) / fieldCell_;
            if (fx < 0.0f || fz < 0.0f || fx > static_cast<float>(fieldCols_ - 1) || fz > static_cast<float>(fieldRows_ - 1))
                continue;
            float canopy = 0.0f, density = 0.0f;
            floorAt(px, pz, canopy, density);
            // herbPick.
            const float u0 = grassHashCpu(cx, cy, 105), u1 = grassHashCpu(cx, cy, 106);
            if (u0 >= density) continue;
            float w[kHerbMax] = {};
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) sum += (w[i] = weight(i, px, pz, canopy));
            if (sum <= 0.0f || u0 >= density * std::min(sum, 1.0f)) continue;
            float pick = u1 * sum;
            int sp = n - 1;
            for (int i = 0; i < n; ++i) {
                pick -= w[i];
                if (pick < 0.0f) { sp = i; break; }
            }
            found.push_back({d2, HerbPlant{px, fieldHeight(px, pz), pz, sp}});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t k = 0; k < found.size() && k < max; ++k) out.push_back(found[k].second);
    return out;
}

bool GrassNode::herbRangeAt(float x0, float z0, float x1, float z1, float& lo, float& hi) const {
    if (!hasField() || !hasHerbs()) return false;
    const float maxX = static_cast<float>(fieldCols_ - 1), maxZ = static_cast<float>(fieldRows_ - 1);
    const float fx0 = (x0 - fieldX0_) / fieldCell_, fx1 = (x1 - fieldX0_) / fieldCell_;
    const float fz0 = (z0 - fieldZ0_) / fieldCell_, fz1 = (z1 - fieldZ0_) / fieldCell_;
    if (fx1 < 0.0f || fz1 < 0.0f || fx0 > maxX || fz0 > maxZ) return false;
    const auto blockOf = [](float f, float maxF, int n) {
        return std::clamp(static_cast<int>(std::clamp(f, 0.0f, maxF)) / kFieldBlock, 0, n - 1);
    };
    const int bi0 = blockOf(fx0, maxX, blockCols_), bi1 = blockOf(fx1, maxX, blockCols_);
    const int bj0 = blockOf(fz0, maxZ, blockRows_), bj1 = blockOf(fz1, maxZ, blockRows_);
    lo = 1e30f;
    hi = -1e30f;
    bool grows = false;
    for (int bj = bj0; bj <= bj1; ++bj) {
        for (int bi = bi0; bi <= bi1; ++bi) {
            const float* b = &blocks_[(static_cast<size_t>(bj) * blockCols_ + bi) * kBlockFloats];
            if (b[3] <= 0.0f) continue;
            grows = true;
            lo = std::min(lo, b[0]);
            hi = std::max(hi, b[1]);
        }
    }
    return grows;
}

void GrassNode::setField(const float* rgba, int cols, int rows, float x0, float z0, float cell) {
    if (!rgba || cols < 2 || rows < 2 || !(cell > 0.0f)) {
        field_.clear();
        blocks_.clear();
        fieldCols_ = fieldRows_ = blockCols_ = blockRows_ = 0;
        setCustomShaderTexture(kFieldTexture, 0, 0, nullptr);
        return;
    }
    field_.assign(rgba, rgba + static_cast<size_t>(cols) * rows * 4);
    // A density floor: a texel thinner than this grows nothing (a few
    // blades in a closed wood's shade are not seen), so the blocks under a
    // canopy read as bare and their tiles are never issued.
    for (size_t k = 1; k < field_.size(); k += 4)
        if (field_[k] < kDensityFloor) field_[k] = 0.0f;
    fieldCols_ = cols;
    fieldRows_ = rows;
    fieldX0_ = x0;
    fieldZ0_ = z0;
    fieldCell_ = cell;
    if (floor_.size() != static_cast<size_t>(cols) * rows * 2) setFloor(nullptr, 0, 0);
    rebuildBlocks();
    uploadField();
}

// The field as the shaders read it: where herbs grow thick the grass gives
// them room, sparser and shorter with the floor's herb density (a meadow's
// forbs stand clear of it), worked into the density and height multiplier
// here rather than read again at every blade's vertex.
void GrassNode::uploadField() {
    const int cols = fieldCols_, rows = fieldRows_;
    if (cols < 2 || rows < 2) return;
    const size_t n = static_cast<size_t>(cols) * rows;
    const bool room = herbFloor_ && floor_.size() == n * 2 && herbCount() > 0;
    if (!room) {
        fieldGpu_.clear();
        setCustomShaderTexture(kFieldTexture, cols, rows, field_.data(), false, false, false, 4);
        return;
    }
    fieldGpu_ = field_;
    for (size_t k = 0; k < n; ++k) {
        const float h = floor_[k * 2 + 1];
        fieldGpu_[k * 4 + 1] *= 1.0f - 0.5f * h;
        fieldGpu_[k * 4 + 3] *= 1.0f - 0.45f * h;
    }
    setCustomShaderTexture(kFieldTexture, cols, rows, fieldGpu_.data(), false, false, false, 4);
}

void GrassNode::setFloor(const float* floor, int cols, int rows, int channels) {
    if (!floor || cols != fieldCols_ || rows != fieldRows_ || cols < 2 || rows < 2 || channels < 1 || channels > 2) {
        if (floor_.empty()) return;
        floor_.clear();
        herbFloor_ = false;
        setCustomShaderTexture(kFloorTexture, 0, 0, nullptr);
    } else {
        const size_t n = static_cast<size_t>(cols) * rows;
        floor_.assign(n * 2, 0.0f);
        for (size_t k = 0; k < n; ++k) {
            for (int c = 0; c < channels; ++c) {
                const float f = floor[k * channels + c];
                floor_[k * 2 + c] = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 0.0f;
            }
        }
        herbFloor_ = channels == 2;
        setCustomShaderTexture(kFloorTexture, cols, rows, floor_.data(), false, false, false, 2);
    }
    rebuildBlocks();
    uploadField();
}

void GrassNode::rebuildBlocks() {
    const int cols = fieldCols_, rows = fieldRows_;
    if (cols < 2 || rows < 2) return;
    // Block (bi, bj) covers texels bi * B .. bi * B + B inclusive, one past
    // its own, so the bilinear ground between two blocks is inside both.
    // Grass grows in it where the field's density and height do; herbs where
    // the floor has them (their own rings draw there).
    blockCols_ = (cols - 2) / kFieldBlock + 1;
    blockRows_ = (rows - 2) / kFieldBlock + 1;
    blocks_.assign(static_cast<size_t>(blockCols_) * blockRows_ * kBlockFloats, 0.0f);
    const bool herbs = herbFloor_ && !floor_.empty();
    for (int bj = 0; bj < blockRows_; ++bj) {
        for (int bi = 0; bi < blockCols_; ++bi) {
            float lo = 1e30f, hi = -1e30f, grows = 0.0f, herb = 0.0f;
            for (int j = bj * kFieldBlock; j <= std::min(bj * kFieldBlock + kFieldBlock, rows - 1); ++j) {
                for (int i = bi * kFieldBlock; i <= std::min(bi * kFieldBlock + kFieldBlock, cols - 1); ++i) {
                    const size_t k = static_cast<size_t>(j) * cols + i;
                    const float* t = &field_[k * 4];
                    lo = std::min(lo, t[0]);
                    hi = std::max(hi, t[0]);
                    if (t[1] > 0.0f && t[3] > 0.0f) grows = std::max(grows, t[3]);
                    if (herbs) herb = std::max(herb, floor_[k * 2 + 1]);
                }
            }
            float* b =&blocks_[(static_cast<size_t>(bj) * blockCols_ + bi) * kBlockFloats];
            b[0] = lo;
            b[1] = hi;
            b[2] = grows;
            b[3] = herb;
        }
    }
}

bool GrassNode::fieldRange(float x0, float z0, float x1, float z1, float& lo, float& hi, float& tall) const {
    if (!hasField()) return false;
    const float maxX = static_cast<float>(fieldCols_ - 1), maxZ = static_cast<float>(fieldRows_ - 1);
    const float fx0 = (x0 - fieldX0_) / fieldCell_, fx1 = (x1 - fieldX0_) / fieldCell_;
    const float fz0 = (z0 - fieldZ0_) / fieldCell_, fz1 = (z1 - fieldZ0_) / fieldCell_;
    if (fx1 < 0.0f || fz1 < 0.0f || fx0 > maxX || fz0 > maxZ) return false;
    const auto blockOf = [](float f, float maxF, int n) {
        return std::clamp(static_cast<int>(std::clamp(f, 0.0f, maxF)) / kFieldBlock, 0, n - 1);
    };
    const int bi0 = blockOf(fx0, maxX, blockCols_), bi1 = blockOf(fx1, maxX, blockCols_);
    const int bj0 = blockOf(fz0, maxZ, blockRows_), bj1 = blockOf(fz1, maxZ, blockRows_);
    lo = 1e30f;
    hi = -1e30f;
    tall = 0.0f;
    bool grows = false;
    for (int bj = bj0; bj <= bj1; ++bj) {
        for (int bi = bi0; bi <= bi1; ++bi) {
            const float* b = &blocks_[(static_cast<size_t>(bj) * blockCols_ + bi) * kBlockFloats];
            if (b[2] <= 0.0f) continue;
            grows = true;
            lo = std::min(lo, b[0]);
            hi = std::max(hi, b[1]);
            tall = std::max(tall, b[2]);
        }
    }
    return grows;
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

std::vector<GrassNode::MoverInfo> GrassNode::moverInfo() const {
    std::vector<MoverInfo> out;
    out.reserve(movers_.size());
    for (const Mover& m : movers_) out.push_back({m.nodeId, m.radius, m.strength, m.height});
    return out;
}

void GrassNode::setMover(uint32_t nodeId, float radius, float strength, float trample, float height) {
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
    it->height = std::max(height, 0.0f);
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

float GrassNode::bendRelease(float age) {
    const float w = 2.0f * 3.14159265f * kBendSpringHz, z = kBendSpringDamping;
    const float wd = w * std::sqrt(1.0f - z * z);
    return std::exp(-z * w * age) * (std::cos(wd * age) + z * w / wd * std::sin(wd * age));
}

float GrassNode::trampleRelease(float age) {
    const float a = age / kTrampleRelax, b = age / kTrampleResidueRelax;
    return (1.0f - kTrampleResidue) * (1.0f + a) * std::exp(-a) + kTrampleResidue * (1.0f + b) * std::exp(-b);
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
                const float e = bendRelease(age);
                bx = c[0] * e;
                bz = c[1] * e;
                tr = c[2] * trampleRelease(age);
            }
            const float nx = d > 1e-3f ? ddx / d : 0.0f, nz = d > 1e-3f ? ddz / d : 0.0f;
            // Built up over about a quarter second at the disc's middle (a
            // tall herb or culm swung over in a tenth reads as a jump).
            const float s = k * strength * dt * 7.0f;
            // Out from the mover at the disc's edge, along its path under its
            // body: the way out from its middle swings round as it passes
            // over a cell, and a flattened blade (or herb) laid that way would
            // swing across with it.
            const float side = 0.7f * (1.0f - 0.7f * k), along = 0.25f + 0.5f * k;
            bx += (nx * side + vx * along) * s;
            bz += (nz * side + vz * along) * s;
            const float bl = std::sqrt(bx * bx + bz * bz);
            if (bl > 1.0f) {
                bx /= bl;
                bz /= bl;
            }
            c[0] = bx;
            c[1] = bz;
            // Flattened over a quarter second at least, not at a stroke.
            c[2] = std::max(tr, std::min(k * trample, tr + dt * 4.0f));
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
