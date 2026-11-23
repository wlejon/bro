#pragma once

#include "brass/codegen/kernel_jit.hpp"
#include "table/query_ir.h"
#include "table/query_jit_agg.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace bro::table {

/// C ABI struct passed by pointer to brass-compiled chunk kernels.
struct JitChunkContext {
    const void* const* leftColPtrs = nullptr;
    void* const* outProjectPtrs = nullptr;
    const uint64_t* aliveWords = nullptr;
    uint32_t rowCount = 0;
    uint32_t hasAliveMask = 0;
    uint32_t* matchIndices = nullptr;  // out in Filter/FilterProject; in for ScatterUpdate
    uint32_t matchCount = 0;
};

using JitKernelFnPtr = void (*)(JitChunkContext*);

enum class JitMode : uint8_t {
    Filter = 0,
    FilterAndProject,
    ScatterUpdate,
};

struct CompiledKernel {
    JitMode mode = JitMode::Filter;
    brass::codegen::KernelFunction func;
    JitKernelFnPtr entry = nullptr;
    bool usesSimd = false;
    uint16_t simdWidth = 0;
    std::vector<uint32_t> jittableFilterIndices;  // indices into plan.filters
    std::vector<uint32_t> residualFilterIndices;  // non-jittable filters (e.g. String ops)
    std::vector<uint32_t> usedColIndices;
    bool allProjectsJittable = false;
};

struct JitCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    size_t entries = 0;
};

/// Thread-safe cache of brass-compiled chunk kernels keyed by schema + AST shape.
class CompiledPlanCache {
public:
    static CompiledPlanCache& instance();

    std::shared_ptr<const CompiledKernel> getOrCompile(
        const QueryPlan& plan, const TableSnapshot& snap, JitMode requestedMode,
        std::string* outError = nullptr);

    std::shared_ptr<const CompiledAggKernel> getOrCompileAgg(
        const QueryPlan& plan, const TableSnapshot& snap,
        std::string* outError = nullptr);

    JitCacheStats stats() const;
    void clear();

private:
    static constexpr size_t kMaxEntries = 512;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const CompiledKernel>> entries_;
    std::unordered_map<std::string, std::shared_ptr<const CompiledAggKernel>> aggEntries_;
    std::deque<std::string> fifoOrder_;
    std::deque<std::string> aggFifoOrder_;
    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
};

}  // namespace bro::table
