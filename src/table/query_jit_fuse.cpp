#include "table/query_jit.h"

#include "brass/mir/cross_module.hpp"
#include "table/query_jit_emit.h"

#include <cstddef>
#include <exception>

namespace bro::table {

namespace {

using namespace jit_detail;

Value* emitColValueForCallee(Builder& b, ColType colType, Value* colPtr, Value* row64, Type targetType) {
    MirVal loaded{};
    if (colType == ColType::Bool) {
        Value* raw = b.build_load_indexed(Type::i8(), colPtr, row64, 1, 0);
        Value* z = b.build_trunc_i32(b.build_zext_i64(raw));
        loaded = MirVal{b.build_ne(z, b.build_iconst_i32(0)), ColType::Bool};
    } else {
        loaded = MirVal{b.build_load_indexed(mirStorageType(colType), colPtr, row64, colScale(colType), 0), colType};
    }

    if (targetType == Type::f64()) return castMirVal(b, loaded, ColType::F64).val;
    if (targetType == Type::f32()) return castMirVal(b, loaded, ColType::F32).val;
    if (targetType == Type::i64()) return castMirVal(b, loaded, ColType::I64).val;
    if (targetType == Type::i32()) return castMirVal(b, loaded, ColType::I32).val;
    return loaded.val;
}

Value* callResultToTruthyI32(Builder& b, Value* resVal) {
    if (!resVal) return b.build_iconst_i32(0);
    const Type t = resVal->type();
    if (t == Type::i32()) return b.build_ne(resVal, b.build_iconst_i32(0));
    if (t == Type::i64()) return b.build_ne(resVal, b.build_iconst_i64(0));
    if (t == Type::f32()) {
        Value* ord = b.build_eq(resVal, resVal);
        Value* nz = b.build_ne(resVal, b.build_fconst_f32(0.0f));
        return b.build_and(ord, nz);
    }
    if (t == Type::f64()) {
        Value* ord = b.build_eq(resVal, resVal);
        Value* nz = b.build_ne(resVal, b.build_fconst_f64(0.0));
        return b.build_and(ord, nz);
    }
    return b.build_iconst_i32(0);
}

}  // namespace

std::shared_ptr<const CompiledKernel> compileFusedUdfFilterKernel(
    const TableSnapshot& snap,
    const FusedUdfSpec& udf,
    std::string* outError) {
    auto fail = [&](std::string msg) -> std::shared_ptr<const CompiledKernel> {
        if (outError) *outError = std::move(msg);
        return nullptr;
    };

    if (!udf.sourceModule || udf.calleeName.empty()) {
        return fail("compileFusedUdfFilterKernel: missing source module or callee name");
    }
    const brass::Function* callee = udf.sourceModule->get_function(udf.calleeName);
    if (!callee) {
        return fail("compileFusedUdfFilterKernel: callee '" + udf.calleeName + "' not found");
    }
    if (callee->param_count() != udf.argColIndices.size()) {
        return fail("compileFusedUdfFilterKernel: parameter count mismatch (" +
                    std::to_string(callee->param_count()) + " vs " +
                    std::to_string(udf.argColIndices.size()) + ")");
    }
    for (uint32_t ci : udf.argColIndices) {
        if (ci >= snap.schema.columns.size()) {
            return fail("compileFusedUdfFilterKernel: column index out of range");
        }
        if (snap.schema.columns[ci].type == ColType::String) {
            return fail("compileFusedUdfFilterKernel: String column cannot be passed to numeric UDF");
        }
    }

    auto out = std::make_shared<CompiledKernel>();
    out->mode = JitMode::Filter;
    out->usedColIndices = udf.argColIndices;

    Module mod("table_fused_udf");
    Builder b(mod);
    const char* entryName = "table_fused_udf_filter";
    Function* fn = mod.create_function(entryName, Type::void_type(), {Type::ptr()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    Value* ctxPtr = b.add_block_param(entry, Type::ptr());
    b.position_at_end(entry);

    Value* leftBase = b.build_load(Type::ptr(), ctxPtr,
                                   static_cast<int32_t>(offsetof(JitChunkContext, leftColPtrs)));
    std::vector<Value*> argColPtrs(udf.argColIndices.size(), nullptr);
    for (size_t k = 0; k < udf.argColIndices.size(); ++k) {
        argColPtrs[k] = b.build_load(Type::ptr(), leftBase,
                                     static_cast<int32_t>(udf.argColIndices[k] * sizeof(void*)));
    }
    Value* matchIdxPtr = b.build_load(Type::ptr(), ctxPtr,
                                      static_cast<int32_t>(offsetof(JitChunkContext, matchIndices)));
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
    BasicBlock* udfBlock = createAttachedBlock(fn, b, "udf_flt");
    BasicBlock* onMatch = createAttachedBlock(fn, b, "on_match");
    BasicBlock* loopLatch = createAttachedBlock(fn, b, "loop_latch");
    Value* latchCount = b.add_block_param(loopLatch, Type::i32());
    BasicBlock* loopExit = createAttachedBlock(fn, b, "loop_exit");
    Value* finalCount = b.add_block_param(loopExit, Type::i32());

    b.build_br(loopHdr, {b.build_iconst_i32(0), b.build_iconst_i32(0)});

    b.position_at_end(loopHdr);
    b.build_br_if(b.build_slt(row, rowCount), aliveChk, {}, loopExit, {matchCount});

    b.position_at_end(aliveChk);
    Value* row64 = b.build_zext_i64(row);
    b.build_br_if(b.build_ne(hasAliveMask, b.build_iconst_i32(0)),
                  aliveLoad, {}, udfBlock, {});

    b.position_at_end(aliveLoad);
    Value* wordIdx = b.build_lshr(row64, b.build_iconst_i64(6));
    Value* word = b.build_load_indexed(Type::i64(), aliveWords, wordIdx, 8, 0);
    Value* bit = b.build_and(row64, b.build_iconst_i64(63));
    Value* isAlive = b.build_trunc_i32(
        b.build_and(b.build_lshr(word, bit), b.build_iconst_i64(1)));
    b.build_br_if(isAlive, udfBlock, {}, loopLatch, {matchCount});

    b.position_at_end(udfBlock);
    std::vector<Value*> callArgs;
    callArgs.reserve(udf.argColIndices.size());
    for (size_t k = 0; k < udf.argColIndices.size(); ++k) {
        const ColType ct = snap.schema.columns[udf.argColIndices[k]].type;
        const Type cparam = callee->param_type(k);
        Type callType = mirStorageType(ct);
        if (!udf.nanBoxedCallee) {
            if (cparam == Type::f64() || cparam == Type::f32() || cparam == Type::i64() || cparam == Type::i32()) {
                callType = cparam;
            }
        } else {
            // NaNBoxed callee accepts Tagged: pass unboxed f64 for floats, i32 for ints/bools.
            callType = colTypeIsFloat(ct) ? Type::f64() : Type::i32();
        }
        callArgs.push_back(emitColValueForCallee(b, ct, argColPtrs[k], row64, callType));
    }

    Type callRetType = callee->return_type();
    if (udf.nanBoxedCallee && callRetType == Type::tagged()) {
        callRetType = Type::i32();
    } else if (callRetType.is_void()) {
        return fail("compileFusedUdfFilterKernel: predicate callee cannot return void");
    }

    Value* callVal = b.build_call(udf.calleeName, callRetType, callArgs);
    brass::Instruction* callInst = callVal ? callVal->defining_instruction() : nullptr;
    Value* cond = callResultToTruthyI32(b, callVal);
    b.build_br_if(cond, onMatch, {}, loopLatch, {matchCount});

    b.position_at_end(onMatch);
    Value* mc64 = b.build_zext_i64(matchCount);
    b.build_store_indexed(Type::i32(), matchIdxPtr, mc64, 4, 0, row);
    Value* nextMc = b.build_add(matchCount, b.build_iconst_i32(1));
    b.build_br(loopLatch, {nextMc});

    b.position_at_end(loopLatch);
    Value* nextRow = b.build_add(row, b.build_iconst_i32(1));
    b.build_br(loopHdr, {nextRow, latchCount});

    b.position_at_end(loopExit);
    b.build_store(Type::i32(), ctxPtr,
                  static_cast<int32_t>(offsetof(JitChunkContext, matchCount)), finalCount);
    b.build_ret_void();

    fn->rebuild_cfg_predecessors();

    brass::CrossDomainFusionOptions fopts;
    fopts.allow_nanbox_unboxing = udf.nanBoxedCallee;
    fopts.max_inlined_insts = 1024;
    fopts.audit_opts.require_no_gc = true;
    fopts.audit_opts.require_no_pinned_tls = true;
    fopts.audit_opts.require_no_alloc = true;

    const brass::CrossDomainFusionResult fres = brass::fuse_cross_module_call(
        *fn, callInst, mod, *udf.sourceModule, udf.calleeName, fopts);
    if (!fres.fused) {
        return fail(fres.diagnostic.empty() ? "compileFusedUdfFilterKernel: cross-module fusion failed"
                                            : fres.diagnostic);
    }

    brass::codegen::KernelOptions opts;
    opts.opt_level = 2;
    opts.enable_optimizations = true;
    opts.enable_superopt = false;
    opts.enable_smt_guard_elim = false;
    opts.enable_vectorize = false;
    opts.enable_slp = false;
    opts.enable_parallel = false;

    try {
        brass::codegen::KernelJit jit(opts);
        out->func = jit.compile(mod, entryName);
        out->entry = reinterpret_cast<JitKernelFnPtr>(out->func.entry_point());
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }
    if (!out->entry) {
        return fail("compileFusedUdfFilterKernel: brass JIT returned null entry pointer");
    }
    return out;
}

}  // namespace bro::table
