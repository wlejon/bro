// bro_probe_grid_gpu_test: the probe grid's world (scene/vulkan/lighting/
// scene_probe_grid.h) against the scene it was made from.
//
// A tilted plane of ground 300 m across with a box standing on it, a few
// round crowns over it and a sun. One render, then the eye moved a few
// columns and rendered again (the finest cascade's world remade where it now
// stands). Each time the world is read back and checked:
//  - the ground's height: every texel inside the ground's extent within 2 cm
//    of the plane (or the box's top), every texel past it a hole. The
//    capture is an orthographic raster of a plane, exact but for the depth's
//    precision (about 2 mm at 20 km);
//  - the canopy: each cascade's voxels against the crowns' share of them,
//    worked out on the CPU (each crown sampled every 5 cm, a point going to
//    the voxel at its height above the plane). The total within 1% (the
//    splat shares each crown's whole volume among its points); the centre of
//    the mass within a twentieth of a voxel across and 20 cm up (the
//    coarsest cascade's crowns have a few points a layer); the mean
//    extinction of each block of 2 x 2 x 2 voxels within a mean error of 6%
//    and a worst of 30% (the splat's points are a few dozen a voxel, so a
//    block the crown's edge cuts is off by a few); the albedo of a filled
//    voxel its species' mean;
//  - the sun's transmittance: the finest cascade's voxels against the same
//    march through the reference's voxels (filtered as the sampler filters):
//    the total shade (the sum of 1 - T) within 2%, a voxel's error 0.01 on
//    average and 0.1 at worst. Against the crowns as they are, not voxels,
//    the shade is about 16% more (exp(-tau) is convex: a medium blurred a
//    voxel wide shades more than a sharp one); under a quarter more is
//    checked.
//
// Then the probes, each world rendered frames a tenth of a second apart
// (fresh probes traced, then refreshed) and read back:
//  - a black crown over black ground under a flat sky of 1: the probes of
//    four columns through it and past it, every layer, against the sky
//    marched through the medium the GPU made (5 cm steps, 4096 directions,
//    projected to the same SH): the irradiance on surfaces facing up, down
//    and across within 0.03 (0.01 on average), the crown's total shade within
//    5%; and a probe in the open, the sky's 1 above within 2%, 0 below;
//  - sunlit ground of albedo 0.3 under a sky of 0.5: probes of two cascades
//    hold the sky above within 2% and the ground's light below (its albedo
//    of the sun's and of the sky's, which reaches the ground through the
//    probes over it) within 3%.

#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "scene/nodes/light_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/nodes/plants_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/lighting/scene_probe_grid.h"

#include <bromesh/primitives/primitives.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace bro;
using scene::vk::ProbeGridTables;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok" : "FAILED", what);
    if (!ok) ++failures;
}

double plane(double x, double z) { return 3.0 + 0.1 * x - 0.05 * z; }

constexpr double kGroundHalf = 150.0;
// The box: half extents, its middle on the plane. (Past the finest
// cascade's square, where the sun's check is: its shade is the ground's.)
constexpr double kBoxX = 40.0, kBoxZ = -40.0, kBoxHalf = 2.0, kBoxHalfY = 1.5;
double boxTop() { return plane(kBoxX, kBoxZ) + 2.0 * kBoxHalfY; }

struct Crown {
    double x, z, scale;
};
const Crown kCrowns[] = {{-6.0, -10.0, 1.0}, {5.0, -15.0, 1.0}, {0.0, 12.0, 1.0}, {-20.0, -20.0, 1.5}};
// The species: 12 m tall, its crown from 0.3 of that up, 3 m across, round.
constexpr double kHeight = 12.0, kBase = 0.3, kRadius = 3.0, kDensity = 0.8;
const float kLight[3] = {0.2f, 0.4f, 0.1f}, kDark[3] = {0.05f, 0.12f, 0.03f};

/// Crown `c`'s middle height and radii.
void crownShape(const Crown& c, double& cy, double& rx, double& ry) {
    const double h = kHeight * c.scale, b = h * kBase;
    cy = plane(c.x, c.z) + 0.5 * (b + h);
    ry = 0.5 * (h - b);
    rx = kRadius * c.scale;
}

/// Whether world point (x, y, z) is inside crown `c`.
bool inCrown(const Crown& c, double x, double y, double z) {
    double cy, rx, ry;
    crownShape(c, cy, rx, ry);
    const double v = (y - cy) / ry;
    if (std::abs(v) > 1.0) return false;
    const double w = std::sqrt(std::max(1.0 - v * v, 0.0));   // round: shape 0
    const double dx = x - c.x, dz = z - c.z;
    return dx * dx + dz * dz <= w * w * rx * rx;
}

bool inCrown(double x, double y, double z) {
    for (const Crown& c : kCrowns)
        if (inCrown(c, x, y, z)) return true;
    return false;
}

void buildScene(scene::SceneGraph& graph, float eyeX, float eyeZ) {
    graph.setCanvasSize(256, 144);
    const float eyeY = static_cast<float>(plane(eyeX, eyeZ) + 1.7);
    graph.setCamera(1.0f, 256.0f / 144.0f, 0.1f, 2000.0f, {eyeX, eyeY, eyeZ}, {eyeX, eyeY, eyeZ - 10.0f});

    constexpr int kGrid = 151;
    std::vector<float> heights(static_cast<size_t>(kGrid) * kGrid);
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x)
            heights[static_cast<size_t>(z) * kGrid + x] =
                static_cast<float>(plane(-kGroundHalf + 2.0 * x, -kGroundHalf + 2.0 * z));
    scene::MeshNode* ground = graph.createMesh("ground");
    ground->setMesh(bromesh::heightmapGrid(heights.data(), kGrid, kGrid, 2.0f));
    graph.root()->addChild(ground);

    scene::MeshNode* box = graph.createMesh("box");
    box->setMesh(bromesh::box(static_cast<float>(kBoxHalf), static_cast<float>(kBoxHalfY),
                              static_cast<float>(kBoxHalf)));
    box->setPosition({static_cast<float>(kBoxX), static_cast<float>(plane(kBoxX, kBoxZ) + kBoxHalfY),
                      static_cast<float>(kBoxZ)});
    graph.root()->addChild(box);

    scene::PlantsNode* plants = graph.createPlants("plants");
    float species[scene::PlantsNode::kSpeciesFloats] = {
        static_cast<float>(kHeight), static_cast<float>(kBase), static_cast<float>(kRadius), 0.0f,
        12.0f, 0.4f, 0.2f, 0.5f,
        2000.0f, 0.2f, 0.5f, 0.2f,
        kLight[0], kLight[1], kLight[2], 0.1f,
        kDark[0], kDark[1], kDark[2], 0.3f,
        0.3f, 0.2f, 0.1f, static_cast<float>(kDensity),
        0.5f, 0.8f, 0.2f, 0.0f};
    plants->setSpecies(species, 1);
    std::vector<float> records;
    int seed = 1;
    for (const Crown& c : kCrowns) {
        const float r[scene::PlantsNode::kPlantFloats] = {
            static_cast<float>(c.x), static_cast<float>(plane(c.x, c.z)), static_cast<float>(c.z), 0.3f,
            static_cast<float>(c.scale), 0.0f, static_cast<float>(seed++), 0.0f};
        records.insert(records.end(), r, r + scene::PlantsNode::kPlantFloats);
    }
    plants->setPlants(records.data(), static_cast<int>(std::size(kCrowns)));
    graph.root()->addChild(plants);

    scene::LightNode* sun = graph.createLight("sun");
    sun->setKind(scene::LightNode::Kind::Directional);
    sun->setDirection({-0.3f, -1.0f, -0.5f});
    sun->setIntensity(3.0f);
    graph.root()->addChild(sun);

    graph.renderer().setProbeGrid(true);
}

struct Cascade {
    double x0, z0;   // its world's corner (world metres)
    double s;        // column spacing
};

Cascade cascadeOf(const ProbeGridTables& t, int c) {
    const auto& b = t.block;
    return {b.cascade[c][0] + b.eye[0], b.cascade[c][1] + b.eye[2], b.cascade[c][2]};
}

void checkHeight(const ProbeGridTables& t) {
    const int cascades = static_cast<int>(t.block.params[1]);
    double worst = 0.0;
    int texels = 0, holesWrong = 0;
    for (int c = 0; c < cascades; ++c) {
        const Cascade k = cascadeOf(t, c);
        const double texel = k.s * PG_COLUMNS / PG_HEIGHT;
        for (int j = 0; j < PG_HEIGHT; ++j) {
            for (int i = 0; i < PG_HEIGHT; ++i) {
                const double x = k.x0 + (i + 0.5) * texel, z = k.z0 + (j + 0.5) * texel;
                const float got = t.height[static_cast<size_t>(c * PG_HEIGHT + j) * PG_HEIGHT + i];
                // Texels the ground's edge or the box's cuts are left out.
                const double edge = std::max(std::abs(x), std::abs(z));
                if (edge > kGroundHalf + texel) {
                    if (got > PG_HOLE_TEST) ++holesWrong;
                    continue;
                }
                if (edge > kGroundHalf - texel) continue;
                const double bx = std::abs(x - kBoxX), bz = std::abs(z - kBoxZ);
                double want = plane(x, z);
                if (bx < kBoxHalf - texel && bz < kBoxHalf - texel) want = boxTop();
                else if (bx < kBoxHalf + texel && bz < kBoxHalf + texel) continue;
                worst = std::max(worst, std::abs(static_cast<double>(got) - want));
                ++texels;
            }
        }
    }
    std::printf("  ground: %d texels, worst %.4f m; %d holes not holes\n", texels, worst, holesWrong);
    check(texels > 0 && worst < 0.02, "the ground's height within 2 cm of the plane and the box");
    check(holesWrong == 0, "past the ground, holes");
}

size_t voxelIndex(int c, int i, int j, int k) {
    return (static_cast<size_t>(c * PG_LAYERS_Y + k) * PG_VOXELS + j) * PG_VOXELS + i;
}

/// The reference canopy of cascade c: each crown's box sampled every 5 cm,
/// a point inside it carrying its volume into the voxel it is in (by its
/// height above the plane), as the splat's do. One cascade's voxels.
std::vector<double> referenceMedium(const ProbeGridTables& t, int c) {
    const double top = t.block.params[2], voxY = top / PG_LAYERS_Y;
    const Cascade k = cascadeOf(t, c);
    const double vox = 0.5 * k.s;
    const double voxVol = vox * vox * voxY;
    std::vector<double> want(static_cast<size_t>(PG_VOXELS) * PG_VOXELS * PG_LAYERS_Y, 0.0);
    constexpr double d = 0.05;
    for (const Crown& cr : kCrowns) {
        double cy, rx, ry;
        crownShape(cr, cy, rx, ry);
        for (double x = cr.x - rx + 0.5 * d; x < cr.x + rx; x += d)
            for (double z = cr.z - rx + 0.5 * d; z < cr.z + rx; z += d)
                for (double y = cy - ry + 0.5 * d; y < cy + ry; y += d) {
                    if (!inCrown(cr, x, y, z)) continue;
                    const int i = static_cast<int>(std::floor((x - k.x0) / vox));
                    const int j = static_cast<int>(std::floor((z - k.z0) / vox));
                    const int kk = static_cast<int>(std::floor((y - plane(x, z)) / voxY));
                    if (i < 0 || j < 0 || kk < 0 || i >= PG_VOXELS || j >= PG_VOXELS || kk >= PG_LAYERS_Y) continue;
                    want[voxelIndex(0, i, j, kk)] += kDensity * d * d * d / voxVol;
                }
    }
    return want;
}

void checkCanopy(const ProbeGridTables& t, int cascades) {
    const double albedo[3] = {0.5 * (kLight[0] + kDark[0]), 0.5 * (kLight[1] + kDark[1]),
                              0.5 * (kLight[2] + kDark[2])};
    for (int c = 0; c < cascades; ++c) {
        const double vox = 0.5 * cascadeOf(t, c).s;
        const std::vector<double> want = referenceMedium(t, c);
        // The totals and the centres of the mass (x, z in voxels, h in layers).
        double sumGot = 0.0, sumWant = 0.0, albedoWorst = 0.0;
        double mGot[3] = {}, mWant[3] = {};
        for (int kk = 0; kk < PG_LAYERS_Y; ++kk)
            for (int j = 0; j < PG_VOXELS; ++j)
                for (int i = 0; i < PG_VOXELS; ++i) {
                    const float* m = &t.medium[voxelIndex(c, i, j, kk) * 4];
                    const double got = m[0], w = want[voxelIndex(0, i, j, kk)];
                    sumGot += got;
                    sumWant += w;
                    const double at[3] = {i + 0.5, j + 0.5, kk + 0.5};
                    for (int a = 0; a < 3; ++a) {
                        mGot[a] += got * at[a];
                        mWant[a] += w * at[a];
                    }
                    if (got > 0.05 * kDensity)
                        for (int ch = 0; ch < 3; ++ch)
                            albedoWorst = std::max(albedoWorst, std::abs(m[1 + ch] - albedo[ch]) / albedo[ch]);
                }
        // Each block of 2 x 2 x 2 voxels, the mean extinction in it.
        double errSum = 0.0, worst = 0.0;
        int compared = 0;
        for (int kk = 0; kk < PG_LAYERS_Y; kk += 2)
            for (int j = 0; j < PG_VOXELS; j += 2)
                for (int i = 0; i < PG_VOXELS; i += 2) {
                    double got = 0.0, w = 0.0;
                    for (int a = 0; a < 8; ++a) {
                        const size_t v = voxelIndex(0, i + (a & 1), j + ((a >> 1) & 1), kk + (a >> 2));
                        got += t.medium[(voxelIndex(c, 0, 0, 0) + v) * 4] / 8.0;
                        w += want[v] / 8.0;
                    }
                    if (got <= 0.0 && w <= 0.0) continue;
                    const double e = std::abs(got - w) / kDensity;
                    errSum += e;
                    worst = std::max(worst, e);
                    ++compared;
                }
        const double total = sumWant > 0.0 ? std::abs(sumGot - sumWant) / sumWant : (sumGot > 0.0 ? 1.0 : 0.0);
        const double mean = compared ? errSum / compared : 0.0;
        double off[3] = {};
        for (int a = 0; a < 3 && sumGot > 0.0 && sumWant > 0.0; ++a) off[a] = mGot[a] / sumGot - mWant[a] / sumWant;
        const double voxY = t.block.params[2] / PG_LAYERS_Y;
        std::printf("  canopy, cascade %d (%.1f m voxels): total %.4g vs %.4g (%.2f%%), centre off by (%.3f, %.3f, "
                    "%.3f) voxels; %d blocks, mean error %.3f, worst %.3f; albedo worst %.3f\n",
                    c, vox, sumGot, sumWant, 100.0 * total, off[0], off[1], off[2], compared, mean, worst,
                    albedoWorst);
        if (sumWant <= 0.0) continue;
        check(total < 0.01, "the canopy's total within 1%");
        // (A crown in the coarsest cascade has its fewest points, a few a
        // layer, so its height is a dozen centimetres out.)
        check(std::max(std::abs(off[0]), std::abs(off[1])) < 0.05 && std::abs(off[2]) * voxY < 0.2,
              "its centre within a twentieth of a voxel across, 20 cm up");
        check(mean < 0.06 && worst < 0.3, "a block's extinction: mean error under 6%, worst under 30%");
        check(albedoWorst < 0.01, "a filled voxel's albedo its species' mean");
    }
}

/// Cascade 0's reference medium at world (x, h above the plane, z), filtered
/// as the GPU's sampler filters its voxels (trilinear, the layers clamped to
/// their middles); nothing past the cascade (no crown stands there).
double sampleMedium(const std::vector<double>& m, const Cascade& k, double top, double x, double h, double z) {
    const double vox = 0.5 * k.s;
    const double fx = (x - k.x0) / vox - 0.5, fz = (z - k.z0) / vox - 0.5;
    if (fx < -0.5 || fz < -0.5 || fx > PG_VOXELS - 0.5 || fz > PG_VOXELS - 0.5) return 0.0;
    const double fy = std::clamp(h / top * PG_LAYERS_Y, 0.5, PG_LAYERS_Y - 0.5) - 0.5;
    const double f[3] = {std::clamp(fx, 0.0, PG_VOXELS - 1.0), std::clamp(fz, 0.0, PG_VOXELS - 1.0), fy};
    const int i0[3] = {static_cast<int>(f[0]), static_cast<int>(f[1]), static_cast<int>(f[2])};
    const int lim[3] = {PG_VOXELS - 1, PG_VOXELS - 1, PG_LAYERS_Y - 1};
    double sum = 0.0;
    for (int a = 0; a < 8; ++a) {
        int idx[3];
        double w = 1.0;
        for (int d = 0; d < 3; ++d) {
            const int up = (a >> d) & 1;
            idx[d] = std::min(i0[d] + up, lim[d]);
            const double fr = f[d] - i0[d];
            w *= up ? fr : 1.0 - fr;
        }
        sum += w * m[voxelIndex(0, idx[0], idx[1], idx[2])];
    }
    return sum;
}

void checkSun(const ProbeGridTables& t) {
    const double top = t.block.params[2], voxY = top / PG_LAYERS_Y;
    const double L[3] = {t.block.sun[0], t.block.sun[1], t.block.sun[2]};
    const Cascade k = cascadeOf(t, 0);
    const double vox = 0.5 * k.s;
    const std::vector<double> medium = referenceMedium(t, 0);
    double shadeGot = 0.0, shadeWant = 0.0, shadeCrowns = 0.0, errSum = 0.0, worst = 0.0;
    int compared = 0;
    for (int kk = 0; kk < PG_LAYERS_Y; ++kk)
        for (int j = 0; j < PG_VOXELS; ++j)
            for (int i = 0; i < PG_VOXELS; ++i) {
                const double x = k.x0 + (i + 0.5) * vox, z = k.z0 + (j + 0.5) * vox;
                const double p[3] = {x, plane(x, z) + (kk + 0.5) * voxY, z};
                // The GPU's march through the reference's voxels: half a
                // voxel a step (shorter across the layers), the medium at
                // each step's middle, ended by the ground.
                double want = 1.0;
                double q[3] = {p[0], p[1], p[2]};
                for (int n = 0; n < 512 && want > 1e-3; ++n) {
                    const double h = q[1] - plane(q[0], q[2]);
                    if (h < 0.0) {
                        want = 0.0;
                        break;
                    }
                    if (std::abs(q[0] - k.x0 - 32.0 * k.s) > 32.0 * k.s ||
                        std::abs(q[2] - k.z0 - 32.0 * k.s) > 32.0 * k.s)
                        break;   // past cascade 0: no crowns, a rising ray
                    double step = 0.5 * vox;
                    if (h < top) {
                        step = std::min(step, 0.5 * voxY / std::max(std::abs(L[1]), 0.05));
                        const double m[3] = {q[0] + 0.5 * step * L[0], q[1] + 0.5 * step * L[1],
                                             q[2] + 0.5 * step * L[2]};
                        const double hm = m[1] - plane(m[0], m[2]);
                        if (hm >= 0.0 && hm < top) want *= std::exp(-sampleMedium(medium, k, top, m[0], hm, m[2]) * step);
                    }
                    for (int d = 0; d < 3; ++d) q[d] += L[d] * step;
                }
                // The crowns as they are, not voxels: how much shade the
                // voxels' blur adds (exp(-tau) is convex, so a blurred medium
                // shades more than a sharp one).
                double tau = 0.0;
                constexpr double kStep = 0.05;
                for (double s = 0.0; s < 80.0; s += kStep)
                    if (inCrown(p[0] + L[0] * s, p[1] + L[1] * s, p[2] + L[2] * s)) tau += kDensity * kStep;
                const double crowns = std::exp(-tau);
                const double got = t.sun[voxelIndex(0, i, j, kk)];
                if (want > 0.999 && got > 0.999) continue;
                shadeGot += 1.0 - got;
                shadeWant += 1.0 - want;
                shadeCrowns += 1.0 - crowns;
                const double e = std::abs(got - want);
                errSum += e;
                worst = std::max(worst, e);
                ++compared;
            }
    const double total = shadeWant > 0.0 ? std::abs(shadeGot - shadeWant) / shadeWant : 1.0;
    const double mean = compared ? errSum / compared : 0.0;
    std::printf("  sun, cascade 0: shade %.4g vs %.4g (%.1f%%), %d voxels, mean error %.4f, worst %.3f; the sharp "
                "crowns' shade %.4g (the voxels add %.1f%%)\n",
                shadeGot, shadeWant, 100.0 * total, compared, mean, worst, shadeCrowns,
                100.0 * (shadeGot / shadeCrowns - 1.0));
    check(total < 0.02, "the canopy's shade within 2% of the march through its voxels");
    check(mean < 0.01 && worst < 0.1, "each voxel's transmittance: mean error under 0.01, worst under 0.1");
    check(std::abs(shadeGot / shadeCrowns - 1.0) < 0.25, "the voxels' blur adds under a quarter to the shade");
}

// --- The probes ------------------------------------------------------------

/// A world for the traces: level ground at y = 0 under a flat sky, with a
/// black crown over the middle (it only shades), or a sun and a ground that
/// sends light back.
struct TraceScene {
    bool crown = false;
    bool sun = false;
    float ambient = 1.0f;   // the sky's radiance
    float albedo = 0.0f;    // the ground's
};
constexpr double kTraceSun[3] = {-0.3, -1.0, -0.5};
constexpr double kTraceSunE = 3.0;

void buildTraceScene(scene::SceneGraph& graph, const TraceScene& s) {
    graph.setCanvasSize(256, 144);
    graph.setCamera(1.0f, 256.0f / 144.0f, 0.1f, 2000.0f, {0.0f, 1.7f, 0.0f}, {0.0f, 1.7f, -10.0f});
    // Out to the coarsest cascade's edge (a high probe's low rays reach it).
    constexpr int kGrid = 206;
    std::vector<float> heights(static_cast<size_t>(kGrid) * kGrid, 0.0f);
    scene::MeshNode* ground = graph.createMesh("ground");
    ground->setMesh(bromesh::heightmapGrid(heights.data(), kGrid, kGrid, 20.0f));
    graph.root()->addChild(ground);
    if (s.crown) {
        scene::PlantsNode* plants = graph.createPlants("plants");
        float species[scene::PlantsNode::kSpeciesFloats] = {
            static_cast<float>(kHeight), static_cast<float>(kBase), static_cast<float>(kRadius), 0.0f,
            12.0f, 0.4f, 0.2f, 0.5f,
            2000.0f, 0.2f, 0.5f, 0.2f,
            0.0f, 0.0f, 0.0f, 0.1f,
            0.0f, 0.0f, 0.0f, 0.3f,
            0.3f, 0.2f, 0.1f, static_cast<float>(kDensity),
            0.5f, 0.8f, 0.2f, 0.0f};
        plants->setSpecies(species, 1);
        const float r[scene::PlantsNode::kPlantFloats] = {0.0f, 0.0f, 0.0f, 0.3f, 1.5f, 0.0f, 1.0f, 0.0f};
        plants->setPlants(r, 1);
        graph.root()->addChild(plants);
    }
    if (s.sun) {
        scene::LightNode* sun = graph.createLight("sun");
        sun->setKind(scene::LightNode::Kind::Directional);
        sun->setDirection({static_cast<float>(kTraceSun[0]), static_cast<float>(kTraceSun[1]),
                           static_cast<float>(kTraceSun[2])});
        sun->setIntensity(static_cast<float>(kTraceSunE));
        graph.root()->addChild(sun);
    }
    graph.renderer().setAmbient(s.ambient, s.ambient, s.ambient);
    graph.renderer().setGroundBounce(s.albedo, s.albedo, s.albedo);
    graph.renderer().setProbeGrid(true);
}

/// A probe's SH, a channel: (c0, cx, cy, cz).
struct Sh {
    double c[3][4];
};

Sh probeSh(const ProbeGridTables& t, int c, int i, int j, int k) {
    const int x = (static_cast<int>(t.block.wrap[c][0]) + i) % PG_COLUMNS;
    const int y = (static_cast<int>(t.block.wrap[c][1]) + j) % PG_COLUMNS;
    const size_t texel = (static_cast<size_t>(c * PG_LAYERS + k) * PG_COLUMNS + y) * PG_COLUMNS + x;
    Sh s{};
    for (int ch = 0; ch < 3; ++ch)
        for (int a = 0; a < 4; ++a) s.c[ch][a] = t.sh[ch][texel * 4 + a];
    return s;
}

/// The irradiance over pi the SH gives a surface facing `n` (red).
double shIrradiance(const double c[4], const double n[3]) {
    return c[0] * PG_SH_Y00 + (2.0 / 3.0) * PG_SH_Y1 * (c[1] * n[0] + c[2] * n[1] + c[3] * n[2]);
}

double layerHeight(const ProbeGridTables& t, int k) {
    const double top = t.block.params[2], bottom = t.block.params[3];
    return bottom + (top - bottom) * std::pow(k / static_cast<double>(PG_LAYERS - 1), t.block.eye[3]);
}

/// The SH (red) a probe at `p` should hold in the crown's world: the sky
/// through the medium the GPU made (cascade 0's, filtered as the sampler
/// filters it, marched in 5 cm steps) above, the black ground below.
void crownReference(const ProbeGridTables& t, const std::vector<double>& medium, const double p[3], double out[4]) {
    const double top = t.block.params[2];
    const Cascade k = cascadeOf(t, 0);
    constexpr int kDirs = 4096;
    constexpr double kStep = 0.05;
    // The crown's voxels (and their filter) lie inside this box.
    const double lo[3] = {-8.0, 0.0, -8.0}, hi[3] = {8.0, top, 8.0};
    for (int a = 0; a < 4; ++a) out[a] = 0.0;
    for (int n = 0; n < kDirs; ++n) {
        const double y = 1.0 - (2.0 * n + 1.0) / kDirs;
        if (y <= 0.0) continue;   // the ground: black
        const double r = std::sqrt(1.0 - y * y), phi = n * 2.39996323;
        const double d[3] = {std::cos(phi) * r, y, std::sin(phi) * r};
        double t0 = 0.0, t1 = 1e9;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(d[a]) < 1e-9) {
                if (p[a] < lo[a] || p[a] > hi[a]) t1 = -1.0;
                continue;
            }
            const double u = (lo[a] - p[a]) / d[a], v = (hi[a] - p[a]) / d[a];
            t0 = std::max(t0, std::min(u, v));
            t1 = std::min(t1, std::max(u, v));
        }
        double tau = 0.0;
        for (double s = t0 + 0.5 * kStep; s < t1; s += kStep) {
            const double q[3] = {p[0] + d[0] * s, p[1] + d[1] * s, p[2] + d[2] * s};
            tau += sampleMedium(medium, k, top, q[0], q[1], q[2]) * kStep;
        }
        const double L = std::exp(-tau);
        out[0] += L * PG_SH_Y00;
        out[1] += L * PG_SH_Y1 * d[0];
        out[2] += L * PG_SH_Y1 * d[1];
        out[3] += L * PG_SH_Y1 * d[2];
    }
    for (int a = 0; a < 4; ++a) out[a] *= 4.0 * 3.14159265358979 / kDirs;
}

constexpr double kUp[3] = {0.0, 1.0, 0.0}, kDown[3] = {0.0, -1.0, 0.0};
constexpr double kSide[3] = {1.0, 0.0, 0.0};

/// Renders `frames` frames a tenth of a second apart (the refreshes age by
/// the scene's clock), then reads the grid back.
bool settle(scene::SceneGraph& graph, int frames, ProbeGridTables& t) {
    for (int f = 0; f < frames; ++f) {
        graph.tickAnimations(0.1f);
        graph.render();
    }
    return graph.renderer().readProbeGrid(t);
}

void checkCrownProbes(const ProbeGridTables& t) {
    // The medium's extinction, cascade 0's voxels.
    std::vector<double> medium(static_cast<size_t>(PG_VOXELS) * PG_VOXELS * PG_LAYERS_Y);
    for (size_t v = 0; v < medium.size(); ++v) medium[v] = t.medium[v * 4];
    const Cascade k = cascadeOf(t, 0);
    double worst = 0.0, sum = 0.0, shadeGot = 0.0, shadeWant = 0.0;
    int probes = 0;
    // Columns through the crown's middle, its edge and past it, every layer.
    for (int i : {16, 18, 20, 24}) {
        for (int layer = 0; layer < PG_LAYERS; ++layer) {
            const double p[3] = {k.x0 + (i + 0.5) * k.s, layerHeight(t, layer), k.z0 + (16 + 0.5) * k.s};
            double want[4];
            crownReference(t, medium, p, want);
            const Sh got = probeSh(t, 0, i, 16, layer);
            for (const double* n : {kUp, kDown, kSide}) {
                const double e = std::abs(shIrradiance(got.c[0], n) - shIrradiance(want, n));
                worst = std::max(worst, e);
                sum += e;
            }
            shadeGot += 1.0 - shIrradiance(got.c[0], kUp);
            shadeWant += 1.0 - shIrradiance(want, kUp);
            ++probes;
        }
    }
    const double mean = sum / (3.0 * probes);
    std::printf("  probes under the crown: %d, irradiance error mean %.4f, worst %.4f (of 1); the crown's shade "
                "%.3f vs %.3f\n",
                probes, mean, worst, shadeGot, shadeWant);
    check(mean < 0.01 && worst < 0.03, "a probe's irradiance up, down and across within 0.03 of the march");
    check(shadeWant > 1.0 && std::abs(shadeGot / shadeWant - 1.0) < 0.05, "the crown's shade within 5%");
    // The open sky over black ground: the light from above is the sky's.
    const Sh open = probeSh(t, 0, 4, 28, 3);
    std::printf("  an open probe: up %.4f, down %.4f\n", shIrradiance(open.c[0], kUp), shIrradiance(open.c[0], kDown));
    check(std::abs(shIrradiance(open.c[0], kUp) - 1.0) < 0.02 && std::abs(shIrradiance(open.c[0], kDown)) < 0.02,
          "in the open, the sky's irradiance above within 2%, the black ground's below");
}

void checkBounceProbes(const ProbeGridTables& t, const TraceScene& s) {
    // The ground: lit by the sun and by the sky (its probes' irradiance above,
    // the sky's), sending its albedo of that back.
    const double len = std::sqrt(kTraceSun[0] * kTraceSun[0] + kTraceSun[1] * kTraceSun[1] + kTraceSun[2] * kTraceSun[2]);
    const double down = s.albedo * (kTraceSunE * (-kTraceSun[1] / len) / 3.14159265358979 + s.ambient);
    double worstUp = 0.0, worstDown = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int layer = 0; layer < PG_LAYERS; ++layer)
            for (int i : {8, 24}) {
                const Sh got = probeSh(t, c, i, i, layer);
                worstUp = std::max(worstUp, std::abs(shIrradiance(got.c[0], kUp) / s.ambient - 1.0));
                worstDown = std::max(worstDown, std::abs(shIrradiance(got.c[0], kDown) / down - 1.0));
            }
    std::printf("  sunlit ground: worst up %.2f%%, down %.2f%% (down wants %.4f)\n", 100.0 * worstUp,
                100.0 * worstDown, down);
    check(worstUp < 0.02, "the sky's irradiance above within 2%");
    check(worstDown < 0.03, "the ground's light below (the sun's and the sky's, its albedo of them) within 3%");
}

}  // namespace

int main() {
    std::printf("=== bro_probe_grid_gpu_test: the probe grid's world against its scene ===\n");
    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    cfg.enableDynamicRendering = true;
    render::VulkanContext context(cfg);
    if (!context.init()) {
        std::fprintf(stderr, "Failed to initialize headless Vulkan context!\n");
        return 1;
    }
    scene::SceneRenderer::setDefaultVulkanContext(&context);
    {
        scene::SceneGraph graph;
        buildScene(graph, 0.0f, 0.0f);
        graph.render();
        graph.render();
        ProbeGridTables t;
        if (!graph.renderer().readProbeGrid(t)) {
            std::printf("FAILED: no probe grid read back\n");
            ++failures;
        } else {
            std::printf("the eye at the origin (top %.2f m)\n", t.block.params[2]);
            checkHeight(t);
            checkCanopy(t, static_cast<int>(t.block.params[1]));
            checkSun(t);
        }
        // Moved a few columns: the finest cascades' worlds made where they
        // now stand.
        const float eyeX = 5.3f, eyeZ = -3.1f;
        const float eyeY = static_cast<float>(plane(eyeX, eyeZ) + 1.7);
        graph.setCamera(1.0f, 256.0f / 144.0f, 0.1f, 2000.0f, {eyeX, eyeY, eyeZ}, {eyeX, eyeY, eyeZ - 10.0f});
        graph.render();
        ProbeGridTables moved;
        if (!graph.renderer().readProbeGrid(moved)) {
            std::printf("FAILED: no probe grid read back after the move\n");
            ++failures;
        } else {
            std::printf("the eye moved to (%.1f, %.1f)\n", eyeX, eyeZ);
            check(moved.block.cascade[0][0] + moved.block.eye[0] != t.block.cascade[0][0] + t.block.eye[0],
                  "the finest cascade moved");
            checkHeight(moved);
            checkCanopy(moved, 1);
            checkSun(moved);
        }
    }
    {
        // The probes in a black crown's shade under a flat sky.
        scene::SceneGraph graph;
        TraceScene s;
        s.crown = true;
        buildTraceScene(graph, s);
        ProbeGridTables t;
        if (!settle(graph, 30, t)) {
            std::printf("FAILED: no probe grid read back (the crown)\n");
            ++failures;
        } else {
            std::printf("the probes, a crown over black ground (top %.2f m)\n", t.block.params[2]);
            checkCrownProbes(t);
        }
    }
    {
        // The probes over sunlit ground that sends light back.
        scene::SceneGraph graph;
        TraceScene s;
        s.sun = true;
        s.ambient = 0.5f;
        s.albedo = 0.3f;
        buildTraceScene(graph, s);
        // (The ground's light comes in over refreshes, a half each: the
        // service grants them as the frame's budget allows, so the world
        // gets long enough for any budget to settle it.)
        ProbeGridTables t;
        if (!settle(graph, 80, t)) {
            std::printf("FAILED: no probe grid read back (the sunlit ground)\n");
            ++failures;
        } else {
            std::printf("the probes, sunlit ground\n");
            checkBounceProbes(t, s);
        }
    }
    scene::SceneRenderer::setDefaultVulkanContext(nullptr);
    if (const uint32_t errors = render::vulkanValidationErrorCount()) {
        std::fprintf(stderr, "FAILED: %u Vulkan validation error(s)\n", errors);
        return 1;
    }
    if (failures) {
        std::printf("=== bro_probe_grid_gpu_test: %d check(s) FAILED ===\n", failures);
        return 1;
    }
    std::printf("=== bro_probe_grid_gpu_test: all checks passed ===\n");
    return 0;
}
