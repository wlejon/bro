#include "scene/sdf_gpu_tracer.h"
#include "render/glsl_compiler.h"
#include "codegen-brass/js_gpu_kernel.h"

#include <brass/gpu/vulkan_driver.hpp>
#include <bromesh/isosurface/jit/sdf_compiler.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

void testVulkanGpuComputeExecutionAndParity() {
    // Build a CSG scene with floor + smooth-union sphere/box
    bromesh::SdfGraph g;
    const int floorNode = g.plane({0.0f, 1.0f, 0.0f}, 1.0f);
    const int sph = g.sphere(1.0f);
    const int bx = g.box({0.7f, 0.7f, 0.7f});
    const int tbx = g.translate(bx, {0.5f, 0.2f, 0.0f});
    const int csgObj = g.opSmoothUnion(sph, tbx, 0.25f);
    const int root = g.opUnion(floorNode, csgObj);
    g.setRoot(root);

    const bool vkAvail = SdfGpuTracer::vulkanAvailable();
    check(vkAvail, "Vulkan compute queue with PhysicalStorageBufferAddresses is available");
    if (vkAvail) {
        check(!SdfGpuTracer::vulkanDeviceName().empty(), "Vulkan compute device name is non-empty");
    }

    // 1. 3D Grid evaluation on Vulkan GPU vs CPU reference
    const bromath::AABB3 bounds{{-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}};
    std::vector<float> gpuField;
    bool gridUsedVk = false;
    const bool gridOk = SdfGpuTracer::evaluateGridGpu(g, 16, 16, 16, bounds, gpuField, root, &gridUsedVk);
    check(gridOk && gpuField.size() == 16 * 16 * 16, "evaluateGridGpu produces 16x16x16 SDF grid");
    if (vkAvail) {
        check(gridUsedVk && SdfGpuTracer::lastTraceUsedVulkanGpu(),
              "evaluateGridGpu executed on hardware Vulkan compute queue");
    }

    const auto cpuVol = bromesh::JitSdfCompiler::instance().evaluateVolume(g, 16, 16, 16, bounds, root, false);
    float maxGridDiff = 0.0f;
    for (size_t i = 0; i < gpuField.size() && i < cpuVol.field.size(); ++i) {
        maxGridDiff = std::max(maxGridDiff, std::fabs(gpuField[i] - cpuVol.field[i]));
    }
    check(maxGridDiff < 1e-4f, "Vulkan GPU grid evaluation matches CPU reference within 1e-4");

    // 2. Raymarching + soft shadows + AO on Vulkan GPU vs CPU reference
    std::vector<SdfRay> rays(36);
    for (int i = 0; i < 36; ++i) {
        const float u = (static_cast<float>(i % 6) - 2.5f) * 0.4f;
        const float v = (static_cast<float>(i / 6) - 2.5f) * 0.4f;
        rays[static_cast<size_t>(i)].origin = {u, v + 0.5f, 4.5f};
        rays[static_cast<size_t>(i)].dir = {0.0f, -0.15f, -1.0f};
        rays[static_cast<size_t>(i)].tMax = 20.0f;
    }

    SdfTraceConfig cfg;
    cfg.maxSteps = 128;
    cfg.shadowSteps = 48;
    cfg.aoSteps = 5;

    std::vector<SdfTraceHit> gpuHits;
    bool raysUsedVk = false;
    const bool raysOk = SdfGpuTracer::traceRaysGpu(g, rays, gpuHits, cfg, root, &raysUsedVk);
    check(raysOk && gpuHits.size() == rays.size(), "traceRaysGpu produces 36 hit records");
    if (vkAvail) {
        check(raysUsedVk && SdfGpuTracer::lastTraceUsedVulkanGpu(),
              "traceRaysGpu executed on hardware Vulkan compute queue");
        check(SdfGpuTracer::cachedVulkanPipelineCount() >= 2,
              "VulkanModule pipeline cache holds compiled grid + raymarch pipelines");
    }

    std::vector<SdfTraceHit> cpuHits;
    SdfGpuTracer::traceRaysCpu(g, rays, cpuHits, cfg, root, false);

    bool parityOk = true;
    for (size_t i = 0; i < rays.size(); ++i) {
        if (gpuHits[i].hit != cpuHits[i].hit) {
            parityOk = false;
            break;
        }
        if (gpuHits[i].hit) {
            if (std::fabs(gpuHits[i].t - cpuHits[i].t) > 2e-3f ||
                std::fabs(gpuHits[i].normal.x - cpuHits[i].normal.x) > 5e-3f ||
                std::fabs(gpuHits[i].normal.y - cpuHits[i].normal.y) > 5e-3f ||
                std::fabs(gpuHits[i].normal.z - cpuHits[i].normal.z) > 5e-3f ||
                std::fabs(gpuHits[i].softShadow - cpuHits[i].softShadow) > 1e-2f ||
                std::fabs(gpuHits[i].ambientOcclusion - cpuHits[i].ambientOcclusion) > 1e-2f) {
                parityOk = false;
                break;
            }
        }
    }
    check(parityOk, "Vulkan GPU traceRaysGpu hit distances, normals, soft shadows & AO match CPU reference");
}

void testGlslCompilerRelaxedPrecisionAndComputeAudit() {
    // 1. Fragment shader with bounded math (sin, cos, clamp, mix) + unbounded world position
    const char* fragSrc = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() {
    float wave = sin(vUv.x * 6.2831853) * 0.5 + 0.5;
    float rim = clamp(cos(vUv.y * 3.1415926), 0.0, 1.0);
    float tonemapped = mix(wave, rim, 0.35);
    float worldDist = vWorldPos.x * 4096.0;
    outColor = vec4(tonemapped, wave * rim, clamp(worldDist, 0.0, 1.0), 1.0);
}
)";

    bro::render::GlslCompileOptions fragOpts;
    fragOpts.promoteRelaxedPrecision = true;
    fragOpts.maxAbsFp16 = 1024.0f;
    fragOpts.assumeStageInputsUnitRange = true;

    const auto fragRes = bro::render::GlslCompiler::compileAndOptimize(
        fragSrc, bro::render::ShaderStage::Fragment, fragOpts);
    check(fragRes.ok(), "GlslCompiler::compileAndOptimize compiles fragment shader");
    check(fragRes.relaxedPrecision.enabled && fragRes.relaxedPrecision.parseOk,
          "GlslCompiler RelaxedPrecision pass parses fragment SPIR-V cleanly");
    check(fragRes.relaxedPrecision.promotedCount > 0,
          "GlslCompiler RelaxedPrecision promotes bounded fragment float ops to FP16");

    // 2. Compute shader audited by brass::spirv::audit_compute_shader_binary
    const char* compSrc = R"(#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 0) buffer OutBuf {
    float data[];
} outBuf;
shared float tile[64];
void main() {
    uint lid = gl_LocalInvocationID.x & 63u;
    float s = sin(float(lid) * 0.1) * 0.5 + 0.5;
    tile[lid] = s;
    barrier();
    uint gid = gl_GlobalInvocationID.x;
    outBuf.data[gid] = tile[lid] / (max(s, 0.01) + 0.001);
}
)";

    bro::render::GlslCompileOptions compOpts;
    compOpts.promoteRelaxedPrecision = true;
    compOpts.auditComputeShader = true;

    const auto compRes = bro::render::GlslCompiler::compileAndOptimize(
        compSrc, bro::render::ShaderStage::Compute, compOpts);
    check(compRes.ok(), "GlslCompiler::compileAndOptimize compiles compute shader");
    check(compRes.relaxedPrecision.promotedCount > 0,
          "GlslCompiler RelaxedPrecision promotes bounded compute float ops");
    check(compRes.computeAudit.audited && compRes.computeAudit.valid,
          "GlslCompiler static compute audit validates compute SPIR-V");
    check(compRes.computeAudit.barriersUniform && compRes.computeAudit.sharedBoundsOk &&
          compRes.computeAudit.divSqrtOk && compRes.computeAudit.provenSafe,
          "GlslCompiler static compute audit proves uniform barrier, shared bounds & safe division");
    check(compRes.computeAudit.localSize[0] == 64,
          "GlslCompiler static compute audit extracts workgroup local_size_x = 64");

    // 3. Environment variable toggles BRO_SHADER_RELAXED_PRECISION=1 and BRO_AUDIT_COMPUTE_SHADERS=1
    setenv("BRO_SHADER_RELAXED_PRECISION", "1", 1);
    setenv("BRO_AUDIT_COMPUTE_SHADERS", "1", 1);
    std::string log;
    const auto envSpirv = bro::render::compileGlslToSpirv(
        compSrc, bro::render::ShaderStage::Compute, &log);
    unsetenv("BRO_SHADER_RELAXED_PRECISION");
    unsetenv("BRO_AUDIT_COMPUTE_SHADERS");

    check(!envSpirv.empty(), "compileGlslToSpirv succeeds with BRO_SHADER_RELAXED_PRECISION=1 and BRO_AUDIT_COMPUTE_SHADERS=1");
    check(bro::render::GlslCompiler::lastRelaxedPrecisionStats().promotedCount > 0,
          "BRO_SHADER_RELAXED_PRECISION=1 records promoted RelaxedPrecision stats");
    check(bro::render::GlslCompiler::lastComputeAuditStats().provenSafe,
          "BRO_AUDIT_COMPUTE_SHADERS=1 records proven-safe compute audit stats");
}

void testJsGpuKernelSingleSourceAndVulkanDispatch() {
    // 1. Single-source 2D scalar height kernel (#116)
    const char* heightJs = R"(
        // @kernel
        function terrainWave(x, z) {
            return Math.sin(x * 0.25) * 2.0 + Math.cos(z * 0.25) * 1.5;
        }
    )";
    bronze::GpuCompileOptions opts;
    opts.stage = brass::target::SpirvStage::Compute;
    opts.enableRelaxedPrecision = true;
    opts.localSizeX = 64;

    const auto res = bronze::compileJsGpuKernelSource(heightJs, "terrainWave.js", opts);
    check(res.ok, "bronze::compileJsGpuKernelSource compiles single-source terrainWave JS function");
    check(res.hasScalarReturn && res.scalarParamCount == 2,
          "terrainWave has 2 scalar parameters and scalar return");
    check(res.cpuGpuProvenEquivalent, "terrainWave CPU MIR == SPIR-V equivalence formally proven");
    check(res.singleSourceParityVerified && res.maxCpuGpuUlpDiff <= 2.0f,
          "terrainWave single-source 64-point parity verified within 2 ULPs");
    check(res.relaxedPrecisionReport.promoted_count > 0,
          "terrainWave promotes bounded float ops to RelaxedPrecision");
    check(!res.spirvWords.empty() && !res.ptxAssembly.empty(),
          "terrainWave emits both SPIR-V 1.5 words and PTX assembly");

    const float cpuVal = res.evalCpu2D(1.5f, -0.75f);
    const float spvVal = res.evalSpirv2D(1.5f, -0.75f);
    const float refVal = std::sin(1.5f * 0.25f) * 2.0f + std::cos(-0.75f * 0.25f) * 1.5f;
    check(std::fabs(cpuVal - refVal) < 1e-5f && std::fabs(spvVal - cpuVal) < 1e-5f,
          "terrainWave evalCpu2D and evalSpirv2D match reference value");

    if (brass::gpu::vulkan_available()) {
        brass::target::SpirvKernel k;
        k.entry = "terrainWave";
        k.stage = brass::target::SpirvStage::Compute;
        k.words = res.spirvWords;
        k.params = {
            {brass::Type::f32(), 0, 4},
            {brass::Type::f32(), 4, 4},
            {brass::Type::ptr(), 8, 8},
        };
        k.push_constant_bytes = 16;
        k.local_size[0] = 64;
        k.local_size[1] = 1;
        k.local_size[2] = 1;

        std::string err;
        auto mod = brass::gpu::VulkanModule::load(k, &err);
        auto outBuf = brass::gpu::VulkanBuffer::alloc(sizeof(float));
        float vkVal = 0.0f;
        const bool launched = mod.valid() && outBuf.valid() &&
                              mod.launch_1d(1, 64, {1.5f, -0.75f, outBuf}, &err) &&
                              outBuf.download(&vkVal, sizeof(float));
        check(launched && std::fabs(vkVal - cpuVal) < 1e-5f,
              "single-source JS @kernel terrainWave executes on hardware Vulkan queue and matches CPU");
    }

    // 2. Buffer compute kernel dispatched on Vulkan (#116)
    const char* saxpyJs = R"(
        // @kernel
        function saxpy(out, a, scale, n) {
            let i = global_tid_x();
            if (i < n) {
                out[i] = a[i] * scale + 2.5;
            }
        }
    )";
    const auto saxpyRes = bronze::compileJsGpuKernelSource(saxpyJs, "saxpy.js", opts);
    check(saxpyRes.ok && !saxpyRes.spirvWords.empty(),
          "bronze::compileJsGpuKernelSource compiles buffer compute kernel saxpy");

    if (brass::gpu::vulkan_available()) {
        brass::target::SpirvKernel k;
        k.entry = "saxpy";
        k.stage = brass::target::SpirvStage::Compute;
        k.words = saxpyRes.spirvWords;
        k.params = {
            {brass::Type::ptr(), 0, 8},
            {brass::Type::ptr(), 8, 8},
            {brass::Type::f32(), 16, 4},
            {brass::Type::i32(), 20, 4},
        };
        k.push_constant_bytes = 24;
        k.local_size[0] = 64;
        k.local_size[1] = 1;
        k.local_size[2] = 1;

        constexpr int N = 64;
        std::vector<float> hostA(N), hostOut(N, 0.0f);
        for (int i = 0; i < N; ++i) hostA[static_cast<size_t>(i)] = static_cast<float>(i) * 0.5f;

        auto bufA = brass::gpu::VulkanBuffer::alloc(N * sizeof(float));
        auto bufOut = brass::gpu::VulkanBuffer::alloc(N * sizeof(float));
        std::string err;
        auto mod = brass::gpu::VulkanModule::load(k, &err);
        const bool ok = mod.valid() && bufA.valid() && bufOut.valid() &&
                        bufA.upload(hostA.data(), N * sizeof(float)) &&
                        mod.launch_1d(1, 64, {bufOut, bufA, 4.0f, static_cast<int32_t>(N)}, &err) &&
                        bufOut.download(hostOut.data(), N * sizeof(float));
        bool match = ok;
        for (int i = 0; i < N && match; ++i) {
            const float expected = (static_cast<float>(i) * 0.5f) * 4.0f + 2.5f;
            if (std::fabs(hostOut[static_cast<size_t>(i)] - expected) > 1e-4f) match = false;
        }
        check(match, "JS buffer @kernel saxpy dispatches 64 elements on Vulkan queue and matches expected output");
    }
}

} // namespace

int main() {
    testSpirvCompilationAndFormalAudit();
    testSphereTracingAccuracy();
    testConeTracedSoftShadowsAndAmbientOcclusion();
    testLiveCsgParameterUpdateWithoutMeshing();
    testVulkanGpuComputeExecutionAndParity();
    testGlslCompilerRelaxedPrecisionAndComputeAudit();
    testJsGpuKernelSingleSourceAndVulkanDispatch();

    std::printf("bro_sdf_gpu_tracer_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
