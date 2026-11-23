#include "table/query_jit_agg.h"

#include "brass/target/host_cpu.hpp"
#include "table/query_jit.h"
#include "table/query_jit_emit.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <exception>

namespace bro::table {

namespace {

using namespace jit_detail;

bool constFitsColType(ColType ct, const ExprNode& litNode) {
    if (litNode.op != ExprOp::Const || litNode.literal.isNull || litNode.literal.type == ColType::String) {
        return false;
    }
    if (ct == ColType::I32) {
        if (colTypeIsFloat(litNode.literal.type)) return false;
        if (colTypeIsUnsignedInt(litNode.literal.type) &&
            litNode.literal.u64 > static_cast<uint64_t>(INT32_MAX)) {
            return false;
        }
        return litNode.literal.i64 >= INT32_MIN && litNode.literal.i64 <= INT32_MAX;
    }
    if (ct == ColType::U32) {
        if (colTypeIsFloat(litNode.literal.type)) return false;
        if (!colTypeIsUnsignedInt(litNode.literal.type) && litNode.literal.i64 < 0) return false;
        return litNode.literal.u64 <= UINT32_MAX;
    }
    if (ct == ColType::I64) {
        return !colTypeIsFloat(litNode.literal.type);
    }
    return true;
}

bool inferSimdValueColType(const ExprArena& arena, uint32_t id, ColType& outType, bool& hasCol) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) return false;
    const ExprNode& n = arena.nodes[id];
    if (n.op == ExprOp::ColRef) {
        if (n.tableSide != 0 || n.resolvedCol < 0) return false;
        const ColType ct = n.valueType;
        if (ct == ColType::String || ct == ColType::Bool) return false;
        if (hasCol && outType != ct) return false;
        outType = ct;
        hasCol = true;
        return true;
    }
    if (n.op == ExprOp::Const) {
        return !n.literal.isNull && n.literal.type != ColType::String;
    }
    if (n.op == ExprOp::Add || n.op == ExprOp::Sub ||
        n.op == ExprOp::Mul || n.op == ExprOp::Div) {
        ColType lt = ColType::I32;
        ColType rt = ColType::I32;
        bool lCol = false;
        bool rCol = false;
        if (!inferSimdValueColType(arena, n.lhs, lt, lCol) ||
            !inferSimdValueColType(arena, n.rhs, rt, rCol)) {
            return false;
        }
        if (!lCol && !rCol) return false;
        if (lCol && rCol && lt != rt) return false;
        const ColType ct = lCol ? lt : rt;
        if (ct == ColType::Dict || ct == ColType::U32) return false;
        if (n.valueType != ct) return false;
        if (n.op == ExprOp::Div && !colTypeIsFloat(ct)) return false;
        if (n.op == ExprOp::Mul && ct == ColType::I64) return false;
        if (!lCol && !constFitsColType(ct, arena.nodes[n.lhs])) return false;
        if (!rCol && !constFitsColType(ct, arena.nodes[n.rhs])) return false;
        if (hasCol && outType != ct) return false;
        outType = ct;
        hasCol = true;
        return true;
    }
    return false;
}

bool isSimdPredExpr(const ExprArena& arena, uint32_t id, uint32_t& outLaneBytes) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) return false;
    const ExprNode& n = arena.nodes[id];
    if (n.op == ExprOp::And || n.op == ExprOp::Or) {
        return isSimdPredExpr(arena, n.lhs, outLaneBytes) &&
               isSimdPredExpr(arena, n.rhs, outLaneBytes);
    }
    if (n.op == ExprOp::Not) {
        return isSimdPredExpr(arena, n.lhs, outLaneBytes);
    }
    if (n.op != ExprOp::Eq && n.op != ExprOp::Ne && n.op != ExprOp::Lt &&
        n.op != ExprOp::Le && n.op != ExprOp::Gt && n.op != ExprOp::Ge &&
        n.op != ExprOp::StrEq && n.op != ExprOp::StrNe) {
        return false;
    }

    const ExprNode& ln = arena.nodes[n.lhs];
    const ExprNode& rn = arena.nodes[n.rhs];
    if (ln.op == ExprOp::ColRef && ln.tableSide == 0 && ln.resolvedCol >= 0 &&
        ln.valueType == ColType::Dict &&
        rn.op == ExprOp::Const && rn.literal.type == ColType::String) {
        if (n.op != ExprOp::Eq && n.op != ExprOp::Ne &&
            n.op != ExprOp::StrEq && n.op != ExprOp::StrNe) {
            return false;
        }
        const uint32_t eb = 4;
        if (outLaneBytes == 0) outLaneBytes = eb;
        return outLaneBytes == eb;
    }
    if (rn.op == ExprOp::ColRef && rn.tableSide == 0 && rn.resolvedCol >= 0 &&
        rn.valueType == ColType::Dict &&
        ln.op == ExprOp::Const && ln.literal.type == ColType::String) {
        if (n.op != ExprOp::Eq && n.op != ExprOp::Ne) return false;
        const uint32_t eb = 4;
        if (outLaneBytes == 0) outLaneBytes = eb;
        return outLaneBytes == eb;
    }
    if (n.op == ExprOp::StrEq || n.op == ExprOp::StrNe) return false;

    ColType lt = ColType::I32;
    ColType rt = ColType::I32;
    bool lCol = false;
    bool rCol = false;
    if (!inferSimdValueColType(arena, n.lhs, lt, lCol) ||
        !inferSimdValueColType(arena, n.rhs, rt, rCol)) {
        return false;
    }
    if (!lCol && !rCol) return false;
    if (lCol && rCol && lt != rt) return false;
    const ColType ct = lCol ? lt : rt;
    if (!lCol && !constFitsColType(ct, ln)) return false;
    if (!rCol && !constFitsColType(ct, rn)) return false;
    if (ct == ColType::Dict || ct == ColType::U32) {
        if (n.op != ExprOp::Eq && n.op != ExprOp::Ne) return false;
    }
    const uint32_t eb = static_cast<uint32_t>(colTypeElemSize(ct));
    if (eb != 4 && eb != 8) return false;
    if (outLaneBytes == 0) outLaneBytes = eb;
    return outLaneBytes == eb;
}

Opcode simdCmpOpcode(ExprOp op) {
    switch (op) {
    case ExprOp::Eq:
    case ExprOp::StrEq: return Opcode::vcmp_eq;
    case ExprOp::Ne:
    case ExprOp::StrNe: return Opcode::vcmp_ne;
    case ExprOp::Lt: return Opcode::vcmp_lt;
    case ExprOp::Le: return Opcode::vcmp_le;
    case ExprOp::Gt: return Opcode::vcmp_gt;
    case ExprOp::Ge: return Opcode::vcmp_ge;
    default: return Opcode::vcmp_eq;
    }
}

Value* buildBroadcastScalar(Builder& b, ColType ct, const ExprNode& litNode, bool useAvx2) {
    Type vt = vecTypeForCol(ct, useAvx2);
    switch (ct) {
    case ColType::I32:
        return b.build_vbroadcast(vt, b.build_iconst_i32(static_cast<int32_t>(litNode.literal.i64)));
    case ColType::U32:
        return b.build_vbroadcast(vt, b.build_iconst_i32(static_cast<int32_t>(static_cast<uint32_t>(litNode.literal.u64))));
    case ColType::Dict: {
        uint32_t did = (litNode.literal.type == ColType::String)
                           ? litNode.cachedDictId
                           : static_cast<uint32_t>(litNode.literal.u64);
        return b.build_vbroadcast(vt, b.build_iconst_i32(static_cast<int32_t>(did)));
    }
    case ColType::I64:
        return b.build_vbroadcast(vt, b.build_iconst_i64(litNode.literal.i64));
    case ColType::F32: {
        float fv = static_cast<float>(colTypeIsFloat(litNode.literal.type)
                                          ? litNode.literal.f64
                                          : static_cast<double>(litNode.literal.i64));
        return b.build_vbroadcast(vt, b.build_fconst_f32(fv));
    }
    case ColType::F64: {
        double dv = colTypeIsFloat(litNode.literal.type)
                        ? litNode.literal.f64
                        : (colTypeIsUnsignedInt(litNode.literal.type)
                               ? static_cast<double>(litNode.literal.u64)
                               : static_cast<double>(litNode.literal.i64));
        return b.build_vbroadcast(vt, b.build_fconst_f64(dv));
    }
    default:
        return b.build_vzero(vt);
    }
}

Value* emitSimdValueExpr(Builder& b, const QueryPlan& plan, uint32_t id, ColType ct,
                         const std::vector<Value*>& colPtrs, Value* byteOff, bool useAvx2) {
    const ExprNode& n = plan.exprs.nodes[id];
    if (n.op == ExprOp::ColRef) {
        Value* addr = b.build_add(colPtrs[static_cast<size_t>(n.resolvedCol)], byteOff);
        return b.build_vload(vecTypeForCol(ct, useAvx2), addr, 0);
    }
    if (n.op == ExprOp::Const) {
        return buildBroadcastScalar(b, ct, n, useAvx2);
    }
    Value* l = emitSimdValueExpr(b, plan, n.lhs, ct, colPtrs, byteOff, useAvx2);
    Value* r = emitSimdValueExpr(b, plan, n.rhs, ct, colPtrs, byteOff, useAvx2);
    if (n.op == ExprOp::Add) return b.build_vadd(l, r);
    if (n.op == ExprOp::Sub) return b.build_vsub(l, r);
    if (n.op == ExprOp::Mul) return b.build_vmul(l, r);
    return b.build_vdiv(l, r);
}

Value* emitSimdMaskExpr(Builder& b, const QueryPlan& plan, uint32_t id,
                        const std::vector<Value*>& colPtrs, Value* byteOff, bool useAvx2) {
    const ExprNode& n = plan.exprs.nodes[id];
    if (n.op == ExprOp::And) {
        return b.build_vand(emitSimdMaskExpr(b, plan, n.lhs, colPtrs, byteOff, useAvx2),
                            emitSimdMaskExpr(b, plan, n.rhs, colPtrs, byteOff, useAvx2));
    }
    if (n.op == ExprOp::Or) {
        return b.build_vor(emitSimdMaskExpr(b, plan, n.lhs, colPtrs, byteOff, useAvx2),
                           emitSimdMaskExpr(b, plan, n.rhs, colPtrs, byteOff, useAvx2));
    }
    if (n.op == ExprOp::Not) {
        return b.build_vnot(emitSimdMaskExpr(b, plan, n.lhs, colPtrs, byteOff, useAvx2));
    }

    ColType ct = ColType::I32;
    bool hasCol = false;
    (void)inferSimdValueColType(plan.exprs, n.lhs, ct, hasCol);
    (void)inferSimdValueColType(plan.exprs, n.rhs, ct, hasCol);
    Value* lv = emitSimdValueExpr(b, plan, n.lhs, ct, colPtrs, byteOff, useAvx2);
    Value* rv = emitSimdValueExpr(b, plan, n.rhs, ct, colPtrs, byteOff, useAvx2);
    return b.build_vcmp(simdCmpOpcode(n.op), lv, rv);
}

}  // namespace

bool canCompileSimdFilter(const QueryPlan& plan, const TableSnapshot& snap) {
    if (plan.filters.empty()) return false;
    uint32_t laneBytes = 0;
    for (uint32_t f : plan.filters) {
        if (!canJitExpr(plan.exprs, f, snap)) return false;
        if (!isSimdPredExpr(plan.exprs, f, laneBytes)) return false;
    }
    return laneBytes == 4 || laneBytes == 8;
}

std::shared_ptr<const CompiledKernel> compileSimdFilterKernel(
    const QueryPlan& plan, const TableSnapshot& snap, std::string* outError) {
    uint32_t laneBytes = 0;
    for (uint32_t f : plan.filters) {
        if (!isSimdPredExpr(plan.exprs, f, laneBytes)) {
            if (outError) *outError = "query is not SIMD filter eligible";
            return nullptr;
        }
    }
    const bool useAvx2 = brass::host_has_avx2();
    const uint32_t lanes = useAvx2 ? (laneBytes == 8 ? 4u : 8u)
                                   : (laneBytes == 8 ? 2u : 4u);
    const size_t nCols = snap.schema.size();
    std::vector<bool> usedCols(nCols, false);
    for (uint32_t f : plan.filters) collectUsedCols(plan.exprs, f, usedCols);

    auto out = std::make_shared<CompiledKernel>();
    out->mode = JitMode::Filter;
    out->usesSimd = true;
    out->simdWidth = useAvx2 ? 256 : 128;
    out->usedColIndices = collectUsedColIndices(usedCols);
    for (uint32_t i = 0; i < plan.filters.size(); ++i) out->jittableFilterIndices.push_back(i);

    Module mod("table_simd_filter");
    Builder b(mod);
    Function* fn = mod.create_function("table_simd_filter", Type::void_type(), {Type::ptr()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    Value* ctxPtr = b.add_block_param(entry, Type::ptr());
    b.position_at_end(entry);

    Value* leftBase = b.build_load(Type::ptr(), ctxPtr, static_cast<int32_t>(offsetof(JitChunkContext, leftColPtrs)));
    std::vector<Value*> colPtrs(nCols, nullptr);
    for (size_t c = 0; c < nCols; ++c) {
        if (usedCols[c]) colPtrs[c] = b.build_load(Type::ptr(), leftBase, static_cast<int32_t>(c * sizeof(void*)));
    }
    Value* aliveWords = b.build_load(Type::ptr(), ctxPtr, static_cast<int32_t>(offsetof(JitChunkContext, aliveWords)));
    Value* hasAliveMask = b.build_load(Type::i32(), ctxPtr, static_cast<int32_t>(offsetof(JitChunkContext, hasAliveMask)));
    Value* rowCount = b.build_load(Type::i32(), ctxPtr, static_cast<int32_t>(offsetof(JitChunkContext, rowCount)));
    Value* matchIdxPtr = b.build_load(Type::ptr(), ctxPtr, static_cast<int32_t>(offsetof(JitChunkContext, matchIndices)));
    Value* simdEnd = b.build_and(rowCount, b.build_iconst_i32(~static_cast<int32_t>(lanes - 1)));

    BasicBlock* simdHdr = createAttachedBlock(fn, b, "simd_hdr");
    Value* sRow = b.add_block_param(simdHdr, Type::i32());
    Value* sMc = b.add_block_param(simdHdr, Type::i32());
    BasicBlock* simdBody = createAttachedBlock(fn, b, "simd_body");
    BasicBlock* simdExtract = createAttachedBlock(fn, b, "simd_extract");
    BasicBlock* simdLatch = createAttachedBlock(fn, b, "simd_latch");
    Value* sLatchMc = b.add_block_param(simdLatch, Type::i32());

    BasicBlock* scalarHdr = createAttachedBlock(fn, b, "scalar_hdr");
    Value* row = b.add_block_param(scalarHdr, Type::i32());
    Value* matchCount = b.add_block_param(scalarHdr, Type::i32());
    BasicBlock* aliveChk = createAttachedBlock(fn, b, "alive_chk");
    BasicBlock* aliveLoad = createAttachedBlock(fn, b, "alive_load");
    std::vector<BasicBlock*> filterBlocks;
    for (size_t fi = 0; fi < plan.filters.size(); ++fi) {
        filterBlocks.push_back(createAttachedBlock(fn, b, "s_flt_" + std::to_string(fi)));
    }
    BasicBlock* onMatch = createAttachedBlock(fn, b, "on_match");
    BasicBlock* scalarLatch = createAttachedBlock(fn, b, "scalar_latch");
    Value* latchCount = b.add_block_param(scalarLatch, Type::i32());
    BasicBlock* loopExit = createAttachedBlock(fn, b, "loop_exit");
    Value* finalCount = b.add_block_param(loopExit, Type::i32());

    b.build_br_if(b.build_eq(hasAliveMask, b.build_iconst_i32(0)),
                  simdHdr, {b.build_iconst_i32(0), b.build_iconst_i32(0)},
                  scalarHdr, {b.build_iconst_i32(0), b.build_iconst_i32(0)});

    b.position_at_end(simdHdr);
    b.build_br_if(b.build_slt(sRow, simdEnd), simdBody, {}, scalarHdr, {simdEnd, sMc});

    b.position_at_end(simdBody);
    Value* byteOff = b.build_mul(b.build_zext_i64(sRow), b.build_iconst_i64(laneBytes));
    Value* mask = emitSimdMaskExpr(b, plan, plan.filters[0], colPtrs, byteOff, useAvx2);
    for (size_t fi = 1; fi < plan.filters.size(); ++fi) {
        mask = b.build_vand(mask, emitSimdMaskExpr(b, plan, plan.filters[fi], colPtrs, byteOff, useAvx2));
    }
    Value* anyHit = b.build_vany(mask);
    b.build_br_if(anyHit, simdExtract, {}, simdLatch, {sMc});

    b.position_at_end(simdExtract);
    Value* curMc = sMc;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        Value* ex = b.build_vextract_lane(mask, lane);
        Value* bit = (laneBytes == 8) ? b.build_ne(ex, b.build_iconst_i64(0))
                                      : b.build_ne(ex, b.build_iconst_i32(0));
        Value* mc64 = b.build_zext_i64(curMc);
        b.build_store_indexed(Type::i32(), matchIdxPtr, mc64, 4, 0,
                              b.build_add(sRow, b.build_iconst_i32(static_cast<int32_t>(lane))));
        curMc = b.build_add(curMc, bit);
    }
    b.build_br(simdLatch, {curMc});

    b.position_at_end(simdLatch);
    b.build_br(simdHdr, {b.build_add(sRow, b.build_iconst_i32(static_cast<int32_t>(lanes))), sLatchMc});

    b.position_at_end(scalarHdr);
    b.build_br_if(b.build_slt(row, rowCount), aliveChk, {}, loopExit, {matchCount});

    b.position_at_end(aliveChk);
    Value* row64 = b.build_zext_i64(row);
    BasicBlock* firstStep = filterBlocks.empty() ? onMatch : filterBlocks[0];
    b.build_br_if(b.build_ne(hasAliveMask, b.build_iconst_i32(0)),
                  aliveLoad, {}, firstStep, {});

    b.position_at_end(aliveLoad);
    Value* wordIdx = b.build_lshr(row64, b.build_iconst_i64(6));
    Value* word = b.build_load_indexed(Type::i64(), aliveWords, wordIdx, 8, 0);
    Value* bit = b.build_and(row64, b.build_iconst_i64(63));
    Value* isAlive = b.build_trunc_i32(b.build_and(b.build_lshr(word, bit), b.build_iconst_i64(1)));
    b.build_br_if(isAlive, firstStep, {}, scalarLatch, {matchCount});

    ExprEmitter emitter(b, fn, plan, colPtrs, row64);
    for (size_t fi = 0; fi < plan.filters.size(); ++fi) {
        b.position_at_end(filterBlocks[fi]);
        MirVal pred = emitter.emit(plan.filters[fi]);
        BasicBlock* nextBb = (fi + 1 < filterBlocks.size()) ? filterBlocks[fi + 1] : onMatch;
        b.build_br_if(buildTruthy(b, pred), nextBb, {}, scalarLatch, {matchCount});
    }

    b.position_at_end(onMatch);
    b.build_store_indexed(Type::i32(), matchIdxPtr, b.build_zext_i64(matchCount), 4, 0, row);
    b.build_br(scalarLatch, {b.build_add(matchCount, b.build_iconst_i32(1))});

    b.position_at_end(scalarLatch);
    b.build_br(scalarHdr, {b.build_add(row, b.build_iconst_i32(1)), latchCount});

    b.position_at_end(loopExit);
    b.build_store(Type::i32(), ctxPtr, static_cast<int32_t>(offsetof(JitChunkContext, matchCount)), finalCount);
    b.build_ret_void();

    fn->rebuild_cfg_predecessors();

    brass::codegen::KernelOptions opts;
    opts.enable_superopt = false;
    opts.enable_smt_guard_elim = false;
    opts.enable_vectorize = false;
    opts.enable_slp = false;
    opts.enable_parallel = false;
    opts.enable_avx2 = useAvx2;
    try {
        brass::codegen::KernelJit jit(opts);
        out->func = jit.compile(mod, "table_simd_filter");
        out->entry = reinterpret_cast<JitKernelFnPtr>(out->func.entry_point());
    } catch (const std::exception& ex) {
        if (outError) *outError = ex.what();
        return nullptr;
    }
    return out->entry ? out : nullptr;
}

}  // namespace bro::table
