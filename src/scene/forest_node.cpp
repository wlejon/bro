#include "scene/forest_node.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace bro::scene {

namespace {

constexpr size_t kSpecimenFloats = 16;   // emitForest's binding: see setSpecimens
constexpr size_t kHeaderFloats = 16;
constexpr size_t kSpecimenRecords = 6;   // scene_forest.glsl's FOREST_SPECIMEN_RECORDS

float smooth(float a, float b, float x) {
    if (a == b) return x < a ? 0.0f : 1.0f;
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float bitsOf(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// A specimen's `count` clumps (centre(i) each) ordered so that every run of
// ForestNode::kClumpBlock lies together: the whole split along its widest
// axis at the run boundary nearest its middle, each half again, until a
// part is one run. A crown's ray skips a run whose sphere it misses, and
// the clumps as grown (outermost first) made every run's sphere most of
// the crown, so a ray met nearly every clump.
template <typename Centre>
std::vector<uint32_t> groupClumps(uint32_t count, const Centre& centre) {
    constexpr uint32_t B = ForestNode::kClumpBlock;
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    std::vector<bromath::Vec3> at(count);
    for (uint32_t i = 0; i < count; ++i) at[i] = centre(i);
    std::vector<std::pair<uint32_t, uint32_t>> parts{{0u, count}};
    while (!parts.empty()) {
        const auto [lo, hi] = parts.back();
        parts.pop_back();
        if (hi - lo <= B) continue;
        bromath::Vec3 mn = at[order[lo]], mx = mn;
        for (uint32_t i = lo; i < hi; ++i) {
            const bromath::Vec3& p = at[order[i]];
            mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
            mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
        }
        const float ex = mx.x - mn.x, ey = mx.y - mn.y, ez = mx.z - mn.z;
        const int axis = ex >= ey && ex >= ez ? 0 : ey >= ez ? 1 : 2;
        const auto key = [&](uint32_t i) { const bromath::Vec3& p = at[i]; return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
        // The run boundary nearest the middle (lo is on one).
        const uint32_t runs = (hi - lo + B - 1) / B;
        const uint32_t mid = lo + std::max(1u, runs / 2) * B;
        std::nth_element(order.begin() + lo, order.begin() + mid, order.begin() + hi,
                         [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
        parts.push_back({lo, mid});
        parts.push_back({mid, hi});
    }
    return order;
}

// A unit vector in two floats (octahedral): scene_procedural.glsl's
// procOctDecode reads it.
void octEncode(float x, float y, float z, float& u, float& v) {
    const float l = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (l < 1e-12f) { u = 0.0f; v = 0.0f; return; }
    x /= l; y /= l; z /= l;
    if (z < 0.0f) {
        const float ox = x, oy = y;
        x = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        y = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    }
    u = x;
    v = y;
}

bromath::AABB3 grow(const bromath::AABB3& b, const bromath::Vec3& c, float r) {
    return bromath::amerge(b, bromath::AABB3{{c.x - r, c.y - r, c.z - r}, {c.x + r, c.y + r, c.z + r}});
}

}  // namespace

ForestNode::ForestNode(const std::string& name) : MeshNode(name) {
    forestBounds_ = bromath::aempty3();
}

ForestNode::~ForestNode() = default;

bool ForestNode::setSpecimens(SpecimenArrays&& a) {
    const size_t nSpec = a.specimens.size() / kSpecimenFloats;
    const size_t nSeg = a.segments.size() / 8;
    const size_t nClump = a.clumps.size() / 8;
    const size_t nLeaf = a.leaves.size() / 2;
    if (a.specimens.size() % kSpecimenFloats || a.segments.size() % 8 || a.tangents.size() != nSeg * 6 ||
        a.segmentVegetation.size() != nSeg * 16 || a.keepRadius.size() != nSeg || a.clumps.size() % 8 ||
        a.clumpVegetation.size() != nClump * 16 || a.leaves.size() % 2)
        return false;
    for (size_t i = 0; i < nLeaf; ++i)
        if (a.leaves[2 * i] >= nClump) return false;

    std::vector<Specimen> specs(nSpec);
    for (size_t k = 0; k < nSpec; ++k) {
        const float* f = &a.specimens[k * kSpecimenFloats];
        Specimen& s = specs[k];
        const auto u = [&](int i) { return static_cast<uint32_t>(std::max(0.0f, f[i])); };
        s.segStart = u(0); s.segCount = u(1);
        s.clumpStart = u(2); s.clumpCount = u(3);
        s.leafStart = u(4); s.leafCount = u(5);
        s.height = f[6];
        // Its clumps' height over their width (0: emitted before it existed).
        s.clumpFlat = f[15] > 0.0f ? std::clamp(f[15], 0.2f, 1.0f) : 1.0f;
        if (uint64_t{s.segStart} + s.segCount > nSeg || uint64_t{s.clumpStart} + s.clumpCount > nClump ||
            uint64_t{s.leafStart} + s.leafCount > nLeaf)
            return false;

        // The crown: the extent of its clumps; the bounds: bark and clumps.
        bromath::AABB3 crown = bromath::aempty3();
        float sumR = 0.0f;
        for (uint32_t c = s.clumpStart; c < s.clumpStart + s.clumpCount; ++c) {
            const float* r = &a.clumps[c * 8];
            // Its ellipsoid: r across, 0.9 r up and 0.7 r down times the flatness.
            const float up = r[3] * s.clumpFlat;
            crown = bromath::amerge(crown, bromath::AABB3{{r[0] - r[3], r[1] - 0.7f * up, r[2] - r[3]},
                                                          {r[0] + r[3], r[1] + 0.9f * up, r[2] + r[3]}});
            sumR += r[3];
        }
        s.bounds = crown;
        for (uint32_t g = s.segStart; g < s.segStart + s.segCount; ++g) {
            const float* r = &a.segments[g * 8];
            s.bounds = grow(s.bounds, {r[0], r[1], r[2]}, r[3]);
            s.bounds = grow(s.bounds, {r[4], r[5], r[6]}, r[7]);
            s.trunkRadius = std::max(s.trunkRadius, r[3]);
        }
        if (bromath::aisEmpty(s.bounds)) s.bounds = bromath::AABB3{{0.0f, 0.0f, 0.0f}, {0.0f, s.height, 0.0f}};
        if (s.clumpCount > 0) {
            s.crown = (crown.min + crown.max) * 0.5f;
            const bromath::Vec3 h = (crown.max - crown.min) * 0.5f;
            // The cutout's ragged edge reaches past the disc's middle radius.
            s.crownRadius = 0.92f * std::max(h.x, h.z);
            s.crownRadiusUp = 0.92f * h.y;
            s.meanClumpRadius = sumR / static_cast<float>(s.clumpCount);
            float sumW = 0.0f, best = 1e30f;
            s.crownClump = s.clumpStart;
            for (uint32_t c = s.clumpStart; c < s.clumpStart + s.clumpCount; ++c) {
                sumW += a.clumpVegetation[c * 16 + 9];   // weightTo (broflora appendSegmentVegetation)
                const float* r = &a.clumps[c * 8];
                const float dx = r[0] - s.crown.x, dz = r[2] - s.crown.z;
                if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; s.crownClump = c; }
            }
            s.meanClumpWeight = std::clamp(sumW / static_cast<float>(s.clumpCount), 0.0f, 1.0f);
        } else {
            s.crown = {f[8], f[9], f[10]};
            s.crownRadius = s.crownRadiusUp = f[7];
        }
    }

    specimens_ = std::move(specs);
    if (looks_.size() != specimens_.size() * 8) looks_.clear();
    keepRadius_ = a.keepRadius;
    data_ = std::move(a);
    // Rows naming a specimen that no longer exists go.
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
                                    [&](const Instance& i) { return i.specimen >= specimens_.size(); }),
                     instances_.end());
    rebuildBounds();
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
    return true;
}

void ForestNode::setInstances(const float* rows, size_t count) {
    instances_.clear();
    instances_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const float* r = rows + i * 8;
        if (!(r[5] >= 0.0f) || static_cast<size_t>(r[5]) >= specimens_.size() || !(r[3] > 0.0f)) continue;
        Instance in;
        in.position = {r[0], r[1], r[2]};
        in.scale = r[3];
        in.cosYaw = std::cos(r[4]);
        in.sinYaw = std::sin(r[4]);
        in.specimen = static_cast<uint32_t>(r[5]);
        // A seed from where it stands.
        const float h = std::sin(r[0] * 12.9898f + r[2] * 78.233f) * 43758.5453f;
        in.seed = h - std::floor(h);
        instances_.push_back(in);
    }
    rebuildBounds();
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
}

void ForestNode::rebuildBounds() {
    forestBounds_ = bromath::aempty3();
    for (Instance& in : instances_) {
        const bromath::AABB3& b = specimens_[in.specimen].bounds;
        in.bounds = bromath::aempty3();
        for (int k = 0; k < 8; ++k) {
            const float x = (k & 1) ? b.max.x : b.min.x;
            const float y = (k & 2) ? b.max.y : b.min.y;
            const float z = (k & 4) ? b.max.z : b.min.z;
            const bromath::Vec3 p{in.position.x + in.scale * (x * in.cosYaw + z * in.sinYaw),
                                  in.position.y + in.scale * y,
                                  in.position.z + in.scale * (-x * in.sinYaw + z * in.cosYaw)};
            in.bounds = bromath::amerge(in.bounds, bromath::AABB3{p, p});
        }
        forestBounds_ = bromath::amerge(forestBounds_, in.bounds);
    }
}

void ForestNode::lodToArray(float (&out)[kLodCount]) const {
    const Lod& l = lod_;
    const float v[kLodCount] = {l.leafPixels, l.flutterFade, l.cardWidening, l.clumpFrom, l.clumpTo, l.crownFrom,
                                l.crownTo, l.barkPixels, l.barkSidesPixels, l.shadowKeep, l.range,
                                l.budget, l.shadowBudget, l.clusters, l.barkKeepPixels};
    std::copy(std::begin(v), std::end(v), out);
}

void ForestNode::lodFromArray(const double* v, size_t n) {
    float* dst[kLodCount] = {&lod_.leafPixels, &lod_.flutterFade, &lod_.cardWidening, &lod_.clumpFrom,
                             &lod_.clumpTo, &lod_.crownFrom, &lod_.crownTo, &lod_.barkPixels,
                             &lod_.barkSidesPixels, &lod_.shadowKeep, &lod_.range, &lod_.budget,
                             &lod_.shadowBudget, &lod_.clusters, &lod_.barkKeepPixels};
    for (size_t i = 0; i < n && i < static_cast<size_t>(kLodCount); ++i)
        if (!std::isnan(v[i])) *dst[i] = static_cast<float>(v[i]);
    lod_.leafPixels = std::max(lod_.leafPixels, 0.01f);
    lod_.flutterFade = std::max(lod_.flutterFade, 1.01f);
    lod_.cardWidening = std::max(lod_.cardWidening, 1.0f);
    lod_.clumpFrom = std::max(lod_.clumpFrom, 1.0f);
    lod_.clumpTo = std::max(lod_.clumpTo, lod_.clumpFrom);
    lod_.crownTo = std::max(lod_.crownTo, 0.0f);
    lod_.crownFrom = std::max(lod_.crownFrom, lod_.crownTo);
    lod_.shadowKeep = std::clamp(lod_.shadowKeep, 0.01f, 1.0f);
    lod_.range = std::max(lod_.range, 0.0f);
    lod_.budget = std::max(lod_.budget, 0.0f);
    lod_.shadowBudget = std::max(lod_.shadowBudget, 0.0f);
    lod_.clusters = lod_.clusters > 0.5f ? 1.0f : 0.0f;
    lod_.barkKeepPixels = std::max(lod_.barkKeepPixels, 0.0f);
}

float ForestNode::fadeCode(float lo, float hi) {
    lo = std::clamp(lo, 0.0f, 1.0f);
    hi = std::clamp(hi, 0.0f, 1.0f);
    if (lo <= 0.0f && hi >= 1.0f) return 1.0f;
    return 2.0f * std::round(lo * 4096.0f) + hi;
}

void ForestNode::fadeRanges(const TreeLod& t, float (&lo)[kTiers], float (&hi)[kTiers]) {
    // Crowns first, then clumps, then leaves to the top.
    const int order[kTiers] = {2, 1, 0};
    const float w[kTiers] = {t.leaves, t.clumps, t.crowns};
    float at = 0.0f;
    int last = -1;
    for (int k : order) {
        lo[k] = at;
        hi[k] = k == 0 ? (w[0] > 0.0f ? 1.0f : at) : at + std::max(w[k], 0.0f);
        at = hi[k];
        if (w[k] > 0.0f) last = k;
    }
    // A tier drawn alone covers everything; so does the top one (rounding).
    for (int k = 0; k < kTiers; ++k)
        if (w[k] >= 1.0f) { lo[k] = 0.0f; hi[k] = 1.0f; }
    if (last >= 0 && w[last] > 0.0f) hi[last] = 1.0f;
    int drawn = 0;
    for (int k = 0; k < kTiers; ++k) drawn += w[k] > 0.0f ? 1 : 0;
    if (drawn == 1)
        for (int k = 0; k < kTiers; ++k)
            if (w[k] > 0.0f) lo[k] = 0.0f;
}

ForestNode::TreeLod ForestNode::treeLod(const Lod& lod, const Specimen& s, float px, float leafLength,
                                        int forceTier, float barkPx) {
    TreeLod t;
    px = std::max(px, 1e-6f);
    barkPx = barkPx > 0.0f ? barkPx : px;
    t.barkRadius = lod.barkPixels / (2.0f * barkPx);
    t.barkSides = 2.0f * s.trunkRadius * barkPx > lod.barkSidesPixels ? 10 : 5;
    if (forceTier >= 0) {
        t.leaves = forceTier == 0 ? 1.0f : 0.0f;
        t.clumps = forceTier == 1 ? 1.0f : 0.0f;
        t.crowns = forceTier == 2 ? 1.0f : 0.0f;
        if (forceTier == 0) t.barkRadius = 0.0f;
        return t;
    }
    const float leafPx = std::max(leafLength * px, 1e-6f);
    t.widening = std::max(1.0f, lod.leafPixels / leafPx);
    t.keep = 1.0f / (t.widening * t.widening);
    // Twigs go as the leaves widen: a twig narrower than the leaves' widening
    // times `barkPixels` is lost in the foliage drawn over it (and, past the
    // leaves, in the clumps and crowns), so bark falls away as fast as the
    // leaves thin, down to the trunk and scaffold limbs.
    t.barkRadius *= t.widening;
    // ... but never past what anyone would see go: bark wider than
    // `barkKeepPixels` stays.
    if (lod.barkKeepPixels > 0.0f) t.barkRadius = std::min(t.barkRadius, lod.barkKeepPixels / (2.0f * barkPx));
    t.card = t.widening > lod.cardWidening;
    const float toClumps = smooth(lod.clumpFrom, lod.clumpTo, t.widening);
    // Crowns take over as a clump shrinks on screen (crownFrom > crownTo).
    const float toCrowns = s.clumpCount > 0 ? 1.0f - smooth(lod.crownTo, lod.crownFrom, s.meanClumpRadius * px) : 0.0f;
    t.crowns = toCrowns;
    t.clumps = s.clumpCount > 0 ? toClumps * (1.0f - toCrowns) : 0.0f;
    t.leaves = s.clumpCount > 0 ? (1.0f - toClumps) * (1.0f - toCrowns) : 1.0f;
    return t;
}

float ForestNode::leafLength() const {
    ensureBatches();
    return leafLength_ * data_.leaf.baseScale;
}

int ForestNode::leafCopies() const {
    ensureBatches();
    return leafCopies_;
}

const bromesh::MeshData& ForestNode::leafBatch() const {
    ensureBatches();
    return leafBatch_;
}

const bromesh::MeshData& ForestNode::cardBatch() const {
    ensureBatches();
    return cardBatch_;
}

const bromesh::MeshData& ForestNode::clusterBatch() const {
    ensureBatches();
    return clusterBatch_;
}

float ForestNode::leafAspect() const {
    ensureBatches();
    return leafAspect_;
}

int ForestNode::cardVertices() const {
    ensureBatches();
    return cardVertices_;
}

uint64_t ForestNode::cardGeneration() const {
    ensureBatches();
    return cardGeneration_;
}

void ForestNode::ensureBatches() const {
    if (batchGeneration_ == geometryGeneration() && batchForm_ == look_.leafForm) return;
    batchGeneration_ = geometryGeneration();
    batchForm_ = look_.leafForm;
    cardGeneration_ = nextResourceGeneration();
    cardVertices_ = look_.leafForm != 0 ? 4 : kCardVertices;
    leafBatch_.clear();
    cardBatch_.clear();
    clusterBatch_.clear();
    leafLength_ = 0.0f;
    leafAspect_ = 0.5f;
    leafCopies_ = 1;
    const bromesh::MeshData& m = mesh();
    const size_t vc = m.vertexCount();
    if (vc == 0) return;

    // The leaf in its frame: +Z its tip, +X across, +Y its normal.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, meanY = 0.0f;
    for (size_t i = 0; i < vc; ++i)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], m.positions[i * 3 + k]);
            hi[k] = std::max(hi[k], m.positions[i * 3 + k]);
        }
    for (size_t i = 0; i < vc; ++i) meanY += m.positions[i * 3 + 1];
    meanY /= static_cast<float>(vc);
    leafLength_ = std::max(hi[2] - lo[2], 1e-4f);
    leafAspect_ = std::clamp((hi[0] - lo[0]) / leafLength_, 0.05f, 2.0f);

    // About a thousand vertices an instance (InstancedMeshNode::scatterBatchMesh).
    const int copies = static_cast<int>(std::clamp<size_t>(1024 / vc, 1, 256));
    leafCopies_ = copies;
    const auto repeat = [](const std::vector<float>& src, std::vector<float>& dst, int n) {
        dst.reserve(src.size() * n);
        for (int c = 0; c < n; ++c) dst.insert(dst.end(), src.begin(), src.end());
    };
    repeat(m.positions, leafBatch_.positions, copies);
    repeat(m.normals, leafBatch_.normals, copies);
    repeat(m.uvs, leafBatch_.uvs, copies);
    repeat(m.colors, leafBatch_.colors, copies);
    repeat(m.tangents, leafBatch_.tangents, copies);
    std::vector<uint32_t> idx = m.indices;
    if (idx.empty()) {
        idx.resize(vc);
        for (uint32_t i = 0; i < vc; ++i) idx[i] = i;
    }
    leafBatch_.indices.reserve(idx.size() * copies);
    for (int c = 0; c < copies; ++c)
        for (uint32_t i : idx) leafBatch_.indices.push_back(static_cast<uint32_t>(c * vc) + i);

    // The far card: six vertices in the leaf's outline (base, two a side,
    // tip), its mean colour; a spray's, the four corners of its card.
    const float x0 = lo[0], x1 = hi[0], xc = 0.5f * (lo[0] + hi[0]);
    const float z0 = lo[2], L = leafLength_;
    const float leafCard[kCardVertices][2] = {{xc, z0}, {x1, z0 + 0.3f * L}, {xc + 0.4f * (x1 - xc), z0 + 0.78f * L},
                                              {xc, z0 + L}, {xc - 0.4f * (xc - x0), z0 + 0.78f * L}, {x0, z0 + 0.3f * L}};
    const float sprayCard[4][2] = {{x0, z0}, {x1, z0}, {x1, z0 + L}, {x0, z0 + L}};
    const int cardVerts = cardVertices_;
    const float (*card)[2] = cardVerts == 4 ? sprayCard : leafCard;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (m.colors.size() >= vc * 4) {
        for (int k = 0; k < 4; ++k) color[k] = 0.0f;
        for (size_t i = 0; i < vc; ++i)
            for (int k = 0; k < 4; ++k) color[k] += m.colors[i * 4 + k];
        for (int k = 0; k < 4; ++k) color[k] /= static_cast<float>(vc);
    }
    const bool colored = !m.colors.empty();
    for (int c = 0; c < kCardCopies; ++c) {
        for (int v = 0; v < cardVerts; ++v) {
            cardBatch_.positions.insert(cardBatch_.positions.end(), {card[v][0], meanY, card[v][1]});
            cardBatch_.normals.insert(cardBatch_.normals.end(), {0.0f, 1.0f, 0.0f});
            cardBatch_.uvs.insert(cardBatch_.uvs.end(), {(card[v][0] - x0) / std::max(x1 - x0, 1e-6f),
                                                         (card[v][1] - z0) / L});
            cardBatch_.tangents.insert(cardBatch_.tangents.end(), {1.0f, 0.0f, 0.0f, 1.0f});
            if (colored) cardBatch_.colors.insert(cardBatch_.colors.end(), std::begin(color), std::end(color));
        }
        const uint32_t b = static_cast<uint32_t>(c * cardVerts);
        for (uint32_t t = 1; t + 1 < static_cast<uint32_t>(cardVerts); ++t)
            cardBatch_.indices.insert(cardBatch_.indices.end(), {b, b + t, b + t + 1});
    }

    // The clusters: a unit hexagon at the leaf's middle, which
    // mesh_forest_leaf.vert spreads in the leaf's plane round its widened
    // outline; mesh.frag draws the leaves it stands for there
    // (scene_forest_mass.glsl).
    for (int c = 0; c < kCardCopies; ++c) {
        for (int v = 0; v < kCardVertices; ++v) {
            const float a = 6.2831853f * (static_cast<float>(v) + 0.5f) / static_cast<float>(kCardVertices);
            clusterBatch_.positions.insert(clusterBatch_.positions.end(), {xc, meanY, z0 + 0.5f * L});
            clusterBatch_.normals.insert(clusterBatch_.normals.end(), {0.0f, 1.0f, 0.0f});
            clusterBatch_.uvs.insert(clusterBatch_.uvs.end(), {std::cos(a), std::sin(a)});
            clusterBatch_.tangents.insert(clusterBatch_.tangents.end(), {1.0f, 0.0f, 0.0f, 1.0f});
        }
        const uint32_t b = static_cast<uint32_t>(c * kCardVertices);
        for (uint32_t t = 1; t + 1 < static_cast<uint32_t>(kCardVertices); ++t)
            clusterBatch_.indices.insert(clusterBatch_.indices.end(), {b, b + t, b + t + 1});
    }
}

uint64_t ForestNode::dataGeneration() const {
    refreshGpu();
    return dataGeneration_;
}

void ForestNode::setLook(const Look& l) {
    Look n = l;
    n.leafForm = std::clamp(n.leafForm, 0, kLeafForms - 1);
    n.flutter = std::clamp(std::isfinite(n.flutter) ? n.flutter : 1.0f, 0.0f, 4.0f);
    n.limbBob = std::clamp(std::isfinite(n.limbBob) ? n.limbBob : 0.6f, 0.0f, 3.9f);
    n.twigBob = std::clamp(std::isfinite(n.twigBob) ? n.twigBob : 0.8f, 0.0f, 3.9f);
    if (n.leafForm == look_.leafForm && n.flutter == look_.flutter && n.limbBob == look_.limbBob &&
        n.twigBob == look_.twigBob)
        return;
    look_ = n;
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
}

bool ForestNode::setSpecimenLooks(std::vector<float> looks) {
    if (!looks.empty() && looks.size() != specimens_.size() * 8) return false;
    looks_ = std::move(looks);
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
    return true;
}

void ForestNode::refreshGpu() const {
    const float* c = color();
    const float colors[6] = {c[0], c[1], c[2], barkColor_[0], barkColor_[1], barkColor_[2]};
    if (gpuMeshGeneration_ != geometryGeneration() || gpuFlutter_ != lod_.flutterFade ||
        (!looks_.empty() && !std::equal(std::begin(colors), std::end(colors), gpuColors_))) {
        gpuMeshGeneration_ = geometryGeneration();
        gpuFlutter_ = lod_.flutterFade;
        std::copy(std::begin(colors), std::end(colors), gpuColors_);
        gpuDirty_ = true;
        dataGeneration_ = nextResourceGeneration();
    }
}

const std::vector<float>& ForestNode::gpuData() const {
    refreshGpu();
    if (!gpuDirty_) return gpu_;
    gpuDirty_ = false;
    const SpecimenArrays& a = data_;
    const size_t nSpec = specimens_.size(), nSeg = a.segments.size() / 8, nClump = a.clumps.size() / 8;
    const size_t nLeaf = a.leaves.size() / 2;

    // Record offsets (vec4s) of each array.
    const uint32_t specBase = 0;
    const uint32_t segBase = specBase + static_cast<uint32_t>(nSpec * kSpecimenRecords);
    const uint32_t clumpBase = segBase + static_cast<uint32_t>(nSeg * 7);
    // A crown traces its tree's clumps a block of kClumpBlock at a time
    // (scene_forest_crown.glsl), skipping a block whose bounding sphere its
    // ray misses: each specimen's clumps in runs of kClumpBlock, a record a
    // run (the sphere round the run's ellipsoids), after the clumps.
    const uint32_t blockBase = clumpBase + static_cast<uint32_t>(nClump * 6);
    std::vector<uint32_t> specBlock(nSpec);
    uint32_t nBlock = 0;
    for (size_t k = 0; k < nSpec; ++k) {
        specBlock[k] = blockBase + nBlock * kBlockRecords;
        nBlock += (specimens_[k].clumpCount + kClumpBlock - 1) / kClumpBlock;
    }
    const uint32_t leafBase = blockBase + nBlock * kBlockRecords;
    const uint32_t instBase = leafBase + static_cast<uint32_t>((nLeaf + 1) / 2);
    const size_t records = instBase + instances_.size() * 2;

    gpu_.assign(kHeaderFloats + records * 4, 0.0f);
    float* h = gpu_.data();
    h[0] = bitsOf(a.leaf.seed);
    h[1] = a.leaf.upBias;
    h[2] = a.leaf.tiltJitter;
    h[3] = a.leaf.shell;
    h[4] = a.leaf.baseScale;
    h[5] = a.leaf.scaleJitter;
    h[6] = leafLength();
    h[7] = lod_.flutterFade;
    h[8] = bitsOf(specBase);
    h[9] = bitsOf(segBase);
    h[10] = bitsOf(clumpBase);
    h[11] = bitsOf(leafBase);
    h[12] = bitsOf(instBase);
    // A spray's leaves cover kSprayFill of its card: the far tiers take them
    // as leaves of that area (an ellipse a card's length long; a broad leaf's
    // ellipse is its card's outline already).
    const float fill = sprayFill(look_.leafForm);
    h[13] = look_.leafForm == 0 ? leafAspect() : leafAspect() * fill * (4.0f / 3.14159265f);
    h[14] = look_.flutter;
    // Leaf form (bits 0-3), limb and twig bob in 64ths (8 bits each).
    const auto q = [](float v) { return static_cast<uint32_t>(std::clamp(std::lround(v * 64.0f), 0L, 255L)); };
    h[15] = bitsOf(static_cast<uint32_t>(look_.leafForm) | (q(look_.limbBob) << 8) | (q(look_.twigBob) << 16));

    float* r = gpu_.data() + kHeaderFloats;
    for (size_t k = 0; k < nSpec; ++k) {
        const Specimen& s = specimens_[k];
        float* d = r + (specBase + k * kSpecimenRecords) * 4;
        d[0] = bitsOf(s.segStart); d[1] = bitsOf(s.segCount); d[2] = bitsOf(s.clumpStart); d[3] = bitsOf(s.clumpCount);
        d[4] = bitsOf(s.leafStart); d[5] = bitsOf(s.leafCount); d[6] = bitsOf(s.crownClump);
        d[7] = s.meanClumpWeight;
        d[8] = s.crown.x; d[9] = s.crown.y; d[10] = s.crown.z; d[11] = s.crownRadius;
        d[12] = s.crownRadiusUp; d[13] = s.meanClumpRadius; d[14] = s.height; d[15] = bitsOf(specBlock[k]);
        // Its leaves' and bark's tint: its colour over the node's (1 where it
        // has none, or the node is black).
        // Record 4's w: its clumps' flatness (scene_forest.glsl's forestClumpFlat).
        d[19] = s.clumpFlat;
        for (int part = 0; part < 2; ++part) {
            float* t = d + 16 + part * 4;
            t[0] = t[1] = t[2] = 1.0f;
            if (looks_.size() != nSpec * 8) continue;
            const float* look = &looks_[k * 8 + part * 4];
            if (look[0] + look[1] + look[2] <= 0.0f) continue;
            for (int i = 0; i < 3; ++i) {
                const float base = gpuColors_[part * 3 + i];
                t[i] = base > 1e-4f ? std::clamp(look[i] / base, 0.05f, 8.0f) : 1.0f;
            }
        }
    }
    for (size_t g = 0; g < nSeg; ++g) {
        float* d = r + (segBase + g * 7) * 4;
        std::memcpy(d, &a.segments[g * 8], 8 * sizeof(float));
        const float* t = &a.tangents[g * 6];
        octEncode(t[0], t[1], t[2], d[8], d[9]);
        octEncode(t[3], t[4], t[5], d[10], d[11]);
        std::memcpy(d + 12, &a.segmentVegetation[g * 16], 16 * sizeof(float));
    }
    for (size_t c = 0; c < nClump; ++c) {
        float* d = r + (clumpBase + c * 6) * 4;
        std::memcpy(d, &a.clumps[c * 8], 8 * sizeof(float));
        std::memcpy(d + 8, &a.clumpVegetation[c * 16], 16 * sizeof(float));
    }
    for (size_t k = 0; k < nSpec; ++k) {
        const Specimen& s = specimens_[k];
        // Each clump's ellipsoid (0.9 of its radius up, 0.7 down, its
        // radius across) lies in the sphere of its radius about a point 0.1
        // of it above the centre.
        const auto centre = [&](uint32_t local) {
            const float* p = &a.clumps[(s.clumpStart + local) * 8];
            return bromath::Vec3{p[0], p[1] + 0.1f * p[3], p[2]};
        };
        const std::vector<uint32_t> order = groupClumps(s.clumpCount, centre);
        for (uint32_t b = 0; b * kClumpBlock < s.clumpCount; ++b) {
            const uint32_t i0 = b * kClumpBlock, i1 = std::min(i0 + kClumpBlock, s.clumpCount);
            bromath::Vec3 m{0.0f, 0.0f, 0.0f};
            for (uint32_t i = i0; i < i1; ++i) m = m + centre(order[i]);
            m = m * (1.0f / static_cast<float>(i1 - i0));
            float rad = 0.0f;
            for (uint32_t i = i0; i < i1; ++i)
                rad = std::max(rad, bromath::vdist(centre(order[i]), m) + a.clumps[(s.clumpStart + order[i]) * 8 + 3]);
            float* d = r + (specBlock[k] + b * kBlockRecords) * 4;
            d[0] = m.x; d[1] = m.y; d[2] = m.z; d[3] = rad;
            // Its clumps (the specimen's own indices), two to a float, 0xffff
            // past the last.
            for (uint32_t j = 0; j < kClumpBlock; j += 2) {
                const uint32_t lo = i0 + j < i1 ? order[i0 + j] : 0xffffu;
                const uint32_t hi = i0 + j + 1 < i1 ? order[i0 + j + 1] : 0xffffu;
                d[4 + j / 2] = bitsOf(lo | (hi << 16));
            }
        }
    }
    for (size_t i = 0; i < nLeaf; ++i) {
        float* d = r + leafBase * 4 + i * 2;
        d[0] = bitsOf(a.leaves[2 * i]);
        d[1] = bitsOf(a.leaves[2 * i + 1]);
    }
    for (size_t i = 0; i < instances_.size(); ++i) {
        const Instance& in = instances_[i];
        float* d = r + (instBase + i * 2) * 4;
        d[0] = in.position.x; d[1] = in.position.y; d[2] = in.position.z; d[3] = in.scale;
        d[4] = in.cosYaw; d[5] = in.sinYaw; d[6] = bitsOf(in.specimen); d[7] = in.seed;
    }
    return gpu_;
}

}  // namespace bro::scene
