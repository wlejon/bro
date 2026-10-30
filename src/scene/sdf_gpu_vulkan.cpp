#include "scene/sdf_gpu_tracer.h"

#include "render/vulkan_compute.h"
#include "render/vulkan_context.h"
#include "scene/scene_renderer.h"

#include <bromesh/isosurface/jit/sdf_compiler.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace bro::scene {

namespace {

thread_local bool tl_lastTraceUsedVulkan = false;

bool forceCpuEnv() {
    static const bool force = [] {
        const char* v = std::getenv("BRO_SDF_FORCE_CPU");
        return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
    }();
    return force;
}

inline bromath::Vec3 normalizeRayDir(bromath::Vec3 v) {
    const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (lenSq <= 1e-20f) return {0.0f, 1.0f, 0.0f};
    const float invLen = 1.0f / std::sqrt(lenSq);
    return {v.x * invLen, v.y * invLen, v.z * invLen};
}

uint64_t hashCombine64(uint64_t seed, uint64_t val) {
    return seed ^ (val + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

uint64_t hashFloatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(uint32_t));
    return static_cast<uint64_t>(bits);
}

uint64_t hashTraceConfig(const SdfTraceConfig& cfg) {
    uint64_t h = 0x517cc1b727220a95ULL;
    h = hashCombine64(h, static_cast<uint64_t>(cfg.maxSteps));
    h = hashCombine64(h, hashFloatBits(cfg.hitEpsilon));
    h = hashCombine64(h, hashFloatBits(cfg.normalEpsilon));
    h = hashCombine64(h, hashFloatBits(cfg.lightDir.x));
    h = hashCombine64(h, hashFloatBits(cfg.lightDir.y));
    h = hashCombine64(h, hashFloatBits(cfg.lightDir.z));
    h = hashCombine64(h, hashFloatBits(cfg.shadowK));
    h = hashCombine64(h, hashFloatBits(cfg.shadowMinT));
    h = hashCombine64(h, hashFloatBits(cfg.shadowMaxT));
    h = hashCombine64(h, static_cast<uint64_t>(cfg.shadowSteps));
    h = hashCombine64(h, static_cast<uint64_t>(cfg.aoSteps));
    h = hashCombine64(h, hashFloatBits(cfg.aoStepSize));
    h = hashCombine64(h, hashFloatBits(cfg.aoIntensity));
    h = hashCombine64(h, cfg.computeShadow ? 1ULL : 0ULL);
    h = hashCombine64(h, cfg.computeAo ? 1ULL : 0ULL);
    return h;
}

// The engine's compute, when its device can run these kernels.
render::VulkanCompute* engineCompute() {
    if (forceCpuEnv()) return nullptr;
    render::VulkanContext* ctx = SceneRenderer::defaultVulkanContext();
    if (!ctx || !ctx->isValid() || !ctx->compute().available()) return nullptr;
    return &ctx->compute();
}

// Compiled pipelines, for the compute they were made on (a different one
// drops them all). Main thread, as VulkanCompute is.
struct PipelineCache {
    render::VulkanCompute* compute = nullptr;
    std::unordered_map<uint64_t, std::shared_ptr<render::ComputePipeline>> pipelines;
};

PipelineCache& pipelineCache() {
    static auto* c = new PipelineCache();
    return *c;
}

template <typename Compile>
std::shared_ptr<render::ComputePipeline> cachedPipeline(
    render::VulkanCompute& compute, uint64_t key, Compile&& compile,
    const char* entry, uint32_t pushBytes, uint32_t localSizeX) {
    PipelineCache& cache = pipelineCache();
    if (cache.compute != &compute) {
        cache.pipelines.clear();
        cache.compute = &compute;
    }
    if (auto it = cache.pipelines.find(key); it != cache.pipelines.end()) return it->second;

    const std::vector<uint32_t> words = compile();
    if (words.empty()) return nullptr;
    render::ComputePipelineDesc desc;
    desc.spirv = words;
    desc.entry = entry;
    desc.pushConstantBytes = pushBytes;
    desc.localSize[0] = localSizeX;
    auto pipeline = compute.createPipeline(desc);
    if (pipeline) cache.pipelines[key] = pipeline;
    return pipeline;
}

// The push-constant block of a kernel, field by field at its offset.
template <size_t N>
struct PushBlock {
    uint8_t bytes[N] = {};
    template <typename T>
    void put(size_t offset, T v) { std::memcpy(bytes + offset, &v, sizeof(T)); }
};

} // namespace

bool SdfGpuTracer::vulkanAvailable() {
    return engineCompute() != nullptr;
}

std::string SdfGpuTracer::vulkanDeviceName() {
    if (!engineCompute()) return {};
    return SceneRenderer::defaultVulkanContext()->deviceProperties().deviceName;
}

bool SdfGpuTracer::lastTraceUsedVulkanGpu() {
    return tl_lastTraceUsedVulkan;
}

uint32_t SdfGpuTracer::cachedVulkanPipelineCount() {
    return static_cast<uint32_t>(pipelineCache().pipelines.size());
}

void SdfGpuTracer::releasePipelines() {
    PipelineCache& cache = pipelineCache();
    cache.pipelines.clear();
    cache.compute = nullptr;
}

bool SdfGpuTracer::evaluateGridGpu(
    const bromesh::SdfGraph& graph,
    int dimX,
    int dimY,
    int dimZ,
    const bromath::AABB3& bounds,
    std::vector<float>& outField,
    int rootNode,
    bool* usedVulkanOut) {
    tl_lastTraceUsedVulkan = false;
    if (usedVulkanOut) *usedVulkanOut = false;
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0 || graph.empty()) {
        outField.clear();
        return false;
    }

    const size_t total = static_cast<size_t>(dimX) * static_cast<size_t>(dimY) * static_cast<size_t>(dimZ);
    outField.resize(total, 0.0f);

    if (render::VulkanCompute* compute = engineCompute()) {
        const int resolvedRoot = (rootNode >= 0) ? rootNode : graph.root();
        uint64_t key = hashCombine64(graph.computeHash(), static_cast<uint64_t>(resolvedRoot));
        key = hashCombine64(key, 0x475249445f535056ULL); // "GRID_SPV"
        // eval_sdf_gpu(out: ptr, dimX, dimY, dimZ: i32, min: 3 x f32, step: 3 x f32)
        auto pipeline = cachedPipeline(*compute, key,
            [&] { return compileGridSpirv(graph, resolvedRoot); }, "eval_sdf_gpu", 44, 256);
        const size_t byteSize = total * sizeof(float);
        auto outBuf = pipeline ? compute->createBuffer(byteSize) : nullptr;
        if (outBuf) {
            const float stepX = dimX > 1 ? (bounds.max.x - bounds.min.x) / static_cast<float>(dimX - 1) : 1.0f;
            const float stepY = dimY > 1 ? (bounds.max.y - bounds.min.y) / static_cast<float>(dimY - 1) : 1.0f;
            const float stepZ = dimZ > 1 ? (bounds.max.z - bounds.min.z) / static_cast<float>(dimZ - 1) : 1.0f;
            PushBlock<44> push;
            push.put<uint64_t>(0, outBuf->address());
            push.put<int32_t>(8, dimX);
            push.put<int32_t>(12, dimY);
            push.put<int32_t>(16, dimZ);
            push.put<float>(20, bounds.min.x);
            push.put<float>(24, bounds.min.y);
            push.put<float>(28, bounds.min.z);
            push.put<float>(32, stepX);
            push.put<float>(36, stepY);
            push.put<float>(40, stepZ);
            const uint32_t groups[3] = {static_cast<uint32_t>((total + 255) / 256), 1, 1};
            if (compute->dispatch(*pipeline, push.bytes, groups) &&
                compute->read(*outBuf, 0, outField.data(), byteSize)) {
                tl_lastTraceUsedVulkan = true;
                if (usedVulkanOut) *usedVulkanOut = true;
                return true;
            }
        }
    }

    // On the CPU: JitSdfCompiler::evaluateVolume.
    auto vol = bromesh::JitSdfCompiler::instance().evaluateVolume(
        graph, dimX, dimY, dimZ, bounds, rootNode, true);
    outField = std::move(vol.field);
    return !outField.empty();
}

bool SdfGpuTracer::traceRaysGpu(
    const bromesh::SdfGraph& graph,
    std::span<const SdfRay> rays,
    std::vector<SdfTraceHit>& outHits,
    const SdfTraceConfig& cfg,
    int rootNode,
    bool* usedVulkanOut) {
    tl_lastTraceUsedVulkan = false;
    if (usedVulkanOut) *usedVulkanOut = false;
    outHits.assign(rays.size(), SdfTraceHit{});
    if (graph.empty() || rays.empty()) return false;

    if (render::VulkanCompute* compute = engineCompute()) {
        const int resolvedRoot = (rootNode >= 0) ? rootNode : graph.root();
        uint64_t key = hashCombine64(graph.computeHash(), static_cast<uint64_t>(resolvedRoot));
        key = hashCombine64(key, hashTraceConfig(cfg));
        // trace_sdf_rays(rays: ptr, hits: ptr, n: i32)
        auto pipeline = cachedPipeline(*compute, key,
            [&] { return compileRaymarchSpirv(graph, cfg, resolvedRoot); }, "trace_sdf_rays", 20, 64);

        const size_t n = rays.size();
        std::vector<float> packedRays(n * 8, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const bromath::Vec3 dir = normalizeRayDir(rays[i].dir);
            const float tMin = std::max(rays[i].tMin, 0.0f);
            const float tMax = std::max(rays[i].tMax, tMin);
            float* dst = packedRays.data() + i * 8;
            dst[0] = rays[i].origin.x;
            dst[1] = rays[i].origin.y;
            dst[2] = rays[i].origin.z;
            dst[3] = tMin;
            dst[4] = dir.x;
            dst[5] = dir.y;
            dst[6] = dir.z;
            dst[7] = tMax;
        }

        const size_t byteSize = n * 8 * sizeof(float);
        auto inBuf = pipeline ? compute->createBuffer(byteSize) : nullptr;
        auto outBuf = pipeline ? compute->createBuffer(byteSize) : nullptr;
        std::vector<float> packedHits(n * 8, 0.0f);
        if (inBuf && outBuf && compute->write(*inBuf, 0, packedRays.data(), byteSize)) {
            PushBlock<20> push;
            push.put<uint64_t>(0, inBuf->address());
            push.put<uint64_t>(8, outBuf->address());
            push.put<int32_t>(16, static_cast<int32_t>(n));
            const uint32_t groups[3] = {static_cast<uint32_t>((n + 63) / 64), 1, 1};
            if (compute->dispatch(*pipeline, push.bytes, groups) &&
                compute->read(*outBuf, 0, packedHits.data(), byteSize)) {
                const auto compiled = bromesh::JitSdfCompiler::instance().compile(graph, rootNode);
                for (size_t i = 0; i < n; ++i) {
                    const float* src = packedHits.data() + i * 8;
                    const float* rSrc = packedRays.data() + i * 8;
                    SdfTraceHit& h = outHits[i];
                    h.t = src[0];
                    h.normal = {src[1], src[2], src[3]};
                    h.softShadow = src[4];
                    h.ambientOcclusion = src[5];
                    h.hit = (src[6] > 0.5f);
                    h.steps = static_cast<int>(std::lround(src[7]));
                    h.position = {
                        rSrc[0] + rSrc[4] * h.t,
                        rSrc[1] + rSrc[5] * h.t,
                        rSrc[2] + rSrc[6] * h.t,
                    };
                    if (compiled.pointFn) {
                        h.distance = compiled.pointFn(h.position.x, h.position.y, h.position.z);
                    } else {
                        h.distance = evalPoint(graph, h.position, rootNode);
                    }
                }
                tl_lastTraceUsedVulkan = true;
                if (usedVulkanOut) *usedVulkanOut = true;
                return true;
            }
        }
    }

    traceRaysCpu(graph, rays, outHits, cfg, rootNode, true);
    return true;
}

} // namespace bro::scene
