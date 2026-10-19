// The forest record format end to end: broflora emits a forest and packs its
// specimens (broflora/forest_records.h), a ForestNode takes the arrays as
// world.emitForest hands them to bro.scene, and every field the node carries
// to the GPU (scene_forest_layout.h's records) holds what the emit said. A
// field moved on one side and not the other fails here, not as a wrong leaf
// colour or a tree swaying on the wrong limb.

#include "scene/forest_node.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"

#include <broflora/forest.h>
#include <broflora/forest_records.h>
#include <broflora/presets.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace broflora;
namespace rec = broflora::forest_records;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

std::string at(const char* field, size_t i) { return std::string(field) + " [" + std::to_string(i) + "]"; }

uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

bool same(float a, float b) { return bits(a) == bits(b); }

// The bark's direction an oct-encoded pair holds (forest_node.cpp's octEncode).
void octDecode(float u, float v, float out[3]) {
    float x = u, y = v, z = 1.0f - std::fabs(u) - std::fabs(v);
    if (z < 0.0f) {
        const float ox = x, oy = y;
        x = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        y = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    }
    const float l = std::sqrt(x * x + y * y + z * z);
    out[0] = x / l; out[1] = y / l; out[2] = z / l;
}

// Two flowering shrubs and a fir (test_forest_blooms.cpp's world): blooms on
// some specimens, none on another, species looks and broad and spray leaves.
WorldState grownWorld() {
    WorldState world;
    world.rngState = 0xb100f;
    world.climate.annualTempBase = 15.0f;
    world.climate.annualPrecip = 1100.0f;
    const float W = 30.0f;
    world.shadow.origin = {-W / 2, -2.0f, -W / 2};
    world.shadow.cellSize = 1.0f;
    world.shadow.width = uint32_t(W);
    world.shadow.height = 40;
    world.shadow.depth = uint32_t(W);
    world.shadow.qg.assign(size_t(world.shadow.width) * world.shadow.height * world.shadow.depth, 1.0f);
    const auto hy = addSpeciesPreset(world, "hydrangea");
    const auto fir = addSpeciesPreset(world, "fir");
    const auto oak = addSpeciesPreset(world, "oak");
    if (hy) plantSpecies(world, *hy, {-5.0f, 0.0f, 0.0f});
    if (fir) plantSpecies(world, *fir, {5.0f, 0.0f, 0.0f});
    if (oak) plantSpecies(world, *oak, {0.0f, 0.0f, 6.0f});
    for (int t = 0; t < 600; ++t) step(world, 0.02f);
    return world;
}

}  // namespace

int main() {
    const WorldState world = grownWorld();
    const ForestEmit f = emitForest(world);
    const rec::Packed p = rec::pack(f);
    const size_t nSpec = f.specimens.size();
    check(nSpec == 3, "three specimens grow");
    check(!f.blooms.empty(), "the hydrangea blooms");
    if (nSpec == 0) return 1;

    // world.emitForest's arrays, as the binding hands them to the node.
    bro::scene::ForestNode node;
    bro::scene::ForestNode::SpecimenArrays a;
    a.specimens = p.specimens;
    a.segments = f.segments;
    a.tangents = f.tangents;
    a.segmentVegetation = f.segmentVegetation;
    a.keepRadius = f.keepRadius;
    a.segmentClumps = f.segmentClumps;
    a.clumpGroups = f.clumpGroups;
    a.clumps = f.clumps;
    a.clumpVegetation = f.clumpVegetation;
    a.leaves = f.leaves;
    const auto& lo = f.leafOptions;
    a.leaf = {lo.seed, lo.upBias, lo.tiltJitter, lo.shell, lo.baseScale, lo.scaleJitter};
    check(node.setSpecimens(std::move(a)), "the node takes the specimens");
    check(node.setBlooms(f.blooms, p.blooms), "the node takes the blooms");
    std::vector<float> looks = p.looks;
    looks.insert(looks.end(), p.leafLooks.begin(), p.leafLooks.end());
    check(node.setSpecimenLooks(looks), "the node takes the looks and leaf looks");
    node.setColor(0.25f, 0.5f, 0.2f);
    node.setBarkColor(0.3f, 0.2f, 0.1f);

    // The packed instances (a specimen where its plant grew), then turned
    // and scaled ones the packing never makes.
    std::vector<float> rows = p.instances;
    for (size_t k = 0; k < nSpec; ++k) {
        float* r = rec::append(rows, rec::instance::kFloats);
        r[rec::instance::kPosition] = 10.0f + float(k);
        r[rec::instance::kPosition + 1] = -1.5f;
        r[rec::instance::kPosition + 2] = 3.25f * float(k);
        r[rec::instance::kScale] = 0.75f + 0.25f * float(k);
        r[rec::instance::kYaw] = 0.5f + float(k);
        r[rec::instance::kSpecimen] = float(nSpec - 1 - k);
    }
    const size_t nInst = rows.size() / rec::instance::kFloats;
    node.setInstances(rows.data(), nInst);
    check(node.instanceCount() == nInst, "every instance is kept");

    // The packed specimens hold ForestSpecimen's fields.
    for (size_t k = 0; k < nSpec; ++k) {
        const ForestSpecimen& s = f.specimens[k];
        const float* r = &p.specimens[k * rec::specimen::kFloats];
        namespace sp = rec::specimen;
        check(r[sp::kSegStart] == float(s.segStart) && r[sp::kSegCount] == float(s.segCount), at("packed segment run", k));
        check(r[sp::kClumpStart] == float(s.clumpStart) && r[sp::kClumpCount] == float(s.clumpCount), at("packed clump run", k));
        check(r[sp::kLeafStart] == float(s.leafStart) && r[sp::kLeafCount] == float(s.leafCount), at("packed leaf run", k));
        check(r[sp::kPlant] == float(s.plant), at("packed plant", k));
        const float* b = &p.blooms[k * rec::specimen_bloom::kFloats];
        check(b[rec::specimen_bloom::kStart] == float(s.bloomStart) && b[rec::specimen_bloom::kCount] == float(s.bloomCount),
              at("packed bloom run", k));
        check(b[rec::specimen_bloom::kForm] == float(s.bloomForm), at("packed bloom form", k));
        const float* in = &p.instances[k * rec::instance::kFloats];
        check(in[rec::instance::kSpecimen] == float(k) && in[rec::instance::kScale] == 1.0f && in[rec::instance::kYaw] == 0.0f,
              at("packed instance", k));
        for (int i = 0; i < 3; ++i)
            check(same(in[rec::instance::kPosition + i], r[sp::kOrigin + i]), at("packed instance at its origin", k));
    }

    // What the node read from them.
    const auto& specs = node.specimens();
    check(specs.size() == nSpec, "the node holds every specimen");
    for (size_t k = 0; k < std::min(specs.size(), nSpec); ++k) {
        const ForestSpecimen& s = f.specimens[k];
        const auto& n = specs[k];
        const float* r = &p.specimens[k * rec::specimen::kFloats];
        check(n.segStart == s.segStart && n.segCount == s.segCount, at("segment run", k));
        check(n.clumpStart == s.clumpStart && n.clumpCount == s.clumpCount, at("clump run", k));
        check(n.leafStart == s.leafStart && n.leafCount == s.leafCount, at("leaf run", k));
        check(same(n.height, r[rec::specimen::kHeight]), at("height", k));
        check(same(n.clumpFlat, r[rec::specimen::kClumpFlat]), at("clump flatness", k));
        const float* b = &p.blooms[k * rec::specimen_bloom::kFloats];
        // A specimen with no blooms keeps no run (its start is wherever the
        // emit had reached).
        const bool blooms = s.bloomForm != 0;
        check(n.bloomStart == (blooms ? s.bloomStart : 0) && n.bloomCount == (blooms ? s.bloomCount : 0), at("bloom run", k));
        check(n.bloomForm == int(s.bloomForm), at("bloom form", k));
        for (int i = 0; i < 3; ++i) check(same(n.bloomColor[i], b[rec::specimen_bloom::kColor + i]), at("bloom colour", k));
    }

    // The GPU records, read by the shaders' names.
    const std::vector<float>& g = node.gpuData();
    check(g.size() >= FOREST_HEADER_FLOATS, "a header");
    if (g.size() < FOREST_HEADER_FLOATS) return 1;
    const float* h = g.data();
    const float* R = h + FOREST_HEADER_FLOATS;
    const auto base = [&](int field) { return bits(h[field]); };
    check(base(FOREST_H_SEED) == lo.seed, "header: leaf seed");
    check(same(h[FOREST_H_UP_BIAS], lo.upBias) && same(h[FOREST_H_TILT_JITTER], lo.tiltJitter) &&
              same(h[FOREST_H_SHELL], lo.shell) && same(h[FOREST_H_BASE_SCALE], lo.baseScale) &&
              same(h[FOREST_H_SCALE_JITTER], lo.scaleJitter),
          "header: leaf placement");
    check(base(FOREST_H_SPECIMENS) == 0, "header: specimens first");
    const size_t records = (g.size() - FOREST_HEADER_FLOATS) / 4;
    for (int field : {FOREST_H_SEGMENTS, FOREST_H_CLUMPS, FOREST_H_LEAVES, FOREST_H_INSTANCES})
        check(base(field) <= records, at("header: a base within the records", size_t(field)));
    const float* specRec = R + size_t(base(FOREST_H_SPECIMENS)) * 4;
    const float* segRec = R + size_t(base(FOREST_H_SEGMENTS)) * 4;
    const float* clumpRec = R + size_t(base(FOREST_H_CLUMPS)) * 4;
    const float* leafRec = R + size_t(base(FOREST_H_LEAVES)) * 4;
    const float* instRec = R + size_t(base(FOREST_H_INSTANCES)) * 4;
    const float* bloomRec = R + size_t(node.bloomRecordBase()) * 4;

    const float node2[6] = {0.25f, 0.5f, 0.2f, 0.3f, 0.2f, 0.1f};
    bool anyLook = false, anyLeafLook = false;
    for (size_t k = 0; k < nSpec; ++k) {
        const ForestSpecimen& s = f.specimens[k];
        const float* d = specRec + k * FOREST_SPECIMEN_RECORDS * 4;
        check(bits(d[FOREST_SP_SEG_START]) == s.segStart && bits(d[FOREST_SP_SEG_COUNT]) == s.segCount, at("gpu segment run", k));
        check(bits(d[FOREST_SP_CLUMP_START]) == s.clumpStart && bits(d[FOREST_SP_CLUMP_COUNT]) == s.clumpCount,
              at("gpu clump run", k));
        check(bits(d[FOREST_SP_LEAF_START]) == s.leafStart && bits(d[FOREST_SP_LEAF_COUNT]) == s.leafCount, at("gpu leaf run", k));
        check(bits(d[FOREST_SP_CROWN_CLUMP]) < std::max(s.clumpCount, 1u) + s.clumpStart, at("gpu crown clump", k));
        check(same(d[FOREST_SP_HEIGHT], p.specimens[k * rec::specimen::kFloats + rec::specimen::kHeight]), at("gpu height", k));
        check(same(d[FOREST_SP_CLUMP_FLAT], p.specimens[k * rec::specimen::kFloats + rec::specimen::kClumpFlat]),
              at("gpu clump flatness", k));
        check(d[FOREST_SP_FREE] == 0.0f, at("gpu free lane", k));
        const float* b = &p.blooms[k * rec::specimen_bloom::kFloats];
        for (int i = 0; i < 3; ++i)
            check(same(d[FOREST_SP_BLOOM_COLOR + i], b[rec::specimen_bloom::kColor + i]), at("gpu bloom colour", k));
        check(bits(d[FOREST_SP_BLOOM_FORM]) == s.bloomForm, at("gpu bloom form", k));
        // The leaf look, field by field.
        const float* L = &p.leafLooks[k * rec::leaf_look::kFloats];
        namespace ll = rec::leaf_look;
        check(same(d[FOREST_SP_LEAF], L[ll::kWidth]) && same(d[FOREST_SP_LEAF + 1], L[ll::kTip]) &&
                  same(d[FOREST_SP_LEAF + 2], L[ll::kTeeth]) && same(d[FOREST_SP_LEAF + 3], L[ll::kToothDepth]),
              at("gpu broad leaf", k));
        check(same(d[FOREST_SP_GLOSS], L[ll::kGloss]), at("gpu gloss", k));
        check(same(d[FOREST_SP_BRUSH], L[ll::kSprayBrush]), at("gpu spray brush", k));
        check(same(d[FOREST_SP_PETIOLE], L[ll::kPetiole]), at("gpu petiole", k));
        check(same(d[FOREST_SP_PHYLLOTAXIS], L[ll::kPhyllotaxis]), at("gpu phyllotaxis", k));
        for (int i = 0; i < ll::kFloats; ++i) anyLeafLook |= L[i] != 0.0f;
        // The look: a tint, its colour over the node's.
        for (int part = 0; part < 2; ++part) {
            const float* look = &p.looks[k * rec::look::kFloats + (part ? rec::look::kBarkColor : rec::look::kLeafColor)];
            const float* t = d + (part ? FOREST_SP_BARK_TINT : FOREST_SP_LEAF_TINT);
            const bool has = look[0] + look[1] + look[2] > 0.0f;
            anyLook |= has;
            for (int i = 0; i < 3; ++i) {
                const float want = has ? std::clamp(look[i] / node2[part * 3 + i], 0.05f, 8.0f) : 1.0f;
                check(same(t[i], want), at(part ? "gpu bark tint" : "gpu leaf tint", k));
            }
        }
    }
    check(anyLook, "some specimen has its own colours");
    check(anyLeafLook, "some specimen has its own leaf");

    const size_t nSeg = f.segments.size() / rec::segment::kFloats;
    for (size_t i = 0; i < nSeg; ++i) {
        const float* w = &f.segments[i * rec::segment::kFloats];
        const float* d = segRec + i * FOREST_SEGMENT_RECORDS * 4;
        for (int c = 0; c < 3; ++c) {
            check(same(d[FOREST_SEG_FROM + c], w[rec::segment::kFrom + c]), at("segment from", i));
            check(same(d[FOREST_SEG_TO + c], w[rec::segment::kTo + c]), at("segment to", i));
        }
        check(same(d[FOREST_SEG_R_FROM], w[rec::segment::kRFrom]), at("segment from radius", i));
        // The end radius narrows to what goes on from it, negative at a tip.
        check(std::fabs(d[FOREST_SEG_R_TO]) <= w[rec::segment::kRTo] * 1.0001f + 1e-7f, at("segment to radius", i));
        for (int e = 0; e < 2; ++e) {
            const float* t = &f.tangents[i * rec::tangent::kFloats + (e ? rec::tangent::kTo : rec::tangent::kFrom)];
            const float l = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (l < 1e-6f) continue;
            float got[3];
            octDecode(d[FOREST_SEG_TANGENTS + 2 * e], d[FOREST_SEG_TANGENTS + 2 * e + 1], got);
            check(got[0] * t[0] / l + got[1] * t[1] / l + got[2] * t[2] / l > 0.9999f, at("segment tangent", i));
        }
        const float* v = &f.segmentVegetation[i * rec::vegetation::kFloats];
        for (int c = 0; c < int(rec::vegetation::kFloats); ++c)
            check(same(d[FOREST_SEG_VEGETATION + c], v[c]), at("segment vegetation", i));
    }
    // Each segment's few clumps, made global (its specimen's run).
    size_t listed = 0;
    for (size_t k = 0; k < nSpec; ++k) {
        const ForestSpecimen& s = f.specimens[k];
        for (uint32_t i = s.segStart; i < s.segStart + s.segCount; ++i) {
            const float* d = segRec + size_t(i) * FOREST_SEGMENT_RECORDS * 4;
            for (size_t j = 0; j < rec::segment_clumps::kMax; ++j) {
                const float w = f.segmentClumps[i * rec::segment_clumps::kFloats + rec::segment_clumps::kClumps + j];
                const uint32_t want = w >= 0.0f ? s.clumpStart + uint32_t(w) : FOREST_SEG_NO_CLUMP;
                check(bits(d[FOREST_SEG_CLUMPS + j]) == want, at("segment clumps", i));
                listed += w >= 0.0f;
            }
        }
    }
    check(listed > 0, "some segments list their clumps");
    // Each clump's drop group, as its ordinal in its specimen.
    size_t grouped = 0;
    for (size_t k = 0; k < nSpec; ++k) {
        const ForestSpecimen& s = f.specimens[k];
        std::map<float, uint32_t> ordinal;
        for (uint32_t c = s.clumpStart; c < s.clumpStart + s.clumpCount; ++c) {
            const uint32_t want = ordinal.emplace(f.clumpGroups[c], uint32_t(ordinal.size())).first->second;
            check(bits(clumpRec[size_t(c) * FOREST_CLUMP_RECORDS * 4 + FOREST_CLUMP_GROUP]) == want, at("clump group", c));
            grouped += f.clumpGroups[c] != float(c - s.clumpStart);
        }
    }
    check(grouped > 0, "some clumps drop with others");

    const size_t nClump = f.clumps.size() / rec::clump::kFloats;
    for (size_t i = 0; i < nClump; ++i) {
        const float* w = &f.clumps[i * rec::clump::kFloats];
        const float* d = clumpRec + i * FOREST_CLUMP_RECORDS * 4;
        for (int c = 0; c < 3; ++c) {
            check(same(d[FOREST_CLUMP_CENTRE + c], w[rec::clump::kCentre + c]), at("clump centre", i));
            check(same(d[FOREST_CLUMP_TWIG + c], w[rec::clump::kTwig + c]), at("clump twig", i));
        }
        check(same(d[FOREST_CLUMP_RADIUS], w[rec::clump::kRadius]), at("clump radius", i));
        check(same(d[FOREST_CLUMP_LEAVES], w[rec::clump::kLeafCount]), at("clump leaves", i));
        // The vegetation record, but for its path: the clump's placement-key base.
        const float* v = &f.clumpVegetation[i * rec::vegetation::kFloats];
        for (int c = 0; c < int(rec::vegetation::kFloats); ++c)
            if (FOREST_CLUMP_VEGETATION + c != FOREST_CLUMP_KEY_BASE)
                check(same(d[FOREST_CLUMP_VEGETATION + c], v[c]), at("clump vegetation", i));
    }
    // Each leaf as it was, and each clump's key base its leaves' first key
    // (the leaves are shuffled; a clump's keys are one run).
    const size_t nLeaf = f.leaves.size() / rec::leaf::kValues;
    std::vector<uint32_t> keyBase(nClump, ~0u);
    for (size_t i = 0; i < nLeaf; ++i) {
        const uint32_t clump = f.leaves[i * rec::leaf::kValues + rec::leaf::kClump];
        const uint32_t key = f.leaves[i * rec::leaf::kValues + rec::leaf::kKey];
        check(bits(leafRec[i * rec::leaf::kValues]) == clump && bits(leafRec[i * rec::leaf::kValues + 1]) == key,
              at("leaf", i));
        if (clump < nClump) keyBase[clump] = std::min(keyBase[clump], key);
    }
    for (size_t c = 0; c < nClump; ++c)
        check(bits(clumpRec[c * FOREST_CLUMP_RECORDS * 4 + FOREST_CLUMP_KEY_BASE]) == (keyBase[c] == ~0u ? 0u : keyBase[c]),
              at("clump key base", c));

    for (size_t i = 0; i < nInst; ++i) {
        const float* w = &rows[i * rec::instance::kFloats];
        const float* d = instRec + i * FOREST_INSTANCE_RECORDS * 4;
        for (int c = 0; c < 3; ++c) check(same(d[FOREST_INST_POSITION + c], w[rec::instance::kPosition + c]), at("instance position", i));
        check(same(d[FOREST_INST_SCALE], w[rec::instance::kScale]), at("instance scale", i));
        const float yaw = w[rec::instance::kYaw];
        check(std::fabs(d[FOREST_INST_COS_YAW] - std::cos(yaw)) < 1e-6f && std::fabs(d[FOREST_INST_SIN_YAW] - std::sin(yaw)) < 1e-6f,
              at("instance yaw", i));
        check(bits(d[FOREST_INST_SPECIMEN]) == uint32_t(w[rec::instance::kSpecimen]), at("instance specimen", i));
    }

    for (size_t k = 0; k < nSpec; ++k) {
        const ForestSpecimen& s = f.specimens[k];
        for (uint32_t b = s.bloomStart; b < s.bloomStart + s.bloomCount; ++b) {
            const float* w = &f.blooms[b * rec::bloom::kFloats];
            const float* d = bloomRec + b * FOREST_BLOOM_RECORDS * 4;
            for (int c = 0; c < 3; ++c) {
                check(same(d[FOREST_BLOOM_POSITION + c], w[rec::bloom::kPosition + c]), at("bloom position", b));
                check(same(d[FOREST_BLOOM_DIR + c], w[rec::bloom::kDir + c]), at("bloom direction", b));
            }
            check(same(d[FOREST_BLOOM_SIZE], w[rec::bloom::kSize]), at("bloom size", b));
            check(bits(d[FOREST_BLOOM_SEGMENT]) == s.segStart + uint32_t(w[rec::bloom::kSegment]), at("bloom segment", b));
        }
    }

    std::printf("forest records (%zu segment clumps listed): %d checks, %d failed (%zu specimens, %zu segments, %zu clumps, %zu leaves, %zu blooms)\n",
                listed, g_checks, g_failures, nSpec, nSeg, nClump, nLeaf, f.blooms.size() / rec::bloom::kFloats);
    return g_failures == 0 ? 0 : 1;
}
