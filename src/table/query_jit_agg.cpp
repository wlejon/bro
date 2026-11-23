#include "table/query_jit_agg.h"

#include "table/query_jit.h"
#include "table/query_jit_emit.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <exception>

namespace bro::table {

namespace {

using namespace jit_detail;

bool isSimdLeafExpr(const ExprArena& arena, uint32_t id, uint32_t& outLaneBytes) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) return false;
    const ExprNode& n = arena.nodes[id];
    if (n.op == ExprOp::And || n.op == ExprOp::Or) {
        return isSimdLeafExpr(arena, n.lhs, outLaneBytes) &&
               isSimdLeafExpr(arena, n.rhs, outLaneBytes);
    }
    if (n.op == ExprOp::Not) {
        return isSimdLeafExpr(arena, n.lhs, outLaneBytes);
    }
    if (n.op != ExprOp::Eq && n.op != ExprOp::Ne && n.op != ExprOp::Lt &&
        n.op != ExprOp::Le && n.op != ExprOp::Gt && n.op != ExprOp::Ge &&
        n.op != ExprOp::StrEq && n.op != ExprOp::StrNe) {
        return false;
    }
    const ExprNode* colNode = &arena.nodes[n.lhs];
    const ExprNode* litNode = &arena.nodes[n.rhs];
    if (colNode->op == ExprOp::Const && litNode->op == ExprOp::ColRef) {
        std::swap(colNode, litNode);
    }
    if (colNode->op != ExprOp::ColRef || colNode->tableSide != 0 || colNode->resolvedCol < 0 ||
        litNode->op != ExprOp::Const) {
        return false;
    }
    const ColType ct = colNode->valueType;
    if (ct == ColType::String || ct == ColType::Bool) return false;
    if (ct == ColType::Dict || ct == ColType::U32) {
        if (n.op != ExprOp::Eq && n.op != ExprOp::Ne &&
            n.op != ExprOp::StrEq && n.op != ExprOp::StrNe) {
            return false;  // x64 vcmp_lt/gt is signed
        }
        if (ct == ColType::U32 &&
            (colTypeIsFloat(litNode->literal.type) ||
             (!colTypeIsUnsignedInt(litNode->literal.type) && litNode->literal.i64 < 0) ||
             litNode->literal.u64 > UINT32_MAX)) {
            return false;
        }
    } else if (colTypeIsInteger(ct)) {
        if (colTypeIsFloat(litNode->literal.type)) return false;
        if (ct == ColType::I32 &&
            (litNode->literal.i64 < INT32_MIN || litNode->literal.i64 > INT32_MAX)) {
            return false;
        }
    }
    const uint32_t eb = static_cast<uint32_t>(colTypeElemSize(ct));
    if (eb != 4 && eb != 8) return false;
    if (outLaneBytes == 0) outLaneBytes = eb;
    return outLaneBytes == eb;
}

Type vecTypeForCol(ColType ct) {
    switch (ct) {
    case ColType::I32:
    case ColType::U32:
    case ColType::Dict:
        return Type::i32x4();
    case ColType::F32:
        return Type::f32x4();
    case ColType::I64:
        return Type::i64x2();
    case ColType::F64:
        return Type::f64x2();
    default:
        return Type::i32x4();
    }
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

ExprOp flipCmp(ExprOp op) {
    switch (op) {
    case ExprOp::Lt: return ExprOp::Gt;
    case ExprOp::Le: return ExprOp::Ge;
    case ExprOp::Gt: return ExprOp::Lt;
    case ExprOp::Ge: return ExprOp::Le;
    default: return op;
    }
}

Value* buildBroadcastScalarForCol(Builder& b, ColType ct, const ExprNode& litNode) {
    Type vt = vecTypeForCol(ct);
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

Value* emitSimdMaskExpr(Builder& b, const QueryPlan& plan, uint32_t id,
                        const std::vector<Value*>& colPtrs, Value* byteOff,
                        Type maskVecType) {
    const ExprNode& n = plan.exprs.nodes[id];
    if (n.op == ExprOp::And) {
        return b.build_vand(emitSimdMaskExpr(b, plan, n.lhs, colPtrs, byteOff, maskVecType),
                            emitSimdMaskExpr(b, plan, n.rhs, colPtrs, byteOff, maskVecType));
    }
    if (n.op == ExprOp::Or) {
        return b.build_vor(emitSimdMaskExpr(b, plan, n.lhs, colPtrs, byteOff, maskVecType),
                           emitSimdMaskExpr(b, plan, n.rhs, colPtrs, byteOff, maskVecType));
    }
    if (n.op == ExprOp::Not) {
        Value* m = emitSimdMaskExpr(b, plan, n.lhs, colPtrs, byteOff, maskVecType);
        return b.build_vnot(m);
    }
    const ExprNode* colNode = &plan.exprs.nodes[n.lhs];
    const ExprNode* litNode = &plan.exprs.nodes[n.rhs];
    ExprOp cmpOp = n.op;
    if (colNode->op == ExprOp::Const && litNode->op == ExprOp::ColRef) {
        std::swap(colNode, litNode);
        cmpOp = flipCmp(cmpOp);
    }
    Value* addr = b.build_add(colPtrs[static_cast<size_t>(colNode->resolvedCol)], byteOff);
    Value* vecVal = b.build_vload(vecTypeForCol(colNode->valueType), addr, 0);
    Value* rhsVec = buildBroadcastScalarForCol(b, colNode->valueType, *litNode);
    return b.build_vcmp(simdCmpOpcode(cmpOp), vecVal, rhsVec);
}

bool isSimdReducibleAgg(const AggSpec& a, const QueryPlan& plan, uint32_t& outLaneBytes) {
    if (a.kind == AggSpec::Kind::Count && a.expr == kInvalidExpr) return true;
    if (a.expr == kInvalidExpr) return false;
    const ExprNode& en = plan.exprs.nodes[a.expr];
    if (en.op != ExprOp::ColRef || en.tableSide != 0 || en.resolvedCol < 0) return false;
    const ColType ct = en.valueType;
    if (a.kind == AggSpec::Kind::Count) return true;
    if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
        // Keep integer sum overflow-free on I64 and FP sum exact on F64.
        if (ct != ColType::F64 && ct != ColType::I64) return false;
        if (outLaneBytes == 0) outLaneBytes = 8;
        return outLaneBytes == 8;
    }
    // Min / Max: x64 supports F32x4, F64x2, I32x4 natively (no I64x2 vmin/vmax).
    if (ct == ColType::F64) {
        if (outLaneBytes == 0) outLaneBytes = 8;
        return outLaneBytes == 8;
    }
    if (ct == ColType::F32 || ct == ColType::I32) {
        if (outLaneBytes == 0) outLaneBytes = 4;
        return outLaneBytes == 4;
    }
    return false;
}

}  // namespace

bool canCompileSimdFilter(const QueryPlan& plan, const TableSnapshot& snap) {
    if (plan.filters.empty()) return false;
    uint32_t laneBytes = 0;
    for (uint32_t f : plan.filters) {
        if (!canJitExpr(plan.exprs, f, snap)) return false;
        if (!isSimdLeafExpr(plan.exprs, f, laneBytes)) return false;
    }
    return laneBytes == 4 || laneBytes == 8;
}

bool canCompileAggKernel(const QueryPlan& plan, const TableSnapshot& snap) {
    if (plan.join.kind != JoinSpec::Kind::None || !plan.groupKeys.empty() ||
        plan.aggregates.empty() || plan.aggregates.size() > kMaxJitAggs) {
        return false;
    }
    for (uint32_t f : plan.filters) {
        if (!canJitExpr(plan.exprs, f, snap)) return false;
    }
    for (const AggSpec& a : plan.aggregates) {
        if (a.kind == AggSpec::Kind::Count && a.expr == kInvalidExpr) continue;
        if (a.kind == AggSpec::Kind::Min || a.kind == AggSpec::Kind::Max) {
            if (a.outputType == ColType::String || a.outputType == ColType::Dict) return false;
        }
        if (!canJitExpr(plan.exprs, a.expr, snap)) return false;
    }
    return true;
}

std::shared_ptr<const CompiledKernel> compileSimdFilterKernel(
    const QueryPlan& plan, const TableSnapshot& snap, std::string* outError) {
    uint32_t laneBytes = 0;
    for (uint32_t f : plan.filters) {
        if (!isSimdLeafExpr(plan.exprs, f, laneBytes)) {
            if (outError) *outError = "query is not SIMD filter eligible";
            return nullptr;
        }
    }
    const uint32_t lanes = (laneBytes == 8) ? 2u : 4u;
    const Type maskVecType = (laneBytes == 8) ? Type::i64x2() : Type::i32x4();
    const size_t nCols = snap.schema.size();
    std::vector<bool> usedCols(nCols, false);
    for (uint32_t f : plan.filters) collectUsedCols(plan.exprs, f, usedCols);

    auto out = std::make_shared<CompiledKernel>();
    out->mode = JitMode::Filter;
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
    Value* mask = emitSimdMaskExpr(b, plan, plan.filters[0], colPtrs, byteOff, maskVecType);
    for (size_t fi = 1; fi < plan.filters.size(); ++fi) {
        mask = b.build_vand(mask, emitSimdMaskExpr(b, plan, plan.filters[fi], colPtrs, byteOff, maskVecType));
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

std::shared_ptr<const CompiledAggKernel> compileAggKernel(
    const QueryPlan& plan, const TableSnapshot& snap, std::string* outError) {
    if (!canCompileAggKernel(plan, snap)) {
        if (outError) *outError = "query is not JIT aggregation eligible";
        return nullptr;
    }

    const size_t nCols = snap.schema.size();
    const size_t nAggs = plan.aggregates.size();
    std::vector<bool> usedCols(nCols, false);
    for (uint32_t f : plan.filters) collectUsedCols(plan.exprs, f, usedCols);
    for (const AggSpec& a : plan.aggregates) {
        if (a.expr != kInvalidExpr) collectUsedCols(plan.exprs, a.expr, usedCols);
    }

    uint32_t simdLaneBytes = 0;
    bool simdEligible = plan.filters.empty();
    bool hasSimdNumericAgg = false;
    if (simdEligible) {
        for (const AggSpec& a : plan.aggregates) {
            if (!isSimdReducibleAgg(a, plan, simdLaneBytes)) {
                simdEligible = false;
                break;
            }
            if (a.kind != AggSpec::Kind::Count) hasSimdNumericAgg = true;
        }
        if (!hasSimdNumericAgg || (simdLaneBytes != 4 && simdLaneBytes != 8)) {
            simdEligible = false;
        }
    }

    auto out = std::make_shared<CompiledAggKernel>();
    out->usesSimd = simdEligible;

    Module mod("table_jit_agg");
    Builder b(mod);
    Function* fn = mod.create_function("table_jit_agg", Type::void_type(), {Type::ptr()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    Value* ctxPtr = b.add_block_param(entry, Type::ptr());
    b.position_at_end(entry);

    Value* leftBase = b.build_load(Type::ptr(), ctxPtr, static_cast<int32_t>(offsetof(JitAggContext, leftColPtrs)));
    std::vector<Value*> colPtrs(nCols, nullptr);
    for (size_t c = 0; c < nCols; ++c) {
        if (usedCols[c]) colPtrs[c] = b.build_load(Type::ptr(), leftBase, static_cast<int32_t>(c * sizeof(void*)));
    }
    Value* aliveWords = b.build_load(Type::ptr(), ctxPtr, static_cast<int32_t>(offsetof(JitAggContext, aliveWords)));
    Value* hasAliveMask = b.build_load(Type::i32(), ctxPtr, static_cast<int32_t>(offsetof(JitAggContext, hasAliveMask)));
    Value* rowCount = b.build_load(Type::i32(), ctxPtr, static_cast<int32_t>(offsetof(JitAggContext, rowCount)));

    std::vector<ColType> accScalarTypes(nAggs, ColType::I64);
    std::vector<Value*> initAccs(nAggs, nullptr);
    for (size_t ai = 0; ai < nAggs; ++ai) {
        const AggSpec& a = plan.aggregates[ai];
        if (a.kind == AggSpec::Kind::Count) {
            accScalarTypes[ai] = ColType::I64;
            initAccs[ai] = b.build_iconst_i64(0);
        } else if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
            const bool isFlt = (a.outputType == ColType::F64);
            accScalarTypes[ai] = isFlt ? ColType::F64 : ColType::I64;
            initAccs[ai] = isFlt ? b.build_fconst_f64(0.0) : b.build_iconst_i64(0);
        } else {
            accScalarTypes[ai] = a.outputType;
            initAccs[ai] = (a.outputType == ColType::F64)
                               ? b.build_fconst_f64(0.0)
                               : (a.outputType == ColType::F32
                                      ? b.build_fconst_f32(0.0f)
                                      : (a.outputType == ColType::I64 ? b.build_iconst_i64(0)
                                                                      : b.build_iconst_i32(0)));
        }
    }

    BasicBlock* scalarHdr = createAttachedBlock(fn, b, "agg_hdr");
    Value* row = b.add_block_param(scalarHdr, Type::i32());
    Value* mc = b.add_block_param(scalarHdr, Type::i32());
    std::vector<Value*> sAccs(nAggs, nullptr);
    for (size_t ai = 0; ai < nAggs; ++ai) {
        sAccs[ai] = b.add_block_param(scalarHdr, mirStorageType(accScalarTypes[ai]));
    }

    BasicBlock* aliveChk = createAttachedBlock(fn, b, "alive_chk");
    BasicBlock* aliveLoad = createAttachedBlock(fn, b, "alive_load");
    std::vector<BasicBlock*> filterBlocks;
    for (size_t fi = 0; fi < plan.filters.size(); ++fi) {
        filterBlocks.push_back(createAttachedBlock(fn, b, "flt_" + std::to_string(fi)));
    }
    BasicBlock* onMatch = createAttachedBlock(fn, b, "on_match");
    BasicBlock* loopLatch = createAttachedBlock(fn, b, "agg_latch");
    Value* latchMc = b.add_block_param(loopLatch, Type::i32());
    std::vector<Value*> lAccs(nAggs, nullptr);
    for (size_t ai = 0; ai < nAggs; ++ai) {
        lAccs[ai] = b.add_block_param(loopLatch, mirStorageType(accScalarTypes[ai]));
    }
    BasicBlock* loopExit = createAttachedBlock(fn, b, "agg_exit");
    Value* finalMc = b.add_block_param(loopExit, Type::i32());
    std::vector<Value*> fAccs(nAggs, nullptr);
    for (size_t ai = 0; ai < nAggs; ++ai) {
        fAccs[ai] = b.add_block_param(loopExit, mirStorageType(accScalarTypes[ai]));
    }

    if (simdEligible) {
        const uint32_t lanes = (simdLaneBytes == 8) ? 2u : 4u;
        const int32_t iLanes = static_cast<int32_t>(lanes);
        Value* simdEnd = b.build_and(rowCount, b.build_iconst_i32(~(iLanes - 1)));
        Value* canDoSimd = b.build_and(
            b.build_eq(hasAliveMask, b.build_iconst_i32(0)),
            b.build_sge(rowCount, b.build_iconst_i32(iLanes)));

        BasicBlock* simdInit = createAttachedBlock(fn, b, "simd_init");
        BasicBlock* simdHdr = createAttachedBlock(fn, b, "simd_hdr");
        BasicBlock* simdBody = createAttachedBlock(fn, b, "simd_body");
        BasicBlock* simdMerge = createAttachedBlock(fn, b, "simd_merge");

        b.build_br_if(canDoSimd, simdInit, {},
                      scalarHdr, [&]() {
                          std::vector<Value*> args = {b.build_iconst_i32(0), b.build_iconst_i32(0)};
                          args.insert(args.end(), initAccs.begin(), initAccs.end());
                          return args;
                      }());

        b.position_at_end(simdInit);
        std::vector<Value*> simdInitVecs(nAggs, nullptr);
        std::vector<Type> simdVecTypes(nAggs, Type::f64x2());
        for (size_t ai = 0; ai < nAggs; ++ai) {
            const AggSpec& a = plan.aggregates[ai];
            if (a.kind == AggSpec::Kind::Count) continue;
            const ExprNode& en = plan.exprs.nodes[a.expr];
            simdVecTypes[ai] = vecTypeForCol(en.valueType);
            simdInitVecs[ai] = b.build_vload(simdVecTypes[ai], colPtrs[static_cast<size_t>(en.resolvedCol)], 0);
        }

        Value* vRow = b.add_block_param(simdHdr, Type::i32());
        std::vector<Value*> vAccs(nAggs, nullptr);
        std::vector<Value*> hdrInitArgs = {b.build_iconst_i32(iLanes)};
        for (size_t ai = 0; ai < nAggs; ++ai) {
            if (plan.aggregates[ai].kind != AggSpec::Kind::Count) {
                vAccs[ai] = b.add_block_param(simdHdr, simdVecTypes[ai]);
                hdrInitArgs.push_back(simdInitVecs[ai]);
            }
        }
        b.build_br(simdHdr, hdrInitArgs);

        b.position_at_end(simdHdr);
        b.build_br_if(b.build_slt(vRow, simdEnd), simdBody, {}, simdMerge, {});

        b.position_at_end(simdBody);
        Value* byteOff = b.build_mul(b.build_zext_i64(vRow), b.build_iconst_i64(simdLaneBytes));
        std::vector<Value*> nextVecArgs = {b.build_add(vRow, b.build_iconst_i32(iLanes))};
        for (size_t ai = 0; ai < nAggs; ++ai) {
            const AggSpec& a = plan.aggregates[ai];
            if (a.kind == AggSpec::Kind::Count) continue;
            const size_t c = static_cast<size_t>(plan.exprs.nodes[a.expr].resolvedCol);
            Value* vecVal = b.build_vload(simdVecTypes[ai], b.build_add(colPtrs[c], byteOff), 0);
            if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
                nextVecArgs.push_back(b.build_vadd(vAccs[ai], vecVal));
            } else if (a.kind == AggSpec::Kind::Min) {
                nextVecArgs.push_back(b.build_vmin(vAccs[ai], vecVal));
            } else {
                nextVecArgs.push_back(b.build_vmax(vAccs[ai], vecVal));
            }
        }
        b.build_br(simdHdr, nextVecArgs);

        b.position_at_end(simdMerge);
        std::vector<Value*> tailArgs = {simdEnd, simdEnd};
        for (size_t ai = 0; ai < nAggs; ++ai) {
            const AggSpec& a = plan.aggregates[ai];
            if (a.kind == AggSpec::Kind::Count) {
                tailArgs.push_back(b.build_zext_i64(simdEnd));
                continue;
            }
            Value* accS = b.build_vextract_lane(vAccs[ai], 0);
            for (uint32_t ln = 1; ln < lanes; ++ln) {
                Value* laneV = b.build_vextract_lane(vAccs[ai], ln);
                if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
                    accS = b.build_add(accS, laneV);
                } else if (a.kind == AggSpec::Kind::Min) {
                    Value* lt = b.build_slt(laneV, accS);
                    accS = b.build_select(lt, laneV, accS);
                } else {
                    Value* gt = b.build_sgt(laneV, accS);
                    accS = b.build_select(gt, laneV, accS);
                }
            }
            const ColType srcColType = plan.exprs.nodes[a.expr].valueType;
            tailArgs.push_back(castMirVal(b, MirVal{accS, srcColType}, accScalarTypes[ai]).val);
        }
        b.build_br(scalarHdr, tailArgs);
    } else {
        std::vector<Value*> entryArgs = {b.build_iconst_i32(0), b.build_iconst_i32(0)};
        entryArgs.insert(entryArgs.end(), initAccs.begin(), initAccs.end());
        b.build_br(scalarHdr, entryArgs);
    }

    b.position_at_end(scalarHdr);
    std::vector<Value*> exitArgs = {mc};
    exitArgs.insert(exitArgs.end(), sAccs.begin(), sAccs.end());
    b.build_br_if(b.build_slt(row, rowCount), aliveChk, {}, loopExit, exitArgs);

    b.position_at_end(aliveChk);
    Value* row64 = b.build_zext_i64(row);
    std::vector<Value*> skipArgs = {mc};
    skipArgs.insert(skipArgs.end(), sAccs.begin(), sAccs.end());
    BasicBlock* firstStep = filterBlocks.empty() ? onMatch : filterBlocks[0];
    b.build_br_if(b.build_ne(hasAliveMask, b.build_iconst_i32(0)),
                  aliveLoad, {}, firstStep, {});

    b.position_at_end(aliveLoad);
    Value* wordIdx = b.build_lshr(row64, b.build_iconst_i64(6));
    Value* word = b.build_load_indexed(Type::i64(), aliveWords, wordIdx, 8, 0);
    Value* bit = b.build_and(row64, b.build_iconst_i64(63));
    Value* isAlive = b.build_trunc_i32(b.build_and(b.build_lshr(word, bit), b.build_iconst_i64(1)));
    b.build_br_if(isAlive, firstStep, {}, loopLatch, skipArgs);

    ExprEmitter emitter(b, fn, plan, colPtrs, row64);
    for (size_t fi = 0; fi < plan.filters.size(); ++fi) {
        b.position_at_end(filterBlocks[fi]);
        MirVal pred = emitter.emit(plan.filters[fi]);
        BasicBlock* nextBb = (fi + 1 < filterBlocks.size()) ? filterBlocks[fi + 1] : onMatch;
        b.build_br_if(buildTruthy(b, pred), nextBb, {}, loopLatch, skipArgs);
    }

    b.position_at_end(onMatch);
    Value* isFirst = b.build_eq(mc, b.build_iconst_i32(0));
    std::vector<Value*> nextMatchArgs = {b.build_add(mc, b.build_iconst_i32(1))};
    for (size_t ai = 0; ai < nAggs; ++ai) {
        const AggSpec& a = plan.aggregates[ai];
        if (a.kind == AggSpec::Kind::Count) {
            nextMatchArgs.push_back(b.build_add(sAccs[ai], b.build_iconst_i64(1)));
            continue;
        }
        MirVal rawV = emitter.emit(a.expr);
        MirVal v = castMirVal(b, rawV, accScalarTypes[ai]);
        if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
            nextMatchArgs.push_back(b.build_add(sAccs[ai], v.val));
        } else if (a.kind == AggSpec::Kind::Min) {
            Value* cmp = isUnsignedIntLike(accScalarTypes[ai])
                             ? b.build_ult(v.val, sAccs[ai])
                             : b.build_slt(v.val, sAccs[ai]);
            nextMatchArgs.push_back(b.build_select(b.build_or(isFirst, cmp), v.val, sAccs[ai]));
        } else {
            Value* cmp = isUnsignedIntLike(accScalarTypes[ai])
                             ? b.build_ugt(v.val, sAccs[ai])
                             : b.build_sgt(v.val, sAccs[ai]);
            nextMatchArgs.push_back(b.build_select(b.build_or(isFirst, cmp), v.val, sAccs[ai]));
        }
    }
    b.build_br(loopLatch, nextMatchArgs);

    b.position_at_end(loopLatch);
    std::vector<Value*> hdrNextArgs = {b.build_add(row, b.build_iconst_i32(1)), latchMc};
    hdrNextArgs.insert(hdrNextArgs.end(), lAccs.begin(), lAccs.end());
    b.build_br(scalarHdr, hdrNextArgs);

    b.position_at_end(loopExit);
    b.build_store(Type::i32(), ctxPtr, static_cast<int32_t>(offsetof(JitAggContext, matchCount)), finalMc);
    for (size_t ai = 0; ai < nAggs; ++ai) {
        const AggSpec& a = plan.aggregates[ai];
        if (a.kind == AggSpec::Kind::Count) {
            b.build_store(Type::i64(), ctxPtr,
                          static_cast<int32_t>(offsetof(JitAggContext, sumI64) + ai * sizeof(int64_t)),
                          fAccs[ai]);
        } else if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
            if (accScalarTypes[ai] == ColType::F64) {
                b.build_store(Type::f64(), ctxPtr,
                              static_cast<int32_t>(offsetof(JitAggContext, sumF64) + ai * sizeof(double)),
                              fAccs[ai]);
            } else {
                b.build_store(Type::i64(), ctxPtr,
                              static_cast<int32_t>(offsetof(JitAggContext, sumI64) + ai * sizeof(int64_t)),
                              fAccs[ai]);
            }
        } else {
            const size_t baseOff = (a.kind == AggSpec::Kind::Min)
                                       ? (colTypeIsFloat(accScalarTypes[ai])
                                              ? offsetof(JitAggContext, minF64)
                                              : offsetof(JitAggContext, minI64))
                                       : (colTypeIsFloat(accScalarTypes[ai])
                                              ? offsetof(JitAggContext, maxF64)
                                              : offsetof(JitAggContext, maxI64));
            if (colTypeIsFloat(accScalarTypes[ai])) {
                MirVal wide = castMirVal(b, MirVal{fAccs[ai], accScalarTypes[ai]}, ColType::F64);
                b.build_store(Type::f64(), ctxPtr, static_cast<int32_t>(baseOff + ai * sizeof(double)),
                              wide.val);
            } else {
                MirVal wide = castMirVal(b, MirVal{fAccs[ai], accScalarTypes[ai]}, ColType::I64);
                b.build_store(Type::i64(), ctxPtr, static_cast<int32_t>(baseOff + ai * sizeof(int64_t)),
                              wide.val);
            }
        }
    }
    b.build_ret_void();

    fn->rebuild_cfg_predecessors();

    brass::codegen::KernelOptions opts;
    opts.enable_superopt = false;
    opts.enable_smt_guard_elim = false;
    opts.enable_vectorize = false;
    opts.enable_slp = false;
    opts.enable_parallel = false;
    try {
        brass::codegen::KernelJit jit(opts);
        out->func = jit.compile(mod, "table_jit_agg");
        out->entry = reinterpret_cast<JitAggFnPtr>(out->func.entry_point());
    } catch (const std::exception& ex) {
        if (outError) *outError = ex.what();
        return nullptr;
    }
    return out->entry ? out : nullptr;
}

std::shared_ptr<Table> runCompiledAgg(
    const CompiledAggKernel& kernel, const QueryPlan& plan,
    const TableSnapshot& snap, bool useParallel) {
    const size_t nChunks = snap.chunks.size();
    const size_t nCols = snap.schema.size();
    const size_t nAggs = plan.aggregates.size();
    std::vector<JitAggContext> contexts(nChunks);

    auto runChunk = [&](size_t ci) {
        const auto& ch = snap.chunks[ci];
        if (ch.aliveCount == 0 || ch.rowCount == 0) return;
        std::vector<const void*> colPtrs(nCols);
        for (size_t c = 0; c < nCols; ++c) colPtrs[c] = ch.columns[c]->rawData();
        JitAggContext& ctx = contexts[ci];
        ctx.leftColPtrs = colPtrs.data();
        ctx.aliveWords = ch.aliveWords.data();
        ctx.rowCount = ch.rowCount;
        ctx.hasAliveMask = ch.allAlive() ? 0u : 1u;
        ctx.matchCount = 0;
        kernel.entry(&ctx);
    };

    if (useParallel && nChunks > 1) {
        brass::codegen::parallel_for(
            0, static_cast<int64_t>(nChunks),
            [&](int64_t s, int64_t e) {
                for (int64_t ci = s; ci < e; ++ci) runChunk(static_cast<size_t>(ci));
            },
            1);
    } else {
        for (size_t ci = 0; ci < nChunks; ++ci) runChunk(ci);
    }

    uint64_t totalMatch = 0;
    double sumF[kMaxJitAggs]{};
    int64_t sumI[kMaxJitAggs]{};
    double minF[kMaxJitAggs]{};
    double maxF[kMaxJitAggs]{};
    int64_t minI[kMaxJitAggs]{};
    int64_t maxI[kMaxJitAggs]{};
    bool hasExt = false;

    for (size_t ci = 0; ci < nChunks; ++ci) {
        const JitAggContext& c = contexts[ci];
        if (c.matchCount == 0) continue;
        totalMatch += c.matchCount;
        for (size_t ai = 0; ai < nAggs; ++ai) {
            const AggSpec& a = plan.aggregates[ai];
            if (a.kind == AggSpec::Kind::Count) {
                sumI[ai] += c.sumI64[ai];
            } else if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
                if (a.outputType == ColType::F64) sumF[ai] += c.sumF64[ai];
                else sumI[ai] += c.sumI64[ai];
            } else if (a.kind == AggSpec::Kind::Min) {
                if (colTypeIsFloat(a.outputType)) {
                    minF[ai] = !hasExt ? c.minF64[ai] : std::min(minF[ai], c.minF64[ai]);
                } else {
                    minI[ai] = !hasExt ? c.minI64[ai] : std::min(minI[ai], c.minI64[ai]);
                }
            } else {
                if (colTypeIsFloat(a.outputType)) {
                    maxF[ai] = !hasExt ? c.maxF64[ai] : std::max(maxF[ai], c.maxF64[ai]);
                } else {
                    maxI[ai] = !hasExt ? c.maxI64[ai] : std::max(maxI[ai], c.maxI64[ai]);
                }
            }
        }
        hasExt = true;
    }

    std::vector<ColumnDef> outSchema;
    outSchema.reserve(nAggs);
    std::vector<ScalarValue> row(nAggs);
    for (size_t ai = 0; ai < nAggs; ++ai) {
        const AggSpec& a = plan.aggregates[ai];
        std::string name = !a.alias.empty() ? a.alias : ("agg_" + std::to_string(ai));
        outSchema.push_back(ColumnDef{std::move(name), a.outputType});
        if (a.kind == AggSpec::Kind::Count) {
            row[ai] = ScalarValue::makeI64(sumI[ai]);
        } else if (a.kind == AggSpec::Kind::Sum) {
            row[ai] = (a.outputType == ColType::F64) ? ScalarValue::makeF64(sumF[ai])
                                                     : ScalarValue::makeI64(sumI[ai]);
        } else if (a.kind == AggSpec::Kind::Avg) {
            double total = (a.expr != kInvalidExpr && plan.exprs.nodes[a.expr].valueType != ColType::String &&
                            colTypeIsFloat(plan.exprs.nodes[a.expr].valueType))
                               ? sumF[ai]
                               : ((a.outputType == ColType::F64) ? sumF[ai] : static_cast<double>(sumI[ai]));
            row[ai] = ScalarValue::makeF64(totalMatch > 0 ? (total / static_cast<double>(totalMatch)) : 0.0);
        } else if (!hasExt) {
            row[ai] = ScalarValue::makeNull();
        } else {
            const bool isMin = (a.kind == AggSpec::Kind::Min);
            switch (a.outputType) {
            case ColType::F32: row[ai] = ScalarValue::makeF32(static_cast<float>(isMin ? minF[ai] : maxF[ai])); break;
            case ColType::F64: row[ai] = ScalarValue::makeF64(isMin ? minF[ai] : maxF[ai]); break;
            case ColType::I32: row[ai] = ScalarValue::makeI32(static_cast<int32_t>(isMin ? minI[ai] : maxI[ai])); break;
            case ColType::U32: row[ai] = ScalarValue::makeU32(static_cast<uint32_t>(isMin ? minI[ai] : maxI[ai])); break;
            case ColType::Bool: row[ai] = ScalarValue::makeBool((isMin ? minI[ai] : maxI[ai]) != 0); break;
            default: row[ai] = ScalarValue::makeI64(isMin ? minI[ai] : maxI[ai]); break;
            }
        }
    }

    auto outTable = std::make_shared<Table>(std::move(outSchema));
    outTable->appendRow(row);
    return outTable;
}

}  // namespace bro::table
