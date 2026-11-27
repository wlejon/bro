#include "table/query_jit.h"

#include "brass/cache/artifact_cache.hpp"
#include "brass/cache/native_module_cache.hpp"
#include "brass/target/host_cpu.hpp"
#include "table/query_jit_emit.h"

#include <cstddef>
#include <exception>

namespace bro::table {

namespace {

using namespace jit_detail;

brass::cache::NativeModuleCache& table_native_cache() {
    static constexpr uint64_t kMaxBytes = 64ULL * 1024 * 1024;
    static auto* store = new brass::cache::ArtifactCache(
        brass::cache::default_cache_dir("bro", "table_kernels", "BRO_TABLE_CACHE_DIR"), kMaxBytes);
    static auto* cache = new brass::cache::NativeModuleCache(*store);
    return *cache;
}

brass::cache::CacheKey makeTableDiskKey(
    const QueryPlan& plan, const TableSnapshot& snap, std::string_view modeTag,
    const brass::codegen::KernelOptions& opts) {
    brass::cache::KeyBuilder kb("bro.table_kernel/1");
    kb.add_brass_codegen();
    kb.add("plan", buildPlanCacheKey(plan, snap, modeTag));
    kb.add_u64("opt_level", static_cast<uint64_t>(opts.opt_level));
    kb.add_bool("enable_optimizations", opts.enable_optimizations);
    kb.add_bool("enable_superopt", opts.enable_superopt);
    kb.add_bool("enable_smt_guard_elim", opts.enable_smt_guard_elim);
    kb.add_bool("enable_vectorize", opts.enable_vectorize);
    kb.add_bool("enable_slp", opts.enable_slp);
    kb.add_bool("enable_trace_layout", opts.enable_trace_layout);
    kb.add_bool("enable_pre_ra_schedule", opts.enable_pre_ra_schedule);
    kb.add_bool("enable_post_ra_schedule", opts.enable_post_ra_schedule);
    kb.add_bool("enable_software_pipelining", opts.enable_software_pipelining);
    kb.add_bool("enable_parallel", opts.enable_parallel);
    return kb.finish();
}

std::shared_ptr<CompiledKernel> compileKernelImpl(const QueryPlan& plan, const TableSnapshot& snap,
                                                  JitMode requestedMode, std::string* outError) {
    auto out = std::make_shared<CompiledKernel>();
    const size_t nCols = snap.schema.size();
    std::vector<bool> usedCols(nCols, false);

    for (uint32_t i = 0; i < plan.filters.size(); ++i) {
        if (canJitExpr(plan.exprs, plan.filters[i], snap)) {
            out->jittableFilterIndices.push_back(i);
            collectUsedCols(plan.exprs, plan.filters[i], usedCols);
        } else {
            out->residualFilterIndices.push_back(i);
        }
    }

    bool canDirectProject = false;
    if (requestedMode == JitMode::FilterAndProject &&
        out->residualFilterIndices.empty() && !plan.selectItems.empty()) {
        canDirectProject = true;
        for (const SelectItem& si : plan.selectItems) {
            if (si.outputType == ColType::String || !canJitExpr(plan.exprs, si.expr, snap)) {
                canDirectProject = false;
                break;
            }
        }
        if (canDirectProject) {
            for (const SelectItem& si : plan.selectItems) {
                collectUsedCols(plan.exprs, si.expr, usedCols);
            }
        }
    }
    out->allProjectsJittable = canDirectProject;

    if (requestedMode == JitMode::ScatterUpdate) {
        out->mode = JitMode::ScatterUpdate;
        for (const UpdateAssignment& up : plan.updates) {
            collectUsedCols(plan.exprs, up.expr, usedCols);
        }
    } else {
        out->mode = canDirectProject ? JitMode::FilterAndProject : JitMode::Filter;
    }
    out->usedColIndices = collectUsedColIndices(usedCols);

    Module mod("table_query");
    Builder b(mod);
    const char* entryName = (out->mode == JitMode::ScatterUpdate) ? "table_scatter_update"
                                                                  : "table_chunk_filter";
    Function* fn = mod.create_function(entryName, Type::void_type(), {Type::ptr()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    Value* ctxPtr = b.add_block_param(entry, Type::ptr());
    b.position_at_end(entry);

    Value* leftBase = b.build_load(Type::ptr(), ctxPtr,
                                   static_cast<int32_t>(offsetof(JitChunkContext, leftColPtrs)));
    std::vector<Value*> colPtrs(nCols, nullptr);
    for (size_t c = 0; c < nCols; ++c) {
        if (usedCols[c]) {
            colPtrs[c] = b.build_load(Type::ptr(), leftBase,
                                      static_cast<int32_t>(c * sizeof(void*)));
        }
    }

    std::vector<Value*> outPtrs;
    Value* outBase = nullptr;
    if (out->mode == JitMode::FilterAndProject) {
        outBase = b.build_load(Type::ptr(), ctxPtr,
                               static_cast<int32_t>(offsetof(JitChunkContext, outProjectPtrs)));
        outPtrs.resize(plan.selectItems.size(), nullptr);
        for (size_t k = 0; k < plan.selectItems.size(); ++k) {
            outPtrs[k] = b.build_load(Type::ptr(), outBase,
                                      static_cast<int32_t>(k * sizeof(void*)));
        }
    } else if (out->mode == JitMode::ScatterUpdate) {
        outBase = b.build_load(Type::ptr(), ctxPtr,
                               static_cast<int32_t>(offsetof(JitChunkContext, outProjectPtrs)));
        outPtrs.resize(plan.updates.size(), nullptr);
        for (size_t k = 0; k < plan.updates.size(); ++k) {
            outPtrs[k] = b.build_load(Type::ptr(), outBase,
                                      static_cast<int32_t>(k * sizeof(void*)));
        }
    }

    Value* matchIdxPtr = b.build_load(Type::ptr(), ctxPtr,
                                      static_cast<int32_t>(offsetof(JitChunkContext, matchIndices)));

    if (out->mode == JitMode::ScatterUpdate) {
        Value* matchCount = b.build_load(Type::i32(), ctxPtr,
                                         static_cast<int32_t>(offsetof(JitChunkContext, matchCount)));
        BasicBlock* loopHdr = createAttachedBlock(fn, b, "upd_hdr");
        Value* mIdx = b.add_block_param(loopHdr, Type::i32());
        BasicBlock* loopBody = createAttachedBlock(fn, b, "upd_body");
        BasicBlock* loopExit = createAttachedBlock(fn, b, "upd_exit");

        b.build_br(loopHdr, {b.build_iconst_i32(0)});

        b.position_at_end(loopHdr);
        b.build_br_if(b.build_slt(mIdx, matchCount), loopBody, {}, loopExit, {});

        b.position_at_end(loopBody);
        Value* m64 = b.build_zext_i64(mIdx);
        Value* row = b.build_load_indexed(Type::i32(), matchIdxPtr, m64, 4, 0);
        Value* row64 = b.build_zext_i64(row);

        ExprEmitter emitter(b, fn, plan, colPtrs, row64);
        std::vector<MirVal> evalVals;
        evalVals.reserve(plan.updates.size());
        for (const UpdateAssignment& up : plan.updates) {
            evalVals.push_back(emitter.emit(up.expr));
        }
        for (size_t k = 0; k < plan.updates.size(); ++k) {
            const ColType targetType = snap.schema.columns[static_cast<size_t>(plan.updates[k].colIdx)].type;
            storeTypedOutput(b, evalVals[k], targetType, outPtrs[k], row64);
        }
        Value* nextM = b.build_add(mIdx, b.build_iconst_i32(1));
        b.build_br(loopHdr, {nextM});

        b.position_at_end(loopExit);
        b.build_ret_void();
    } else {
        Value* aliveWords = b.build_load(Type::ptr(), ctxPtr,
                                         static_cast<int32_t>(offsetof(JitChunkContext, aliveWords)));
        Value* hasAliveMask = b.build_load(Type::i32(), ctxPtr,
                                           static_cast<int32_t>(offsetof(JitChunkContext, hasAliveMask)));
        Value* rowCount = b.build_load(Type::i32(), ctxPtr,
                                       static_cast<int32_t>(offsetof(JitChunkContext, rowCount)));

        BasicBlock* loopHdr = createAttachedBlock(fn, b, "loop_hdr");
        Value* row = b.add_block_param(loopHdr, Type::i32());
        Value* matchCount = b.add_block_param(loopHdr, Type::i32());

        BasicBlock* aliveChk = createAttachedBlock(fn, b, "alive_chk");
        BasicBlock* aliveLoad = createAttachedBlock(fn, b, "alive_load");
        std::vector<BasicBlock*> filterBlocks;
        for (size_t fi = 0; fi < out->jittableFilterIndices.size(); ++fi) {
            filterBlocks.push_back(createAttachedBlock(fn, b, "flt_" + std::to_string(fi)));
        }
        BasicBlock* onMatch = createAttachedBlock(fn, b, "on_match");
        BasicBlock* loopLatch = createAttachedBlock(fn, b, "loop_latch");
        Value* latchCount = b.add_block_param(loopLatch, Type::i32());
        BasicBlock* loopExit = createAttachedBlock(fn, b, "loop_exit");
        Value* finalCount = b.add_block_param(loopExit, Type::i32());

        BasicBlock* firstStep = filterBlocks.empty() ? onMatch : filterBlocks[0];
        b.build_br(loopHdr, {b.build_iconst_i32(0), b.build_iconst_i32(0)});

        b.position_at_end(loopHdr);
        b.build_br_if(b.build_slt(row, rowCount), aliveChk, {}, loopExit, {matchCount});

        b.position_at_end(aliveChk);
        Value* row64 = b.build_zext_i64(row);
        b.build_br_if(b.build_ne(hasAliveMask, b.build_iconst_i32(0)),
                      aliveLoad, {}, firstStep, {});

        b.position_at_end(aliveLoad);
        Value* wordIdx = b.build_lshr(row64, b.build_iconst_i64(6));
        Value* word = b.build_load_indexed(Type::i64(), aliveWords, wordIdx, 8, 0);
        Value* bit = b.build_and(row64, b.build_iconst_i64(63));
        Value* isAlive = b.build_trunc_i32(
            b.build_and(b.build_lshr(word, bit), b.build_iconst_i64(1)));
        b.build_br_if(isAlive, firstStep, {}, loopLatch, {matchCount});

        ExprEmitter emitter(b, fn, plan, colPtrs, row64);
        for (size_t fi = 0; fi < out->jittableFilterIndices.size(); ++fi) {
            b.position_at_end(filterBlocks[fi]);
            MirVal pred = emitter.emit(plan.filters[out->jittableFilterIndices[fi]]);
            Value* cond = buildTruthy(b, pred);
            BasicBlock* nextBb = (fi + 1 < filterBlocks.size()) ? filterBlocks[fi + 1] : onMatch;
            b.build_br_if(cond, nextBb, {}, loopLatch, {matchCount});
        }

        b.position_at_end(onMatch);
        Value* mc64 = b.build_zext_i64(matchCount);
        b.build_store_indexed(Type::i32(), matchIdxPtr, mc64, 4, 0, row);
        if (out->mode == JitMode::FilterAndProject) {
            for (size_t k = 0; k < plan.selectItems.size(); ++k) {
                MirVal pv = emitter.emit(plan.selectItems[k].expr);
                storeTypedOutput(b, pv, plan.selectItems[k].outputType, outPtrs[k], mc64);
            }
        }
        Value* nextMc = b.build_add(matchCount, b.build_iconst_i32(1));
        b.build_br(loopLatch, {nextMc});

        b.position_at_end(loopLatch);
        Value* nextRow = b.build_add(row, b.build_iconst_i32(1));
        b.build_br(loopHdr, {nextRow, latchCount});

        b.position_at_end(loopExit);
        b.build_store(Type::i32(), ctxPtr,
                      static_cast<int32_t>(offsetof(JitChunkContext, matchCount)), finalCount);
        b.build_ret_void();
    }

    fn->rebuild_cfg_predecessors();

    brass::codegen::KernelOptions opts;
    opts.enable_superopt = false;
    opts.enable_smt_guard_elim = false;
    opts.enable_vectorize = false;
    opts.enable_slp = false;
    opts.enable_parallel = false;

    const char* diskMode = (out->mode == JitMode::ScatterUpdate)
                               ? "U"
                               : (out->mode == JitMode::FilterAndProject ? "FP" : "F");
    try {
        const brass::cache::CacheKey diskKey = makeTableDiskKey(plan, snap, diskMode, opts);
        const brass::cache::NativeOptions nativeOpts;
        std::string cacheErr;
        auto nativeMod = table_native_cache().get_or_build_object(
            diskKey,
            [&](brass::object::ObjectFile& obj, std::string&) -> bool {
                brass::codegen::KernelJit jit(opts);
                obj = jit.compile_object(mod);
                return true;
            },
            nativeOpts,
            &cacheErr);
        if (nativeMod) {
            if (void* sym = nativeMod->symbol(entryName)) {
                out->func = brass::codegen::KernelFunction::from_owner(
                    entryName, sym, 0, std::move(nativeMod));
                out->entry = reinterpret_cast<JitKernelFnPtr>(sym);
                return out;
            }
        }
        brass::codegen::KernelJit jit(opts);
        out->func = jit.compile(mod, entryName);
        out->entry = reinterpret_cast<JitKernelFnPtr>(out->func.entry_point());
    } catch (const std::exception& ex) {
        if (outError) *outError = ex.what();
        return nullptr;
    }
    if (!out->entry) {
        if (outError) *outError = "brass JIT returned null entry pointer";
        return nullptr;
    }
    return out;
}

}  // namespace

CompiledPlanCache& CompiledPlanCache::instance() {
    static CompiledPlanCache cache;
    return cache;
}

std::shared_ptr<const CompiledKernel> CompiledPlanCache::getOrCompile(
    const QueryPlan& plan, const TableSnapshot& snap, JitMode requestedMode,
    std::string* outError) {
    const bool useAvx2 = brass::host_has_avx2();
    const bool trySimdFilter =
        (requestedMode == JitMode::Filter) && canCompileSimdFilter(plan, snap);
    const char* simdTag = useAvx2 ? "SF256" : "SF128";
    const char* modeTag = (requestedMode == JitMode::ScatterUpdate)
                              ? "U"
                              : (requestedMode == JitMode::FilterAndProject
                                     ? "FP"
                                     : (trySimdFilter ? simdTag : "F"));
    const std::string key = buildPlanCacheKey(plan, snap, modeTag);

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    misses_.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<const CompiledKernel> compiled;
    if (trySimdFilter) {
        compiled = compileSimdFilterKernel(plan, snap, outError);
    }
    if (!compiled) {
        compiled = compileKernelImpl(plan, snap, requestedMode, outError);
    }
    if (!compiled) return nullptr;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto [it, inserted] = entries_.emplace(key, compiled);
    if (inserted) {
        fifoOrder_.push_back(key);
        while (fifoOrder_.size() > kMaxEntries) {
            entries_.erase(fifoOrder_.front());
            fifoOrder_.pop_front();
        }
    }
    return it->second;
}

std::shared_ptr<const CompiledAggKernel> CompiledPlanCache::getOrCompileAgg(
    const QueryPlan& plan, const TableSnapshot& snap, std::string* outError) {
    if (!canCompileAggKernel(plan, snap)) return nullptr;
    const char* aggTag = brass::host_has_avx2() ? "A256" : "A128";
    const std::string key = buildPlanCacheKey(plan, snap, aggTag);

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = aggEntries_.find(key);
        if (it != aggEntries_.end()) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    misses_.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<const CompiledAggKernel> compiled = compileAggKernel(plan, snap, outError);
    if (!compiled) return nullptr;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto [it, inserted] = aggEntries_.emplace(key, compiled);
    if (inserted) {
        aggFifoOrder_.push_back(key);
        while (aggFifoOrder_.size() > kMaxEntries) {
            aggEntries_.erase(aggFifoOrder_.front());
            aggFifoOrder_.pop_front();
        }
    }
    return it->second;
}

JitCacheStats CompiledPlanCache::stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return JitCacheStats{
        hits_.load(std::memory_order_relaxed),
        misses_.load(std::memory_order_relaxed),
        entries_.size() + aggEntries_.size(),
    };
}

void CompiledPlanCache::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_.clear();
    aggEntries_.clear();
    fifoOrder_.clear();
    aggFifoOrder_.clear();
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
}

}  // namespace bro::table
