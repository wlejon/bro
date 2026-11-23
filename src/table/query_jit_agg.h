#pragma once

#include "brass/codegen/kernel_jit.hpp"
#include "table/query_ir.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::table {

inline constexpr size_t kMaxJitAggs = 16;

/// C ABI struct passed to brass-compiled ungrouped aggregation chunk kernels.
struct JitAggContext {
    const void* const* leftColPtrs = nullptr;
    const uint64_t* aliveWords = nullptr;
    uint32_t rowCount = 0;
    uint32_t hasAliveMask = 0;
    uint32_t matchCount = 0;
    uint32_t pad = 0;
    double sumF64[kMaxJitAggs]{};
    int64_t sumI64[kMaxJitAggs]{};
    double minF64[kMaxJitAggs]{};
    double maxF64[kMaxJitAggs]{};
    int64_t minI64[kMaxJitAggs]{};
    int64_t maxI64[kMaxJitAggs]{};
};

using JitAggFnPtr = void (*)(JitAggContext*);

struct CompiledAggKernel {
    brass::codegen::KernelFunction func;
    JitAggFnPtr entry = nullptr;
    bool usesSimd = false;
    uint16_t simdWidth = 0;
    std::vector<uint32_t> residualFilterIndices;
    std::vector<uint32_t> usedColIndices;
};

/// Returns true when the query's filters can compile to a SIMD (AVX2 256-bit or 128-bit) filter loop.
bool canCompileSimdFilter(const QueryPlan& plan, const TableSnapshot& snap);

/// Returns true when an ungrouped numeric/bool aggregation query can JIT cleanly.
bool canCompileAggKernel(const QueryPlan& plan, const TableSnapshot& snap);

/// Compiles a 256-bit AVX2 / 128-bit SIMD chunk filter kernel matching `JitKernelFnPtr`.
std::shared_ptr<const struct CompiledKernel> compileSimdFilterKernel(
    const QueryPlan& plan, const TableSnapshot& snap, std::string* outError);

/// Compiles a fused filter + ungrouped aggregation kernel (with 256-bit AVX2 / 128-bit SIMD reduction).
std::shared_ptr<const CompiledAggKernel> compileAggKernel(
    const QueryPlan& plan, const TableSnapshot& snap, std::string* outError);

/// Returns true if any used column across snapshot chunks contains null cells.
bool aggSnapshotHasNulls(const CompiledAggKernel& kernel, const TableSnapshot& snap);

/// Executes a compiled ungrouped aggregation kernel across snapshot chunks in parallel.
std::shared_ptr<class Table> runCompiledAgg(
    const CompiledAggKernel& kernel, const QueryPlan& plan,
    const TableSnapshot& snap, bool useParallel);

}  // namespace bro::table
