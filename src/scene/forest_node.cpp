#include "scene/forest_node.h"

#include <broflora/forest_records.h>

#include "scene/vulkan/shaders/scene_forest_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace bro::scene {

namespace {

// emitForest's arrays, as broflora lays them out.
namespace rec = broflora::forest_records;
// The node's GPU layout (scene_forest_layout.h, which the forest shaders
// read by): the runs that copy broflora's records verbatim keep broflora's
// offsets.
static_assert(FOREST_SEG_TO - FOREST_SEG_FROM == rec::segment::kTo - rec::segment::kFrom &&
              FOREST_SEG_R_FROM - FOREST_SEG_FROM == rec::segment::kRFrom - rec::segment::kFrom &&
              FOREST_SEG_R_TO - FOREST_SEG_FROM == rec::segment::kRTo - rec::segment::kFrom &&
              rec::segment::kFrom == 0 && FOREST_SEG_TANGENTS - FOREST_SEG_FROM == rec::segment::kFloats);
static_assert(FOREST_CLUMP_CENTRE == rec::clump::kCentre && FOREST_CLUMP_RADIUS == rec::clump::kRadius &&
              FOREST_CLUMP_TWIG == rec::clump::kTwig && FOREST_CLUMP_LEAVES == rec::clump::kLeafCount &&
              FOREST_CLUMP_VEGETATION == rec::clump::kFloats);
static_assert(FOREST_VEG_ROOT == rec::vegetation::kRoot && FOREST_VEG_HEIGHT == rec::vegetation::kHeight &&
              FOREST_VEG_CROWN == rec::vegetation::kCrown && FOREST_VEG_CROWN_RADIUS == rec::vegetation::kCrownRadius &&
              FOREST_VEG_WEIGHT_FROM == rec::vegetation::kWeightFrom && FOREST_VEG_WEIGHT_TO == rec::vegetation::kWeightTo &&
              FOREST_VEG_PATH == rec::vegetation::kPath && FOREST_VEG_BARK == rec::vegetation::kBark &&
              FOREST_VEG_AXIS == rec::vegetation::kAxis && FOREST_VEG_TWIG == rec::vegetation::kTwig &&
              FOREST_VEG_LIMB == rec::vegetation::kLimb && FOREST_VEG_TRUNK == rec::vegetation::kTrunk &&
              rec::vegetation::kFloats == 16);
static_assert(FOREST_BLOOM_POSITION == rec::bloom::kPosition && FOREST_BLOOM_SIZE == rec::bloom::kSize &&
              FOREST_BLOOM_DIR == rec::bloom::kDir && FOREST_BLOOM_SEGMENT == rec::bloom::kSegment);
static_assert(FOREST_LEAVES_PER_RECORD * rec::leaf::kValues == 4);
static_assert(FOREST_SEG_CLUMP_SLOTS == rec::segment_clumps::kMax &&
              rec::segment_clumps::kFloats == rec::segment_clumps::kClumps + rec::segment_clumps::kMax);
static_assert(ForestNode::kClumpBlock == FOREST_CLUMP_BLOCK && ForestNode::kBlockRecords == FOREST_BLOCK_RECORDS &&
              ForestNode::kBlockGroup == FOREST_BLOCK_GROUP);
constexpr size_t kHeaderFloats = FOREST_HEADER_FLOATS;
constexpr size_t kSpecimenRecords = FOREST_SPECIMEN_RECORDS;

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
    const size_t nSpec = a.specimens.size() / rec::specimen::kFloats;
    const size_t nSeg = a.segments.size() / rec::segment::kFloats;
    const size_t nClump = a.clumps.size() / rec::clump::kFloats;
    const size_t nLeaf = a.leaves.size() / rec::leaf::kValues;
    if (a.specimens.size() % rec::specimen::kFloats || a.segments.size() % rec::segment::kFloats ||
        a.tangents.size() != nSeg * rec::tangent::kFloats ||
        a.segmentVegetation.size() != nSeg * rec::vegetation::kFloats || a.keepRadius.size() != nSeg ||
        a.clumps.size() % rec::clump::kFloats || a.clumpVegetation.size() != nClump * rec::vegetation::kFloats ||
        a.leaves.size() % rec::leaf::kValues ||
        (!a.segmentClumps.empty() && a.segmentClumps.size() != nSeg * rec::segment_clumps::kFloats) ||
        (!a.clumpGroups.empty() && a.clumpGroups.size() != nClump))
        return false;
    for (size_t i = 0; i < nLeaf; ++i)
        if (a.leaves[i * rec::leaf::kValues + rec::leaf::kClump] >= nClump) return false;

    std::vector<Specimen> specs(nSpec);
    for (size_t k = 0; k < nSpec; ++k) {
        const float* f = &a.specimens[k * rec::specimen::kFloats];
        Specimen& s = specs[k];
        const auto u = [&](size_t i) { return static_cast<uint32_t>(std::max(0.0f, f[i])); };
        s.segStart = u(rec::specimen::kSegStart); s.segCount = u(rec::specimen::kSegCount);
        s.clumpStart = u(rec::specimen::kClumpStart); s.clumpCount = u(rec::specimen::kClumpCount);
        s.leafStart = u(rec::specimen::kLeafStart); s.leafCount = u(rec::specimen::kLeafCount);
        s.height = f[rec::specimen::kHeight];
        // Its clumps' height over their width (0: emitted before it existed).
        const float flat = f[rec::specimen::kClumpFlat];
        s.clumpFlat = flat > 0.0f ? std::clamp(flat, 0.2f, 1.0f) : 1.0f;
        if (uint64_t{s.segStart} + s.segCount > nSeg || uint64_t{s.clumpStart} + s.clumpCount > nClump ||
            uint64_t{s.leafStart} + s.leafCount > nLeaf)
            return false;
        if (!a.segmentClumps.empty())
            for (size_t i = s.segStart * rec::segment_clumps::kFloats;
                 i < (s.segStart + s.segCount) * rec::segment_clumps::kFloats; ++i)
                if (!(a.segmentClumps[i] < static_cast<float>(s.clumpCount))) return false;
        if (!a.clumpGroups.empty())
            for (uint32_t c = s.clumpStart; c < s.clumpStart + s.clumpCount; ++c)
                if (!(a.clumpGroups[c] >= 0.0f && a.clumpGroups[c] < static_cast<float>(s.clumpCount))) return false;

        // The crown: the extent of its clumps; the bounds: bark and clumps.
        bromath::AABB3 crown = bromath::aempty3();
        float sumR = 0.0f;
        for (uint32_t c = s.clumpStart; c < s.clumpStart + s.clumpCount; ++c) {
            const float* p = &a.clumps[c * rec::clump::kFloats + rec::clump::kCentre];
            const float r = a.clumps[c * rec::clump::kFloats + rec::clump::kRadius];
            // Its ellipsoid: r across, 0.9 r up and 0.7 r down times the flatness.
            const float up = r * s.clumpFlat;
            crown = bromath::amerge(crown, bromath::AABB3{{p[0] - r, p[1] - 0.7f * up, p[2] - r},
                                                          {p[0] + r, p[1] + 0.9f * up, p[2] + r}});
            sumR += r;
        }
        s.bounds = crown;
        for (uint32_t g = s.segStart; g < s.segStart + s.segCount; ++g) {
            const float* r = &a.segments[g * rec::segment::kFloats];
            const float* from = r + rec::segment::kFrom;
            const float* to = r + rec::segment::kTo;
            s.bounds = grow(s.bounds, {from[0], from[1], from[2]}, r[rec::segment::kRFrom]);
            s.bounds = grow(s.bounds, {to[0], to[1], to[2]}, r[rec::segment::kRTo]);
            s.trunkRadius = std::max(s.trunkRadius, r[rec::segment::kRFrom]);
        }
        if (bromath::aisEmpty(s.bounds)) s.bounds = bromath::AABB3{{0.0f, 0.0f, 0.0f}, {0.0f, s.height, 0.0f}};
        s.bareBounds = s.bounds;
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
                sumW += a.clumpVegetation[c * rec::vegetation::kFloats + rec::vegetation::kWeightTo];
                const float* r = &a.clumps[c * rec::clump::kFloats + rec::clump::kCentre];
                const float dx = r[0] - s.crown.x, dz = r[2] - s.crown.z;
                if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; s.crownClump = c; }
            }
            s.meanClumpWeight = std::clamp(sumW / static_cast<float>(s.clumpCount), 0.0f, 1.0f);
        } else {
            const float* crown = f + rec::specimen::kCrown;
            s.crown = {crown[0], crown[1], crown[2]};
            s.crownRadius = s.crownRadiusUp = f[rec::specimen::kCrownRadius];
        }
    }

    specimens_ = std::move(specs);
    if (looks_.size() != specimens_.size() * rec::look::kFloats) looks_.clear();
    if (leafLooks_.size() != specimens_.size() * rec::leaf_look::kFloats) leafLooks_.clear();
    keepRadius_ = a.keepRadius;
    woodArea_.assign(nSeg + 1, 0.0f);
    for (size_t i = 0; i < nSeg; ++i) {
        const float* g = &a.segments[i * rec::segment::kFloats];
        const float* p = g + rec::segment::kFrom;
        const float* q = g + rec::segment::kTo;
        const float len = std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) +
                                    (q[2] - p[2]) * (q[2] - p[2]));
        woodArea_[i + 1] = woodArea_[i] + (g[rec::segment::kRFrom] + std::max(g[rec::segment::kRTo], 0.0f)) * len;
    }
    data_ = std::move(a);
    countClumpTwigs();
    // The blooms were the old specimens' (setBlooms gives the new ones').
    data_.blooms.clear();
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
        namespace ir = rec::instance;
        const float* r = rows + i * ir::kFloats;
        const float* p = r + ir::kPosition;
        if (!(r[ir::kSpecimen] >= 0.0f) || static_cast<size_t>(r[ir::kSpecimen]) >= specimens_.size() ||
            !(r[ir::kScale] > 0.0f))
            continue;
        Instance in;
        in.position = {p[0], p[1], p[2]};
        in.scale = r[ir::kScale];
        in.cosYaw = std::cos(r[ir::kYaw]);
        in.sinYaw = std::sin(r[ir::kYaw]);
        in.specimen = static_cast<uint32_t>(r[ir::kSpecimen]);
        // A seed from where it stands.
        const float h = std::sin(p[0] * 12.9898f + p[2] * 78.233f) * 43758.5453f;
        in.seed = h - std::floor(h);
        instances_.push_back(in);
    }
    rebuildBounds();
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
}

bool ForestNode::setBlooms(std::vector<float> blooms, std::vector<float> specimenBlooms) {
    const size_t nSpec = specimens_.size();
    namespace br = rec::bloom;
    namespace sb = rec::specimen_bloom;
    const size_t nBloom = blooms.size() / br::kFloats;
    const bool none = blooms.empty() && specimenBlooms.empty();
    if (!none && (blooms.size() % br::kFloats || specimenBlooms.size() != nSpec * sb::kFloats)) return false;
    // Every specimen's run inside the blooms, every bloom's segment its
    // specimen's own.
    for (size_t k = 0; k < nSpec && !none; ++k) {
        const float* f = &specimenBlooms[k * sb::kFloats];
        const uint64_t start = static_cast<uint64_t>(std::max(0.0f, f[sb::kStart]));
        const uint64_t count = static_cast<uint64_t>(std::max(0.0f, f[sb::kCount]));
        if (start + count > nBloom) return false;
        for (uint64_t b = start; b < start + count; ++b) {
            const float seg = blooms[b * br::kFloats + br::kSegment];
            if (!(seg >= 0.0f) || seg >= static_cast<float>(specimens_[k].segCount)) return false;
        }
    }
    for (size_t k = 0; k < nSpec; ++k) {
        Specimen& s = specimens_[k];
        s.bloomStart = s.bloomCount = 0;
        s.bloomForm = 0;
        s.bloomSize = 0.0f;
        s.bounds = s.bareBounds;
        if (none) continue;
        const float* f = &specimenBlooms[k * sb::kFloats];
        const int form = static_cast<int>(std::lround(f[sb::kForm]));
        if (form <= 0 || form >= kBloomForms) continue;
        s.bloomStart = static_cast<uint32_t>(std::max(0.0f, f[sb::kStart]));
        s.bloomCount = static_cast<uint32_t>(std::max(0.0f, f[sb::kCount]));
        s.bloomForm = form;
        for (int i = 0; i < 3; ++i) s.bloomColor[i] = std::max(f[sb::kColor + i], 0.0f);
        // Its blooms in its bounds, each as far as its form reaches.
        for (uint32_t b = s.bloomStart; b < s.bloomStart + s.bloomCount; ++b) {
            const float* r = &blooms[b * br::kFloats];
            const float size = r[br::kSize];
            s.bloomSize = std::max(s.bloomSize, size);
            s.bounds = grow(s.bounds, {r[br::kPosition], r[br::kPosition + 1], r[br::kPosition + 2]},
                            size * bloomReach(form));
        }
    }
    data_.blooms = none ? std::vector<float>{} : std::move(blooms);
    rebuildBounds();
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
    return true;
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
                                l.budget, l.shadowBudget, l.clusters, l.barkKeepPixels, l.barkFineSides,
                                l.forceRange, l.leavesRange, l.leavesBand};
    std::copy(std::begin(v), std::end(v), out);
}

void ForestNode::lodFromArray(const double* v, size_t n) {
    float* dst[kLodCount] = {&lod_.leafPixels, &lod_.flutterFade, &lod_.cardWidening, &lod_.clumpFrom,
                             &lod_.clumpTo, &lod_.crownFrom, &lod_.crownTo, &lod_.barkPixels,
                             &lod_.barkSidesPixels, &lod_.shadowKeep, &lod_.range, &lod_.budget,
                             &lod_.shadowBudget, &lod_.clusters, &lod_.barkKeepPixels, &lod_.barkFineSides,
                             &lod_.forceRange, &lod_.leavesRange, &lod_.leavesBand};
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
    lod_.barkFineSides = std::max(lod_.barkFineSides, 0.0f);
    lod_.forceRange = std::max(lod_.forceRange, 0.0f);
    lod_.leavesRange = std::max(lod_.leavesRange, 0.0f);
    lod_.leavesBand = std::max(lod_.leavesBand, 0.0f);
}

ForestNode::ClumpTwigs ForestNode::twigPieces(uint32_t leaves) {
    // scene_forest_shoot.glsl's forestShootCounts and forestTwigPieces.
    if (leaves == 0) return {};
    const uint32_t S = std::max(1u, (leaves + 4u) / 8u);
    const uint32_t NB = S < 3u ? S : std::max(3u, (S + 2u) / 5u);
    // FOREST_STEMS (three lobes' stems of FOREST_STEM_PIECES), each lobe's
    // limb slots (forestLimbSlots: lobe 0's limbs, one a FOREST_LIMB_SPRAYS
    // of its sprays, none under 3) of FOREST_LIMB_PIECES, then
    // FOREST_SPRAY_PIECES a spray and FOREST_SHOOT_PIECES a shoot.
    constexpr uint32_t kStemPieces = 4, kLimbPieces = 5, kLimbSprays = 2, kSprayPieces = 5 + 4, kShootPieces = 4;
    const uint32_t lobeSprays = (NB + 2u) / 3u;
    const uint32_t limbs = lobeSprays < 3u ? 0u : (lobeSprays + kLimbSprays - 1u) / kLimbSprays;
    const uint32_t stems = 3u * kStemPieces + 3u * kLimbPieces * limbs + kSprayPieces * NB;
    return {stems, stems + kShootPieces * S};
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
                                        int forceTier, float barkPx, float dist, float distFar) {
    TreeLod t;
    px = std::max(px, 1e-6f);
    barkPx = barkPx > 0.0f ? barkPx : px;
    t.barkRadius = lod.barkPixels / (2.0f * barkPx);
    const float trunkPx = 2.0f * s.trunkRadius * barkPx;
    t.barkSides = lod.barkFineSides > 0.0f && trunkPx > lod.barkFineSides * lod.barkSidesPixels
                      ? FOREST_BARK_SIDES_NEAREST
                  : trunkPx > lod.barkSidesPixels ? FOREST_BARK_SIDES_NEAR
                                                  : FOREST_BARK_SIDES_FAR;
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
    // Near leaves hand over to cards through a narrow band either side of
    // cardWidening (the drawer splits the kept leaves between them).
    t.cardShare = smooth(lod.cardWidening / 1.06f, lod.cardWidening * 1.06f, t.widening);
    t.card = t.cardShare > 0.5f;
    const float toClumps = smooth(lod.clumpFrom, lod.clumpTo, t.widening);
    // Crowns take over as a clump shrinks on screen (crownFrom > crownTo).
    const float toCrowns = s.clumpCount > 0 ? 1.0f - smooth(lod.crownTo, lod.crownFrom, s.meanClumpRadius * px) : 0.0f;
    t.crowns = toCrowns;
    t.clumps = s.clumpCount > 0 ? toClumps * (1.0f - toCrowns) : 0.0f;
    t.leaves = s.clumpCount > 0 ? (1.0f - toClumps) * (1.0f - toCrowns) : 1.0f;
    // The near sphere: a crown reaching into it draws its leaves whole, and
    // its clumps whole where it reaches past the band's near edge (each
    // tier's pixels are cut to its side of the sphere); one wholly past it
    // hands its leaves' share to its clumps.
    if (lod.leavesRange > 0.0f && dist >= 0.0f && s.clumpCount > 0) {
        const float half = 0.5f * lod.leavesBand;
        const float farD = std::max(distFar, dist);
        if (dist < lod.leavesRange + half) {
            t.leaves = 1.0f;
            t.clumps = farD > lod.leavesRange - half ? 1.0f : 0.0f;
            t.crowns = 0.0f;
        } else {
            t.clumps += t.leaves;
            t.leaves = 0.0f;
        }
    }
    return t;
}

uint32_t ForestNode::beginSeen() {
    hasSeenView_ = false;
    if (seen_.size() != instances_.size()) seen_.assign(instances_.size(), SeenTree{});
    if (++seenStamp_ == 0) {
        for (SeenTree& s : seen_) s.stamp = 0;
        seenStamp_ = 1;
    }
    return seenStamp_;
}

// planForest's camera loop (scene_mesh_drawer_forest.cpp) for one tree,
// which must change with it.
bool ForestNode::measureSeen(uint32_t i, SeenTree& out) const {
    if (i >= instances_.size()) return false;
    const SeenView& v = seenView_;
    const Instance& in = instances_[i];
    const Specimen& sp = specimens_[in.specimen];
    const float reach = v.reach;
    bromath::AABB3 b = bromath::atransform(in.bounds, v.world);
    b.min -= bromath::Vec3{reach, reach, reach};
    b.max += bromath::Vec3{reach, reach, reach};
    if (!bromath::fintersects(v.frustum, b)) return false;
    const bromath::Vec3 e = v.eye;
    const bromath::Vec3 q{std::clamp(e.x, b.min.x + reach, b.max.x - reach),
                          std::clamp(e.y, b.min.y + reach, b.max.y - reach),
                          std::clamp(e.z, b.min.z + reach, b.max.z - reach)};
    const float d = std::max(bromath::vlen(q - e), 0.25f);
    if (lod_.range > 0.0f && d > lod_.range) return false;
    float dl = d, far = d;
    if (sp.clumpCount > 0) {
        const bromath::Vec3 local{sp.crown.x * in.cosYaw + sp.crown.z * in.sinYaw, sp.crown.y,
                                  -sp.crown.x * in.sinYaw + sp.crown.z * in.cosYaw};
        const bromath::Vec3 c = bromath::mtransformPoint(v.world, in.position + local * in.scale);
        const float k = in.scale * v.nodeScale / 0.92f;
        const float dh = std::max(0.0f, std::hypot(e.x - c.x, e.z - c.z) - sp.crownRadius * k);
        const float dv = std::max(0.0f, std::fabs(e.y - c.y) - sp.crownRadiusUp * k);
        dl = std::max(std::hypot(dh, dv), d);
        far = bromath::vlen(c - e) + std::max(sp.crownRadius, sp.crownRadiusUp) * k;
    }
    const float unit = in.scale * v.nodeScale;
    out.stamp = seenStamp_;
    out.px = (v.perspective ? v.pxAtMetre / dl : v.pxAtMetre) * unit;
    out.barkPx = (v.perspective ? v.pxAtMetre / d : v.pxAtMetre) * unit;
    out.dist = dl;
    out.far = far;
    out.forceTier = -1;
    return true;
}

bool ForestNode::seenLod(uint32_t i, TreeLod& out) const {
    if (hasSeenView_) {
        SeenTree m;
        if (!measureSeen(i, m)) return false;
        out = treeLod(scaled(lod_, stats().detail), specimens_[instances_[i].specimen], m.px, leafLength(), -1,
                      m.barkPx, m.dist, m.far);
        return true;
    }
    const SeenTree* s = seenNow(i);
    if (!s) return false;
    out = treeLod(seenLod_, specimens_[instances_[i].specimen], s->px, leafLength(), s->forceTier, s->barkPx, s->dist,
                  s->far);
    return true;
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
        n.twigBob == look_.twigBob && n.leafCut == look_.leafCut)
        return;
    look_ = n;
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
}

bool ForestNode::setSpecimenLooks(std::vector<float> looks) {
    const size_t n = specimens_.size();
    const size_t nLook = n * rec::look::kFloats;
    if (!looks.empty() && looks.size() != nLook && looks.size() != nLook + n * rec::leaf_look::kFloats) return false;
    // Past the looks, each specimen's broad leaf (emitForest's specimenLeaves).
    if (looks.size() > nLook) {
        leafLooks_.assign(looks.begin() + static_cast<std::ptrdiff_t>(nLook), looks.end());
        looks.resize(nLook);
    } else {
        leafLooks_.clear();
    }
    looks_ = std::move(looks);
    countClumpTwigs();
    gpuDirty_ = true;
    dataGeneration_ = nextResourceGeneration();
    return true;
}

void ForestNode::countClumpTwigs() {
    const size_t nClump = data_.clumps.size() / rec::clump::kFloats;
    clumpTwigs_.assign(nClump, {});
    for (size_t c = 0; c < nClump; ++c) {
        const float leaves = data_.clumps[c * rec::clump::kFloats + rec::clump::kLeafCount];
        clumpTwigs_[c] = twigPieces(static_cast<uint32_t>(std::max(0.0f, leaves) + 0.5f));
    }
    // A pinnate specimen's leaflets line its own wood (its leaf look's
    // phyllotaxis 3, scene_forest_shoot.glsl's forestPinnateLeaf): no twigs.
    if (leafLooks_.size() != specimens_.size() * rec::leaf_look::kFloats) return;
    for (size_t k = 0; k < specimens_.size(); ++k) {
        if (std::lround(leafLooks_[k * rec::leaf_look::kFloats + rec::leaf_look::kPhyllotaxis]) != 3) continue;
        const Specimen& s = specimens_[k];
        for (uint32_t c = s.clumpStart; c < s.clumpStart + s.clumpCount && c < nClump; ++c) clumpTwigs_[c] = {};
    }
}

bool ForestNode::pinnate(size_t k) const {
    return leafLooks_.size() == specimens_.size() * rec::leaf_look::kFloats && k < specimens_.size() &&
           std::lround(leafLooks_[k * rec::leaf_look::kFloats + rec::leaf_look::kPhyllotaxis]) == 3;
}

void ForestNode::barkOverLeaf(size_t k, float out[3]) const {
    const float* c = color();
    const bool own = looks_.size() == specimens_.size() * rec::look::kFloats && k < specimens_.size();
    const float* ownLeaf = own ? &looks_[k * rec::look::kFloats + rec::look::kLeafColor] : nullptr;
    const float* ownBark = own ? &looks_[k * rec::look::kFloats + rec::look::kBarkColor] : nullptr;
    const float* leaf = own && ownLeaf[0] + ownLeaf[1] + ownLeaf[2] > 0.0f ? ownLeaf : c;
    const float* bark = own && ownBark[0] + ownBark[1] + ownBark[2] > 0.0f ? ownBark : barkColor_;
    for (int i = 0; i < 3; ++i) out[i] = leaf[i] > 1e-4f ? std::clamp(bark[i] / leaf[i], 0.05f, 8.0f) : 1.0f;
}

float ForestNode::woodSeen(size_t k) const {
    if (k >= specimens_.size()) return 1.0f;
    const Specimen& s = specimens_[k];
    if (s.leafCount == 0 || s.crownRadius <= 0.0f) return 1.0f;
    // Its leaves' optical depth through the crown's outline (half their
    // area facing any way, each an ellipse its card long, a spray's at its
    // fill), and the share of wood spread evenly through that depth seen
    // past the leaves in front of it.
    const float len = leafLength();
    const float aspect =
        look_.leafForm == 0 ? leafAspect() : leafAspect() * sprayFill(look_.leafForm) * (4.0f / 3.14159265f);
    const float leaf = 0.785398f * len * len * aspect;
    const float outline = 3.14159265f * s.crownRadius * std::max(s.crownRadiusUp, s.crownRadius * 0.25f);
    const float L = 0.5f * static_cast<float>(s.leafCount) * leaf / outline;
    return L > 1e-3f ? (1.0f - std::exp(-L)) / L : 1.0f;
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
    const size_t nSpec = specimens_.size(), nSeg = a.segments.size() / rec::segment::kFloats;
    const size_t nClump = a.clumps.size() / rec::clump::kFloats, nLeaf = a.leaves.size() / rec::leaf::kValues;

    // Record offsets (vec4s) of each array.
    const uint32_t specBase = 0;
    const uint32_t segBase = specBase + static_cast<uint32_t>(nSpec * kSpecimenRecords);
    const uint32_t clumpBase = segBase + static_cast<uint32_t>(nSeg * FOREST_SEGMENT_RECORDS);
    // A crown traces its tree's clumps a block of kClumpBlock at a time
    // (scene_forest_crown.glsl), skipping a block whose bounding sphere its
    // ray misses: each specimen's clumps in runs of kClumpBlock, a record a
    // run (the sphere round the run's ellipsoids), after the clumps; before
    // a specimen's runs, the ellipsoid each kBlockGroup of them fills
    // (FOREST_GROUP_RECORDS a group).
    const uint32_t blockBase = clumpBase + static_cast<uint32_t>(nClump * FOREST_CLUMP_RECORDS);
    std::vector<uint32_t> specBlock(nSpec);
    uint32_t nBlockRecords = 0;
    for (size_t k = 0; k < nSpec; ++k) {
        const uint32_t runs = (specimens_[k].clumpCount + kClumpBlock - 1) / kClumpBlock;
        nBlockRecords += (runs + kBlockGroup - 1) / kBlockGroup * FOREST_GROUP_RECORDS;
        specBlock[k] = blockBase + nBlockRecords;
        nBlockRecords += runs * kBlockRecords;
    }
    const uint32_t leafBase = blockBase + nBlockRecords;
    const uint32_t instBase =
        leafBase + static_cast<uint32_t>((nLeaf + FOREST_LEAVES_PER_RECORD - 1) / FOREST_LEAVES_PER_RECORD);
    // The blooms after the instances, 2 records each (the bloom entries
    // carry their base: the header is full).
    const uint32_t bloomBase = instBase + static_cast<uint32_t>(instances_.size() * FOREST_INSTANCE_RECORDS);
    const size_t nBloom = a.blooms.size() / rec::bloom::kFloats;
    gpuBloomBase_ = bloomBase;
    const size_t records = bloomBase + nBloom * FOREST_BLOOM_RECORDS;

    gpu_.assign(kHeaderFloats + records * 4, 0.0f);
    float* h = gpu_.data();
    h[FOREST_H_SEED] = bitsOf(a.leaf.seed);
    h[FOREST_H_UP_BIAS] = a.leaf.upBias;
    h[FOREST_H_TILT_JITTER] = a.leaf.tiltJitter;
    h[FOREST_H_SHELL] = a.leaf.shell;
    h[FOREST_H_BASE_SCALE] = a.leaf.baseScale;
    h[FOREST_H_SCALE_JITTER] = a.leaf.scaleJitter;
    h[FOREST_H_LEAF_LENGTH] = leafLength();
    h[FOREST_H_FLUTTER_FADE] = lod_.flutterFade;
    h[FOREST_H_SPECIMENS] = bitsOf(specBase);
    h[FOREST_H_SEGMENTS] = bitsOf(segBase);
    h[FOREST_H_CLUMPS] = bitsOf(clumpBase);
    h[FOREST_H_LEAVES] = bitsOf(leafBase);
    h[FOREST_H_INSTANCES] = bitsOf(instBase);
    // A spray's leaves cover kSprayFill of its card: the far tiers take them
    // as leaves of that area (an ellipse a card's length long; a broad leaf's
    // ellipse is its card's outline already).
    const float fill = sprayFill(look_.leafForm);
    h[FOREST_H_LEAF_ASPECT] = look_.leafForm == 0 ? leafAspect() : leafAspect() * fill * (4.0f / 3.14159265f);
    h[FOREST_H_FLUTTER] = look_.flutter;
    // Leaf form (bits 0-3), limb and twig bob in 64ths (8 bits each).
    const auto q = [](float v) { return static_cast<uint32_t>(std::clamp(std::lround(v * 64.0f), 0L, 255L)); };
    h[FOREST_H_LOOK] = bitsOf(static_cast<uint32_t>(look_.leafForm) | (q(look_.limbBob) << 8) | (q(look_.twigBob) << 16));

    float* r = gpu_.data() + kHeaderFloats;
    for (size_t k = 0; k < nSpec; ++k) {
        const Specimen& s = specimens_[k];
        float* d = r + (specBase + k * kSpecimenRecords) * 4;
        d[FOREST_SP_SEG_START] = bitsOf(s.segStart); d[FOREST_SP_SEG_COUNT] = bitsOf(s.segCount);
        d[FOREST_SP_CLUMP_START] = bitsOf(s.clumpStart); d[FOREST_SP_CLUMP_COUNT] = bitsOf(s.clumpCount);
        d[FOREST_SP_LEAF_START] = bitsOf(s.leafStart); d[FOREST_SP_LEAF_COUNT] = bitsOf(s.leafCount);
        d[FOREST_SP_CROWN_CLUMP] = bitsOf(s.crownClump);
        d[FOREST_SP_CLUMP_WEIGHT] = s.meanClumpWeight;
        d[FOREST_SP_CROWN] = s.crown.x; d[FOREST_SP_CROWN + 1] = s.crown.y; d[FOREST_SP_CROWN + 2] = s.crown.z;
        d[FOREST_SP_CROWN_RADIUS] = s.crownRadius;
        d[FOREST_SP_CROWN_UP] = s.crownRadiusUp; d[FOREST_SP_CLUMP_RADIUS] = s.meanClumpRadius;
        d[FOREST_SP_HEIGHT] = s.height; d[FOREST_SP_BLOCKS] = bitsOf(specBlock[k]);
        // Its leaves' and bark's tint: its colour over the node's (1 where it
        // has none, or the node is black).
        // Record 4's w: its clumps' flatness (scene_forest.glsl's forestClumpFlat).
        d[FOREST_SP_CLUMP_FLAT] = s.clumpFlat;
        // Record 6: its blooms' colour (absolute) and form.
        for (int i = 0; i < 3; ++i) d[FOREST_SP_BLOOM_COLOR + i] = s.bloomColor[i];
        d[FOREST_SP_BLOOM_FORM] = bitsOf(static_cast<uint32_t>(s.bloomForm));
        // Record 7: its broad leaf (width over length, tip, teeth a side,
        // tooth depth); record 5's w its gloss. All 0: the node's own leaf.
        if (leafLooks_.size() == nSpec * rec::leaf_look::kFloats) {
            namespace ll = rec::leaf_look;
            const float* L = &leafLooks_[k * ll::kFloats];
            d[FOREST_SP_LEAF] = L[ll::kWidth]; d[FOREST_SP_LEAF + 1] = L[ll::kTip];
            d[FOREST_SP_LEAF + 2] = L[ll::kTeeth]; d[FOREST_SP_LEAF + 3] = L[ll::kToothDepth];
            d[FOREST_SP_GLOSS] = L[ll::kGloss];
            // Record 8: its sprays' brush (Species::sprayBrush), its broad
            // leaf's petiole (of the blade's length) and phyllotaxis
            // (Species::petiole, Species::phyllotaxis: 0 alternate, 1
            // opposite, 2 spiral; scene_forest_shoot.glsl), 0.
            d[FOREST_SP_BRUSH] = L[ll::kSprayBrush];
            d[FOREST_SP_PETIOLE] = L[ll::kPetiole];
            d[FOREST_SP_PHYLLOTAXIS] = L[ll::kPhyllotaxis];
        }
        for (int part = 0; part < 2; ++part) {
            float* t = d + (part ? FOREST_SP_BARK_TINT : FOREST_SP_LEAF_TINT);
            t[0] = t[1] = t[2] = 1.0f;
            if (looks_.size() != nSpec * rec::look::kFloats) continue;
            const float* look = &looks_[k * rec::look::kFloats + (part ? rec::look::kBarkColor : rec::look::kLeafColor)];
            if (look[0] + look[1] + look[2] <= 0.0f) continue;
            for (int i = 0; i < 3; ++i) {
                const float base = gpuColors_[part * 3 + i];
                t[i] = base > 1e-4f ? std::clamp(look[i] / base, 0.05f, 8.0f) : 1.0f;
            }
        }
    }
    for (size_t g = 0; g < nSeg; ++g) {
        float* d = r + (segBase + g * FOREST_SEGMENT_RECORDS) * 4;
        std::memcpy(d + FOREST_SEG_FROM, &a.segments[g * rec::segment::kFloats], rec::segment::kFloats * sizeof(float));
        const float* t = &a.tangents[g * rec::tangent::kFloats];
        const float* t0 = t + rec::tangent::kFrom;
        const float* t1 = t + rec::tangent::kTo;
        octEncode(t0[0], t0[1], t0[2], d[FOREST_SEG_TANGENTS], d[FOREST_SEG_TANGENTS + 1]);
        octEncode(t1[0], t1[1], t1[2], d[FOREST_SEG_TANGENTS + 2], d[FOREST_SEG_TANGENTS + 3]);
        std::memcpy(d + FOREST_SEG_VEGETATION, &a.segmentVegetation[g * rec::vegetation::kFloats],
                    rec::vegetation::kFloats * sizeof(float));
    }
    // Each segment's few clumps, made global: its wood is drawn while any
    // of them is not bare (scene_forest.glsl's forestSegmentBare).
    for (const Specimen& s : specimens_)
        for (uint32_t g = s.segStart; g < s.segStart + s.segCount && g < nSeg; ++g) {
            float* d = r + (segBase + g * FOREST_SEGMENT_RECORDS) * 4 + FOREST_SEG_CLUMPS;
            for (uint32_t j = 0; j < FOREST_SEG_CLUMP_SLOTS; ++j) {
                const float c = a.segmentClumps.empty()
                    ? -1.0f : a.segmentClumps[g * rec::segment_clumps::kFloats + rec::segment_clumps::kClumps + j];
                d[j] = bitsOf(c >= 0.0f ? s.clumpStart + static_cast<uint32_t>(c) : FOREST_SEG_NO_CLUMP);
            }
        }
    // The bark's end where a branch stops or forks into thinner ones: the
    // drawn wood narrows over its last segment to the thickest that goes
    // on from its end, or at a tip (none does) to kTipTaper of itself, into
    // the twigs its clump grows from. Without it the wood ended in an open
    // cut as thick as itself, a finger-thick stem stopping dead with only
    // its twigs going on. (The records only: the drawer's keep radii, and
    // so what is drawn, are the emit's.)
    constexpr float kTipTaper = 0.3f;
    for (const Specimen& s : specimens_) {
        std::vector<std::array<float, 4>> starts;   // a segment's start and radius there
        starts.reserve(s.segCount);
        for (uint32_t g = s.segStart; g < s.segStart + s.segCount && g < nSeg; ++g)
        {
            const float* r = &a.segments[g * rec::segment::kFloats];
            const float* p = r + rec::segment::kFrom;
            starts.push_back({p[0], p[1], p[2], r[rec::segment::kRFrom]});
        }
        std::sort(starts.begin(), starts.end());
        constexpr float kTol = 1e-4f;
        for (uint32_t g = s.segStart; g < s.segStart + s.segCount && g < nSeg; ++g) {
            const float* e = &a.segments[g * rec::segment::kFloats + rec::segment::kTo];
            auto it = std::lower_bound(starts.begin(), starts.end(),
                                       std::array<float, 4>{e[0] - kTol, -1e30f, -1e30f, -1e30f});
            float on = -1.0f;   // the thickest going on from its end
            for (; it != starts.end() && (*it)[0] <= e[0] + kTol; ++it)
                if (std::fabs((*it)[1] - e[1]) <= kTol && std::fabs((*it)[2] - e[2]) <= kTol) on = std::max(on, (*it)[3]);
            float* d = r + (segBase + g * FOREST_SEGMENT_RECORDS) * 4;
            // A tip's end radius goes in negative: the bark closes over its
            // last stretch to a rounded point (scene_forest_tube.glsl). Ribbed
            // bark (a cactus's: the bark word's top bit, broflora's
            // Species::barkRibs) keeps its girth to the end, which closes
            // over it in a blunt dome.
            const float bark = a.segmentVegetation[g * rec::vegetation::kFloats + rec::vegetation::kBark];
            const uint32_t word = static_cast<uint32_t>(std::max(bark, 0.0f) + 0.5f);
            const float taper = (word >> 23) & 1u ? 1.0f : kTipTaper;
            d[FOREST_SEG_R_TO] = on < 0.0f ? -d[FOREST_SEG_R_TO] * taper : std::min(d[FOREST_SEG_R_TO], on);
        }
    }
    // Each clump's leaves are keyed from its first (emitForest keys a
    // clump's leaves in one run): the shoot model numbers them from there.
    std::vector<uint32_t> keyBase(nClump, ~0u);
    for (size_t i = 0; i < nLeaf; ++i) {
        const uint32_t* l = &a.leaves[i * rec::leaf::kValues];
        const uint32_t c = l[rec::leaf::kClump];
        if (c < nClump) keyBase[c] = std::min(keyBase[c], l[rec::leaf::kKey]);
    }
    for (size_t c = 0; c < nClump; ++c) {
        float* d = r + (clumpBase + c * FOREST_CLUMP_RECORDS) * 4;
        std::memcpy(d + FOREST_CLUMP_CENTRE, &a.clumps[c * rec::clump::kFloats], rec::clump::kFloats * sizeof(float));
        std::memcpy(d + FOREST_CLUMP_VEGETATION, &a.clumpVegetation[c * rec::vegetation::kFloats],
                    rec::vegetation::kFloats * sizeof(float));
        // Its vegetation record's r2.z (the path, which no clump stage
        // reads): its placement-key base (scene_forest_shoot.glsl).
        d[FOREST_CLUMP_KEY_BASE] = bitsOf(keyBase[c] == ~0u ? 0u : keyBase[c]);
    }
    // Its drop group's ordinal in its specimen, in clump order (each clump
    // its own without the array).
    for (const Specimen& s : specimens_) {
        std::unordered_map<uint32_t, uint32_t> ordinal;
        for (uint32_t c = s.clumpStart; c < s.clumpStart + s.clumpCount && c < nClump; ++c) {
            const uint32_t g = a.clumpGroups.empty() ? c - s.clumpStart : static_cast<uint32_t>(a.clumpGroups[c]);
            const uint32_t o = ordinal.emplace(g, static_cast<uint32_t>(ordinal.size())).first->second;
            r[(clumpBase + c * FOREST_CLUMP_RECORDS) * 4 + FOREST_CLUMP_GROUP] = bitsOf(o);
        }
    }
    for (size_t k = 0; k < nSpec; ++k) {
        const Specimen& s = specimens_[k];
        // Each clump's ellipsoid (0.9 of its radius up, 0.7 down, its
        // radius across) lies in the sphere of its radius about a point 0.1
        // of it above the centre. A pinnate specimen's clump is a run along
        // its wood (scene_forest_crown.glsl), within its radius of the run's
        // middle, 0.35 of it back along the twig direction.
        const bool pinnate = leafLooks_.size() == nSpec * rec::leaf_look::kFloats &&
            std::lround(leafLooks_[k * rec::leaf_look::kFloats + rec::leaf_look::kPhyllotaxis]) == 3;
        const auto centre = [&](uint32_t local) {
            const float* q = &a.clumps[(s.clumpStart + local) * rec::clump::kFloats];
            const float* p = q + rec::clump::kCentre;
            const float cr = q[rec::clump::kRadius];
            if (pinnate) {
                const float* t = q + rec::clump::kTwig;
                bromath::Vec3 D{t[0], t[1], t[2]};
                const float dl = bromath::vlen(D);
                D = dl > 1e-6f ? D * (1.0f / dl) : bromath::Vec3{0.0f, 1.0f, 0.0f};
                return bromath::Vec3{p[0], p[1], p[2]} - D * (0.35f * cr);
            }
            return bromath::Vec3{p[0], p[1] + 0.1f * cr, p[2]};
        };
        const std::vector<uint32_t> order = groupClumps(s.clumpCount, centre);
        // The upright ellipsoid clumps [i0, i1) of `order` fill: the one with
        // their union's second moments (a solid ellipsoid's are a fifth of
        // its radius squared an axis), each clump's ellipsoid weighted by its
        // volume (its radius across, 0.8 of it times the specimen's flatness
        // up about a point 0.1 of that above its centre, as
        // scene_forest_crown.glsl traces it): its centre, radius across and
        // up, and their leaves. What a crown whose clumps are each a few
        // pixels traces in their place.
        const float flat = s.clumpFlat > 0.0f ? s.clumpFlat : 1.0f;
        const auto fill = [&](uint32_t i0, uint32_t i1, float out[6]) {
            double W = 0.0, sx = 0.0, sy = 0.0, sz = 0.0, leaves = 0.0;
            for (uint32_t i = i0; i < i1; ++i) {
                const float* q = &a.clumps[(s.clumpStart + order[i]) * rec::clump::kFloats];
                const double cr = q[rec::clump::kRadius], w = cr * cr * cr;
                W += w;
                sx += w * q[rec::clump::kCentre];
                sy += w * (q[rec::clump::kCentre + 1] + 0.1 * cr * flat);
                sz += w * q[rec::clump::kCentre + 2];
                leaves += q[rec::clump::kLeafCount];
            }
            W = std::max(W, 1e-12);
            const double fx = sx / W, fy = sy / W, fz = sz / W;
            double across = 0.0, up = 0.0;
            for (uint32_t i = i0; i < i1; ++i) {
                const float* q = &a.clumps[(s.clumpStart + order[i]) * rec::clump::kFloats];
                const double cr = q[rec::clump::kRadius], w = cr * cr * cr, ry = 0.8 * flat * cr;
                const double dx = q[rec::clump::kCentre] - fx, dz = q[rec::clump::kCentre + 2] - fz;
                const double dy = q[rec::clump::kCentre + 1] + 0.1 * cr * flat - fy;
                across += w * (0.5 * (dx * dx + dz * dz) + 0.2 * cr * cr);
                up += w * (dy * dy + 0.2 * ry * ry);
            }
            out[0] = static_cast<float>(fx);
            out[1] = static_cast<float>(fy);
            out[2] = static_cast<float>(fz);
            out[3] = static_cast<float>(std::sqrt(5.0 * across / W));
            out[4] = static_cast<float>(std::sqrt(5.0 * up / W));
            out[5] = static_cast<float>(leaves);
        };
        const uint32_t runs = (s.clumpCount + kClumpBlock - 1) / kClumpBlock;
        const uint32_t groups = (runs + kBlockGroup - 1) / kBlockGroup;
        for (uint32_t g = 0; g < groups; ++g) {
            const uint32_t i0 = g * kBlockGroup * kClumpBlock;
            const uint32_t i1 = std::min(i0 + kBlockGroup * kClumpBlock, s.clumpCount);
            float f[6];
            fill(i0, i1, f);
            float* d = r + (specBlock[k] - (groups - g) * FOREST_GROUP_RECORDS) * 4;
            d[FOREST_GROUP_FILL] = f[0]; d[FOREST_GROUP_FILL + 1] = f[1]; d[FOREST_GROUP_FILL + 2] = f[2];
            d[FOREST_GROUP_FILL + 3] = f[3];
            d[FOREST_GROUP_FILL_UP] = f[4];
            d[FOREST_GROUP_LEAVES] = f[5];
        }
        for (uint32_t b = 0; b * kClumpBlock < s.clumpCount; ++b) {
            const uint32_t i0 = b * kClumpBlock, i1 = std::min(i0 + kClumpBlock, s.clumpCount);
            bromath::Vec3 m{0.0f, 0.0f, 0.0f};
            for (uint32_t i = i0; i < i1; ++i) m = m + centre(order[i]);
            m = m * (1.0f / static_cast<float>(i1 - i0));
            // The least sphere round the clumps' (Badoiu-Clarkson: the centre
            // stepped toward the farthest point, the best kept), not the one
            // about their mean: a ray that misses it skips the run, and the
            // crowns' trace is mostly these tests.
            const auto clumpR = [&](uint32_t i) {
                return a.clumps[(s.clumpStart + order[i]) * rec::clump::kFloats + rec::clump::kRadius];
            };
            const auto reach = [&](const bromath::Vec3& c, uint32_t& far) {
                float best = -1.0f;
                for (uint32_t i = i0; i < i1; ++i) {
                    const float e = bromath::vdist(centre(order[i]), c) + clumpR(i);
                    if (e > best) { best = e; far = i; }
                }
                return best;
            };
            uint32_t far = i0;
            float rad = reach(m, far);
            bromath::Vec3 c = m;
            for (int it = 1; it <= 64; ++it) {
                const bromath::Vec3 to = centre(order[far]) - c;
                const float l = bromath::vlen(to);
                const bromath::Vec3 p = l > 1e-9f ? centre(order[far]) + to * (clumpR(far) / l) : centre(order[far]);
                c = c + (p - c) * (1.0f / static_cast<float>(it + 1));
                const float e = reach(c, far);
                if (e < rad) { rad = e; m = c; }
            }
            float* d = r + (specBlock[k] + b * kBlockRecords) * 4;
            d[FOREST_BLOCK_SPHERE] = m.x; d[FOREST_BLOCK_SPHERE + 1] = m.y; d[FOREST_BLOCK_SPHERE + 2] = m.z;
            d[FOREST_BLOCK_SPHERE + 3] = rad;
            // Its clumps (the specimen's own indices), two to a float, 0xffff
            // past the last.
            for (uint32_t j = 0; j < kClumpBlock; j += 2) {
                const uint32_t lo = i0 + j < i1 ? order[i0 + j] : 0xffffu;
                const uint32_t hi = i0 + j + 1 < i1 ? order[i0 + j + 1] : 0xffffu;
                d[FOREST_BLOCK_CLUMPS + j / 2] = bitsOf(lo | (hi << 16));
            }
            // The ellipsoid the run's clumps fill.
            float f[6];
            fill(i0, i1, f);
            d[FOREST_BLOCK_FILL] = f[0]; d[FOREST_BLOCK_FILL + 1] = f[1]; d[FOREST_BLOCK_FILL + 2] = f[2];
            d[FOREST_BLOCK_FILL + 3] = f[3];
            d[FOREST_BLOCK_FILL_UP] = f[4];
            d[FOREST_BLOCK_LEAVES] = f[5];
        }
    }
    for (size_t i = 0; i < nLeaf; ++i) {
        const uint32_t* l = &a.leaves[i * rec::leaf::kValues];
        float* d = r + leafBase * 4 + i * rec::leaf::kValues;
        d[0] = bitsOf(l[rec::leaf::kClump]);
        d[1] = bitsOf(l[rec::leaf::kKey]);
    }
    for (size_t i = 0; i < instances_.size(); ++i) {
        const Instance& in = instances_[i];
        float* d = r + (instBase + i * FOREST_INSTANCE_RECORDS) * 4;
        d[FOREST_INST_POSITION] = in.position.x; d[FOREST_INST_POSITION + 1] = in.position.y;
        d[FOREST_INST_POSITION + 2] = in.position.z; d[FOREST_INST_SCALE] = in.scale;
        d[FOREST_INST_COS_YAW] = in.cosYaw; d[FOREST_INST_SIN_YAW] = in.sinYaw;
        d[FOREST_INST_SPECIMEN] = bitsOf(in.specimen); d[FOREST_INST_SEED] = in.seed;
    }
    // Each bloom's segment made global (its specimen's run of segments).
    for (size_t k = 0; k < nSpec; ++k) {
        const Specimen& s = specimens_[k];
        for (uint32_t b = s.bloomStart; b < s.bloomStart + s.bloomCount; ++b) {
            const float* src = &a.blooms[b * rec::bloom::kFloats];
            float* d = r + (bloomBase + b * FOREST_BLOOM_RECORDS) * 4;
            std::memcpy(d + FOREST_BLOOM_POSITION, src, rec::bloom::kSegment * sizeof(float));
            d[FOREST_BLOOM_SEGMENT] = bitsOf(s.segStart + static_cast<uint32_t>(src[rec::bloom::kSegment]));
        }
    }
    return gpu_;
}

}  // namespace bro::scene
