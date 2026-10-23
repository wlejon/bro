#pragma once

#include <bromesh/isosurface/jit/sdf_node.h>
#include <bromath/vec.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bro::scene {

/// Ray specification for SDF sphere-tracing.
struct SdfRay {
    bromath::Vec3 origin{0.0f, 0.0f, 0.0f};
    float tMin = 0.0f;
    bromath::Vec3 dir{0.0f, 0.0f, -1.0f};
    float tMax = 100.0f;
};

/// Configuration for SDF sphere-tracing, cone-traced soft shadows, and ambient occlusion.
struct SdfTraceConfig {
    int maxSteps = 128;
    float hitEpsilon = 1e-4f;
    float normalEpsilon = 1e-3f;
    bromath::Vec3 lightDir{0.57735027f, 0.57735027f, 0.57735027f};
    float shadowK = 16.0f;
    float shadowMinT = 0.02f;
    float shadowMaxT = 20.0f;
    int shadowSteps = 64;
    int aoSteps = 5;
    float aoStepSize = 0.08f;
    float aoIntensity = 1.5f;
    bool computeShadow = true;
    bool computeAo = true;
};

/// Result of tracing a single ray against an SDF graph.
struct SdfTraceHit {
    bool hit = false;
    float t = 0.0f;
    float distance = 0.0f;
    bromath::Vec3 position{0.0f, 0.0f, 0.0f};
    bromath::Vec3 normal{0.0f, 1.0f, 0.0f};
    float softShadow = 1.0f;
    float ambientOcclusion = 1.0f;
    int steps = 0;
};

/// Summary of formal SPIR-V module verification and compute-shader static safety audit.
struct SdfSpirvAuditSummary {
    bool ok = false;
    bool moduleVerified = false;
    bool parseOk = false;
    bool structuralOk = false;
    bool dominanceOk = false;
    bool barriersUniform = false;
    bool sharedBoundsOk = false;
    bool divSqrtOk = false;
    uint32_t wordCount = 0;
    uint32_t numFunctions = 0;
    uint32_t numBlocks = 0;
    uint32_t numInstructions = 0;
    uint32_t hazardCount = 0;
    uint32_t divisionCount = 0;
    uint32_t provenSafeDivisions = 0;
    std::vector<std::string> diagnostics;
};

/// Live GPU SDF sphere-tracer, cone-traced soft-shadow & ambient-occlusion evaluator,
/// and SPIR-V 1.5 compute shader compiler + formal verifier.
class SdfGpuTracer {
public:
    /// Compile an SdfGraph into a Vulkan SPIR-V 1.5 grid evaluation compute shader
    /// (`evaluate_sdf_grid`) via `bromesh::JitSdfCompiler::compile_spirv` and verify it.
    static std::vector<uint32_t> compileGridSpirv(
        const bromesh::SdfGraph& graph, int rootNode = -1);

    /// Compile an SdfGraph into a specialized Vulkan SPIR-V 1.5 sphere-tracing compute
    /// shader (`trace_sdf_rays`) that evaluates ray hits, finite-difference SDF normals,
    /// cone-traced soft shadows (`min(k * d / t)`), and normal-hemisphere ambient occlusion.
    static std::vector<uint32_t> compileRaymarchSpirv(
        const bromesh::SdfGraph& graph,
        const SdfTraceConfig& cfg = {},
        int rootNode = -1);

    /// Formally verify a SPIR-V 1.5 binary word stream using both `brass::spirv::verify`
    /// and `brass::spirv::audit_compute_shader_binary`.
    static SdfSpirvAuditSummary auditSpirvBinary(std::span<const uint32_t> words);

    /// Evaluate the signed distance at world-space point `p` using JIT-compiled native
    /// code (with scalar fallback).
    static float evalPoint(
        const bromesh::SdfGraph& graph, bromath::Vec3 p, int rootNode = -1);

    /// Compute the normalized SDF surface gradient normal at `p` via tetrahedral finite
    /// differences (`eps` spacing).
    static bromath::Vec3 evalNormal(
        const bromesh::SdfGraph& graph,
        bromath::Vec3 p,
        float eps = 1e-3f,
        int rootNode = -1);

    /// Evaluate cone-traced soft shadow factor in `[0, 1]` from `origin` toward `lightDir`
    /// using `min(k * d / t)` along the shadow ray.
    static float evalSoftShadow(
        const bromesh::SdfGraph& graph,
        bromath::Vec3 origin,
        bromath::Vec3 lightDir,
        float k = 16.0f,
        float tMin = 0.02f,
        float tMax = 20.0f,
        int maxSteps = 64,
        int rootNode = -1);

    /// Evaluate multi-step normal-hemisphere ambient occlusion in `[0, 1]` at surface
    /// point `pos` with surface normal `normal`.
    static float evalAmbientOcclusion(
        const bromesh::SdfGraph& graph,
        bromath::Vec3 pos,
        bromath::Vec3 normal,
        int steps = 5,
        float stepSize = 0.08f,
        float intensity = 1.5f,
        int rootNode = -1);

    /// Sphere-trace a single ray against `graph`, computing hit position, analytic normal,
    /// cone-traced soft shadow, and ambient occlusion.
    static SdfTraceHit traceRay(
        const bromesh::SdfGraph& graph,
        const SdfRay& ray,
        const SdfTraceConfig& cfg = {},
        int rootNode = -1);

    /// Sphere-trace a batch of rays against `graph` in parallel without any CPU Marching
    /// Cubes triangle rebuild.
    static void traceRays(
        const bromesh::SdfGraph& graph,
        std::span<const SdfRay> rays,
        std::vector<SdfTraceHit>& outHits,
        const SdfTraceConfig& cfg = {},
        int rootNode = -1,
        bool parallel = true);

    /// Mutate the float parameters (`f0..f3`) of an existing node in `graph` in-place
    /// for interactive CSG / blend weight / transform editing without rebuilding topology.
    static bool updateNodeParams(
        bromesh::SdfGraph& graph,
        int nodeIndex,
        float f0,
        float f1 = 0.0f,
        float f2 = 0.0f,
        float f3 = 0.0f);
};

} // namespace bro::scene
