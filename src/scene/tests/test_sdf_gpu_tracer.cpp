#include "scene/sdf_gpu_tracer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bro::scene;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void testSpirvCompilationAndFormalAudit() {
    bromesh::SdfGraph g;
    const int s0 = g.sphere(1.2f);
    const int b0 = g.box({0.8f, 0.8f, 0.8f});
    const int tb = g.translate(b0, {0.6f, 0.2f, -0.3f});
    const int su = g.opSmoothUnion(s0, tb, 0.35f);
    const int cyl = g.cylinder(0.35f, 1.5f);
    const int sub = g.opSmoothSubtraction(su, cyl, 0.1f);
    const int tor = g.torus(1.4f, 0.2f);
    const int rot = g.rotateY(tor, 0.5f);
    const int u0 = g.opUnion(sub, rot);
    const int noise = g.noise3D({1.0f, 1.0f, 1.0f}, 0.02f);
    const int root = g.displace(u0, noise);
    g.setRoot(root);

    // 1. Grid SPIR-V shader (eval_sdf_gpu)
    const auto gridWords = SdfGpuTracer::compileGridSpirv(g, root);
    check(gridWords.size() > 32, "grid SPIR-V binary is non-empty");
    if (!gridWords.empty()) {
        check(gridWords[0] == 0x07230203u, "grid SPIR-V magic number is 0x07230203");
        check(gridWords[1] == 0x00010500u, "grid SPIR-V version is 1.5 (0x00010500)");
    }

    const auto gridAudit = SdfGpuTracer::auditSpirvBinary(gridWords);
    if (!gridAudit.ok) {
        for (const auto& d : gridAudit.diagnostics) {
            std::fprintf(stderr, "grid audit diagnostic: %s\n", d.c_str());
        }
    }
    check(gridAudit.parseOk, "grid SPIR-V binary parses cleanly");
    check(gridAudit.structuralOk, "grid SPIR-V passes structural CFG verification");
    check(gridAudit.dominanceOk, "grid SPIR-V passes SSA dominance verification");
    check(gridAudit.barriersUniform, "grid SPIR-V passes barrier uniformity verification");
    check(gridAudit.sharedBoundsOk, "grid SPIR-V passes shared-memory bounds verification");
    check(gridAudit.divSqrtOk, "grid SPIR-V passes division/sqrt safety verification");
    check(gridAudit.hazardCount == 0, "grid SPIR-V has zero static hazards");
    check(gridAudit.provenSafeDivisions == gridAudit.divisionCount,
          "grid SPIR-V proves every division/sqrt safe");
    check(gridAudit.ok, "grid SPIR-V full audit report is ok()");

    // 2. Raymarch + soft shadow + AO SPIR-V shader (trace_sdf_rays)
    SdfTraceConfig cfg;
    cfg.maxSteps = 96;
    cfg.shadowSteps = 48;
    cfg.aoSteps = 5;
    const auto rayWords = SdfGpuTracer::compileRaymarchSpirv(g, cfg, root);
    check(rayWords.size() > 64, "raymarch SPIR-V binary is non-empty");
    if (!rayWords.empty()) {
        check(rayWords[0] == 0x07230203u, "raymarch SPIR-V magic number is 0x07230203");
        check(rayWords[1] == 0x00010500u, "raymarch SPIR-V version is 1.5 (0x00010500)");
    }

    const auto rayAudit = SdfGpuTracer::auditSpirvBinary(rayWords);
    if (!rayAudit.ok) {
        for (const auto& d : rayAudit.diagnostics) {
            std::fprintf(stderr, "raymarch audit diagnostic: %s\n", d.c_str());
        }
    }
    check(rayAudit.parseOk, "raymarch SPIR-V binary parses cleanly");
    check(rayAudit.structuralOk, "raymarch SPIR-V passes structural CFG verification");
    check(rayAudit.dominanceOk, "raymarch SPIR-V passes SSA dominance verification");
    check(rayAudit.barriersUniform, "raymarch SPIR-V passes barrier uniformity verification");
    check(rayAudit.sharedBoundsOk, "raymarch SPIR-V passes shared-memory bounds verification");
    check(rayAudit.divSqrtOk, "raymarch SPIR-V passes division/sqrt safety verification");
    check(rayAudit.hazardCount == 0, "raymarch SPIR-V has zero static hazards");
    check(rayAudit.divisionCount > 0 && rayAudit.provenSafeDivisions == rayAudit.divisionCount,
          "raymarch SPIR-V proves all divisions and square roots safe");
    check(rayAudit.ok, "raymarch SPIR-V full audit report is ok()");
}

void testSphereTracingAccuracy() {
    // Analytic sphere of radius 1.5 at the origin
    bromesh::SdfGraph sphereGraph;
    sphereGraph.sphere(1.5f);

    SdfRay centerRay;
    centerRay.origin = {0.0f, 0.0f, 5.0f};
    centerRay.dir = {0.0f, 0.0f, -1.0f};
    centerRay.tMin = 0.0f;
    centerRay.tMax = 20.0f;

    SdfTraceConfig cfg;
    cfg.maxSteps = 128;
    cfg.hitEpsilon = 1e-4f;

    const SdfTraceHit hit = SdfGpuTracer::traceRay(sphereGraph, centerRay, cfg);
    check(hit.hit, "sphere-trace hits sphere of radius 1.5 from z=5");
    check(std::fabs(hit.t - 3.5f) < 1e-3f, "sphere-trace hit distance t matches analytic 3.5");
    check(std::fabs(hit.position.z - 1.5f) < 1e-3f, "sphere-trace hit position z matches 1.5");
    check(std::fabs(hit.normal.z - 1.0f) < 1e-3f &&
          std::fabs(hit.normal.x) < 1e-3f &&
          std::fabs(hit.normal.y) < 1e-3f,
          "sphere-trace normal at (0,0,1.5) is (0,0,1)");

    // Off-axis ray hitting sphere at x = 0.9, y = 0.0 -> z = sqrt(1.5^2 - 0.9^2) = 1.2
    SdfRay offAxisRay;
    offAxisRay.origin = {0.9f, 0.0f, 4.0f};
    offAxisRay.dir = {0.0f, 0.0f, -1.0f};
    offAxisRay.tMax = 20.0f;
    const SdfTraceHit offHit = SdfGpuTracer::traceRay(sphereGraph, offAxisRay, cfg);
    check(offHit.hit, "off-axis ray hits sphere at x=0.9");
    check(std::fabs(offHit.t - (4.0f - 1.2f)) < 1.5e-3f,
          "off-axis sphere hit t matches analytic 2.8");
    check(std::fabs(offHit.normal.x - (0.9f / 1.5f)) < 2e-3f &&
          std::fabs(offHit.normal.z - (1.2f / 1.5f)) < 2e-3f,
          "off-axis sphere normal matches analytic (0.6, 0, 0.8)");

    // Miss ray passing outside sphere radius
    SdfRay missRay;
    missRay.origin = {2.5f, 0.0f, 5.0f};
    missRay.dir = {0.0f, 0.0f, -1.0f};
    missRay.tMax = 20.0f;
    const SdfTraceHit missHit = SdfGpuTracer::traceRay(sphereGraph, missRay, cfg);
    check(!missHit.hit, "ray passing outside sphere misses cleanly");

    // Batch parallel traceRays (64 rays) matches single-ray traceRay
    std::vector<SdfRay> batchRays(64);
    for (int i = 0; i < 64; ++i) {
        const float u = (static_cast<float>(i % 8) - 3.5f) * 0.25f;
        const float v = (static_cast<float>(i / 8) - 3.5f) * 0.25f;
        batchRays[static_cast<size_t>(i)].origin = {u, v, 5.0f};
        batchRays[static_cast<size_t>(i)].dir = {0.0f, 0.0f, -1.0f};
        batchRays[static_cast<size_t>(i)].tMax = 20.0f;
    }
    std::vector<SdfTraceHit> batchHits;
    SdfGpuTracer::traceRays(sphereGraph, batchRays, batchHits, cfg, -1, true);
    check(batchHits.size() == batchRays.size(), "parallel traceRays produces 64 hits");

    bool allMatch = true;
    for (size_t i = 0; i < batchRays.size(); ++i) {
        const SdfTraceHit single = SdfGpuTracer::traceRay(sphereGraph, batchRays[i], cfg);
        if (single.hit != batchHits[i].hit ||
            std::fabs(single.t - batchHits[i].t) > 1e-5f) {
            allMatch = false;
            break;
        }
    }
    check(allMatch, "parallel traceRays matches single-ray traceRay exactly");
}

void testConeTracedSoftShadowsAndAmbientOcclusion() {
    // Scene: floor plane at y = -1.0 and occluder sphere of radius 0.8 at (0, 0.5, 0)
    bromesh::SdfGraph g;
    const int floorNode = g.plane({0.0f, 1.0f, 0.0f}, 1.0f); // y + 1 = 0 -> y = -1
    const int sph = g.sphere(0.8f);
    const int occluder = g.translate(sph, {0.0f, 0.5f, 0.0f});
    g.opUnion(floorNode, occluder);

    const bromath::Vec3 upLight{0.0f, 1.0f, 0.0f};

    // 1. Directly beneath the sphere at (0, -0.98, 0): full umbra
    const float umbra = SdfGpuTracer::evalSoftShadow(
        g, {0.0f, -0.98f, 0.0f}, upLight, 16.0f, 0.02f, 10.0f, 64);
    check(umbra < 0.02f, "cone-traced soft shadow in umbra is near 0");

    // 2. Grazing the sphere edge at (0.85, -0.98, 0): partial penumbra (0 < s < 1)
    const float penumbra = SdfGpuTracer::evalSoftShadow(
        g, {0.85f, -0.98f, 0.0f}, upLight, 8.0f, 0.02f, 10.0f, 64);
    check(penumbra > 0.05f && penumbra < 0.95f,
          "cone-traced soft shadow in penumbra is smoothly between 0 and 1");

    // 3. Sharper k gives darker/tighter shadow at the same grazing point inside penumbra
    const float softPenumbra = SdfGpuTracer::evalSoftShadow(
        g, {0.95f, -0.98f, 0.0f}, upLight, 4.0f, 0.02f, 10.0f, 64);
    const float sharpPenumbra = SdfGpuTracer::evalSoftShadow(
        g, {0.95f, -0.98f, 0.0f}, upLight, 24.0f, 0.02f, 10.0f, 64);
    check(sharpPenumbra > softPenumbra,
          "higher shadow k produces tighter penumbra than lower shadow k");

    // 4. Far unoccluded point at (4.0, -0.98, 0): fully lit
    const float unoccluded = SdfGpuTracer::evalSoftShadow(
        g, {4.0f, -0.98f, 0.0f}, upLight, 16.0f, 0.02f, 10.0f, 64);
    check(unoccluded > 0.98f, "cone-traced soft shadow far from occluder is near 1");

    // 5. Ambient Occlusion: concave 90-degree corner between floor (y=-1) and wall (x=0)
    bromesh::SdfGraph cornerGraph;
    const int fl = cornerGraph.plane({0.0f, 1.0f, 0.0f}, 1.0f); // y = -1
    const int wall = cornerGraph.box({0.2f, 2.0f, 2.0f});       // wall around x=0
    cornerGraph.opUnion(fl, wall);

    // Point right in the concave crease (x = 0.21, y = -1.0, z = 0.0) with upward normal
    const float creaseAo = SdfGpuTracer::evalAmbientOcclusion(
        cornerGraph, {0.21f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 5, 0.08f, 2.0f);
    // Point on open floor far from the wall (x = 3.0, y = -1.0, z = 0.0)
    const float openAo = SdfGpuTracer::evalAmbientOcclusion(
        cornerGraph, {3.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 5, 0.08f, 2.0f);

    check(openAo > 0.95f, "ambient occlusion on open flat floor is near 1.0");
    check(creaseAo < 0.70f && creaseAo < openAo,
          "ambient occlusion in concave CSG crease is significantly darker than open floor");
}

void testLiveCsgParameterUpdateWithoutMeshing() {
    // Interactive CSG scene: SmoothUnion of a sphere at origin and a translated sphere
    bromesh::SdfGraph g;
    const int sA = g.sphere(1.0f);
    const int sB = g.sphere(1.0f);
    const int transB = g.translate(sB, {2.5f, 0.0f, 0.0f});
    const int blendRoot = g.opSmoothUnion(sA, transB, 0.05f);

    // Ray aimed at the midpoint (1.25, 0, 4) -> (0, 0, -1)
    SdfRay midRay;
    midRay.origin = {1.25f, 0.0f, 4.0f};
    midRay.dir = {0.0f, 0.0f, -1.0f};
    midRay.tMax = 15.0f;

    SdfTraceConfig cfg;
    cfg.maxSteps = 128;
    cfg.hitEpsilon = 1e-4f;

    // Initially, spheres of radius 1.0 at x=0 and x=2.5 have a gap around x=1.25
    const SdfTraceHit beforeHit = SdfGpuTracer::traceRay(g, midRay, cfg, blendRoot);
    check(!beforeHit.hit, "midpoint ray initially misses separated spheres (gap at x=1.25)");

    // Mutate Translate node in-place to move second sphere to x=1.4 and widen SmoothUnion k=0.6
    // without any Marching Cubes triangle extraction!
    const bool updatedTrans = SdfGpuTracer::updateNodeParams(g, transB, 1.4f, 0.0f, 0.0f, 0.0f);
    const bool updatedBlend = SdfGpuTracer::updateNodeParams(g, blendRoot, 0.6f, 0.0f, 0.0f, 0.0f);
    check(updatedTrans && updatedBlend, "updateNodeParams mutates CSG graph nodes in-place");

    const SdfTraceHit afterHit = SdfGpuTracer::traceRay(g, midRay, cfg, blendRoot);
    check(afterHit.hit, "midpoint ray immediately hits smooth-blended CSG bridge after live update");
    check(afterHit.t > 2.5f && afterHit.t < 3.6f,
          "live-updated CSG bridge hit distance is physically accurate");

    // Recompile raymarch SPIR-V after parameter mutation and verify zero hazards
    const auto updatedSpirv = SdfGpuTracer::compileRaymarchSpirv(g, cfg, blendRoot);
    const auto updatedAudit = SdfGpuTracer::auditSpirvBinary(updatedSpirv);
    check(updatedAudit.ok && updatedAudit.hazardCount == 0,
          "recompiled SPIR-V after live CSG parameter update passes formal audit");
}

} // namespace

int main() {
    testSpirvCompilationAndFormalAudit();
    testSphereTracingAccuracy();
    testConeTracedSoftShadowsAndAmbientOcclusion();
    testLiveCsgParameterUpdateWithoutMeshing();

    std::printf("bro_sdf_gpu_tracer_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
