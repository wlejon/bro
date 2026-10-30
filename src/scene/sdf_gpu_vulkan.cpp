#include "scene/sdf_gpu_tracer.h"

#include <bromesh/isosurface/jit/sdf_compiler.h>

#if defined(BROMESH_HAS_BRASS_JIT)
#include <brass/gpu/vulkan_driver.hpp>
#include <brass/target/spirv_target.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace bro::scene {

namespace {

thread_local bool tl_lastTraceUsedVulkan = false;

bool forceCpuEnv() {
    const char* v = std::getenv("BRO_SDF_FORCE_CPU");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
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

#if defined(BROMESH_HAS_BRASS_JIT)

std::mutex& vkCacheMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::unordered_map<uint64_t, std::shared_ptr<brass::gpu::VulkanModule>>& vkModuleCache() {
    static auto* c = new std::unordered_map<uint64_t, std::shared_ptr<brass::gpu::VulkanModule>>();
    return *c;
}

std::shared_ptr<brass::gpu::VulkanModule> getOrCreateGridModule(
    const bromesh::SdfGraph& graph, int rootNode) {
    const int resolvedRoot = (rootNode >= 0) ? rootNode : graph.root();
    uint64_t key = hashCombine64(graph.computeHash(), static_cast<uint64_t>(resolvedRoot));
    key = hashCombine64(key, 0x475249445f535056ULL); // "GRID_SPV"

    {
        std::lock_guard<std::mutex> lock(vkCacheMutex());
        auto& cache = vkModuleCache();
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    std::vector<uint32_t> words = SdfGpuTracer::compileGridSpirv(graph, resolvedRoot);
    if (words.empty()) return nullptr;

    brass::target::SpirvKernel k;
    k.entry = "eval_sdf_gpu";
    k.stage = brass::target::SpirvStage::Compute;
    k.words = std::move(words);
    k.params = {
        {brass::Type::ptr(), 0, 8},
        {brass::Type::i32(), 8, 4},
        {brass::Type::i32(), 12, 4},
        {brass::Type::i32(), 16, 4},
        {brass::Type::f32(), 20, 4},
        {brass::Type::f32(), 24, 4},
        {brass::Type::f32(), 28, 4},
        {brass::Type::f32(), 32, 4},
        {brass::Type::f32(), 36, 4},
        {brass::Type::f32(), 40, 4},
    };
    k.push_constant_bytes = 44;
    k.local_size[0] = 256;
    k.local_size[1] = 1;
    k.local_size[2] = 1;
    k.local_size_spec_constants = false;

    std::string err;
    auto mod = std::make_shared<brass::gpu::VulkanModule>(
        brass::gpu::VulkanModule::load(k, &err));
    if (!mod->valid()) return nullptr;

    std::lock_guard<std::mutex> lock(vkCacheMutex());
    vkModuleCache()[key] = mod;
    return mod;
}

std::shared_ptr<brass::gpu::VulkanModule> getOrCreateRaymarchModule(
    const bromesh::SdfGraph& graph,
    const SdfTraceConfig& cfg,
    int rootNode) {
    const int resolvedRoot = (rootNode >= 0) ? rootNode : graph.root();
    uint64_t key = hashCombine64(graph.computeHash(), static_cast<uint64_t>(resolvedRoot));
    key = hashCombine64(key, hashTraceConfig(cfg));

    {
        std::lock_guard<std::mutex> lock(vkCacheMutex());
        auto& cache = vkModuleCache();
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    std::vector<uint32_t> words = SdfGpuTracer::compileRaymarchSpirv(graph, cfg, resolvedRoot);
    if (words.empty()) return nullptr;

    brass::target::SpirvKernel k;
    k.entry = "trace_sdf_rays";
    k.stage = brass::target::SpirvStage::Compute;
    k.words = std::move(words);
    k.params = {
        {brass::Type::ptr(), 0, 8},
        {brass::Type::ptr(), 8, 8},
        {brass::Type::i32(), 16, 4},
    };
    k.push_constant_bytes = 20;
    k.local_size[0] = 64;
    k.local_size[1] = 1;
    k.local_size[2] = 1;
    k.local_size_spec_constants = false;

    std::string err;
    auto mod = std::make_shared<brass::gpu::VulkanModule>(
        brass::gpu::VulkanModule::load(k, &err));
    if (!mod->valid()) return nullptr;

    std::lock_guard<std::mutex> lock(vkCacheMutex());
    vkModuleCache()[key] = mod;
    return mod;
}

#endif // BROMESH_HAS_BRASS_JIT

} // namespace

bool SdfGpuTracer::vulkanAvailable() {
    if (forceCpuEnv()) return false;
#if defined(BROMESH_HAS_BRASS_JIT)
    return brass::gpu::vulkan_available();
#else
    return false;
#endif
}

std::string SdfGpuTracer::vulkanDeviceName() {
#if defined(BROMESH_HAS_BRASS_JIT)
    if (const auto* caps = brass::gpu::vulkan_device_caps()) {
        return caps->info.name;
    }
#endif
    return {};
}

bool SdfGpuTracer::lastTraceUsedVulkanGpu() {
    return tl_lastTraceUsedVulkan;
}

uint32_t SdfGpuTracer::cachedVulkanPipelineCount() {
#if defined(BROMESH_HAS_BRASS_JIT)
    std::lock_guard<std::mutex> lock(vkCacheMutex());
    return static_cast<uint32_t>(vkModuleCache().size());
#else
    return 0;
#endif
}

void SdfGpuTracer::closeVulkan() {
#if defined(BROMESH_HAS_BRASS_JIT)
    {
        std::lock_guard<std::mutex> lock(vkCacheMutex());
        vkModuleCache().clear();
    }
    brass::gpu::vulkan_close();
#endif
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

#if defined(BROMESH_HAS_BRASS_JIT)
    if (vulkanAvailable()) {
        auto mod = getOrCreateGridModule(graph, rootNode);
        if (mod && mod->valid()) {
            const float stepX = dimX > 1 ? (bounds.max.x - bounds.min.x) / static_cast<float>(dimX - 1) : 1.0f;
            const float stepY = dimY > 1 ? (bounds.max.y - bounds.min.y) / static_cast<float>(dimY - 1) : 1.0f;
            const float stepZ = dimZ > 1 ? (bounds.max.z - bounds.min.z) / static_cast<float>(dimZ - 1) : 1.0f;

            const size_t byteSize = total * sizeof(float);
            auto outBuf = brass::gpu::VulkanBuffer::alloc(byteSize);
            if (outBuf.valid()) {
                const uint32_t numGroups = static_cast<uint32_t>((total + 255) / 256);
                std::string err;
                const bool launched = mod->launch_1d(
                    numGroups,
                    256,
                    {
                        outBuf,
                        static_cast<int32_t>(dimX),
                        static_cast<int32_t>(dimY),
                        static_cast<int32_t>(dimZ),
                        bounds.min.x,
                        bounds.min.y,
                        bounds.min.z,
                        stepX,
                        stepY,
                        stepZ,
                    },
                    &err);
                if (launched && outBuf.download(outField.data(), byteSize)) {
                    tl_lastTraceUsedVulkan = true;
                    if (usedVulkanOut) *usedVulkanOut = true;
                    return true;
                }
            }
        }
    }
#endif

    // Automatic CPU fallback via JitSdfCompiler::evaluateVolume
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

#if defined(BROMESH_HAS_BRASS_JIT)
    if (vulkanAvailable()) {
        auto mod = getOrCreateRaymarchModule(graph, cfg, rootNode);
        if (mod && mod->valid()) {
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
            auto inBuf = brass::gpu::VulkanBuffer::alloc(byteSize);
            auto outBuf = brass::gpu::VulkanBuffer::alloc(byteSize);
            if (inBuf.valid() && outBuf.valid() && inBuf.upload(packedRays.data(), byteSize)) {
                const uint32_t numGroups = static_cast<uint32_t>((n + 63) / 64);
                std::string err;
                const bool launched = mod->launch_1d(
                    numGroups,
                    64,
                    {
                        inBuf,
                        outBuf,
                        static_cast<int32_t>(n),
                    },
                    &err);
                std::vector<float> packedHits(n * 8, 0.0f);
                if (launched && outBuf.download(packedHits.data(), byteSize)) {
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
    }
#endif

    traceRaysCpu(graph, rays, outHits, cfg, rootNode, true);
    return true;
}

} // namespace bro::scene
