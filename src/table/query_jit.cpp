#include "table/query_jit.h"

#include "brass/mir/builder.hpp"
#include "brass/mir/function.hpp"
#include "brass/mir/module.hpp"

#include <cstddef>
#include <cstring>
#include <exception>

namespace bro::table {

namespace {

using namespace brass;

struct MirVal {
    Value* val = nullptr;
    ColType type = ColType::I32;
};

Type mirStorageType(ColType t) {
    switch (t) {
    case ColType::I32:
    case ColType::U32:
    case ColType::Dict:
    case ColType::Bool:
        return Type::i32();
    case ColType::I64:
        return Type::i64();
    case ColType::F32:
        return Type::f32();
    case ColType::F64:
        return Type::f64();
    case ColType::String:
        return Type::ptr();
    }
    return Type::i32();
}

uint8_t colScale(ColType t) {
    return static_cast<uint8_t>(colTypeElemSize(t));
}

bool is32IntLike(ColType t) {
    return t == ColType::I32 || t == ColType::U32 || t == ColType::Dict || t == ColType::Bool;
}

bool isUnsignedIntLike(ColType t) {
    return t == ColType::U32 || t == ColType::Dict || t == ColType::Bool;
}

Value* buildTruthy(Builder& b, MirVal v) {
    switch (v.type) {
    case ColType::Bool:
        return b.build_ne(v.val, b.build_iconst_i32(0));
    case ColType::I32:
    case ColType::U32:
    case ColType::Dict:
        return b.build_ne(v.val, b.build_iconst_i32(0));
    case ColType::I64:
        return b.build_ne(v.val, b.build_iconst_i64(0));
    case ColType::F32: {
        Value* ordered = b.build_eq(v.val, v.val);
        Value* nz = b.build_ne(v.val, b.build_fconst_f32(0.0f));
        return b.build_and(ordered, nz);
    }
    case ColType::F64: {
        Value* ordered = b.build_eq(v.val, v.val);
        Value* nz = b.build_ne(v.val, b.build_fconst_f64(0.0));
        return b.build_and(ordered, nz);
    }
    case ColType::String:
        return b.build_iconst_i32(0);
    }
    return b.build_iconst_i32(0);
}

MirVal castMirVal(Builder& b, MirVal src, ColType dst) {
    if (src.type == dst) return src;
    if (dst == ColType::Bool) {
        return MirVal{buildTruthy(b, src), ColType::Bool};
    }

    // Float -> ...
    if (colTypeIsFloat(src.type)) {
        if (dst == ColType::F32) {
            return MirVal{src.type == ColType::F64 ? b.build_fptrunc_f32_f64(src.val) : src.val,
                          ColType::F32};
        }
        if (dst == ColType::F64) {
            return MirVal{src.type == ColType::F32 ? b.build_fpext_f64_f32(src.val) : src.val,
                          ColType::F64};
        }
        if (dst == ColType::I64) {
            return MirVal{src.type == ColType::F32 ? b.build_fptosi_i64_f32(src.val)
                                                   : b.build_fptosi_i64(src.val),
                          ColType::I64};
        }
        // -> 32-bit int
        if (dst == ColType::I32) {
            return MirVal{src.type == ColType::F32 ? b.build_fptosi_i32_f32(src.val)
                                                   : b.build_fptosi_i32(src.val),
                          ColType::I32};
        }
        // -> U32/Dict via i64 trunc
        Value* i64v = (src.type == ColType::F32) ? b.build_fptosi_i64_f32(src.val)
                                                 : b.build_fptosi_i64(src.val);
        return MirVal{b.build_trunc_i32(i64v), dst};
    }

    // Int -> Float
    if (colTypeIsFloat(dst)) {
        if (src.type == ColType::I64) {
            return MirVal{dst == ColType::F32 ? b.build_sitofp_f32_i64(src.val)
                                              : b.build_sitofp_f64_i64(src.val),
                          dst};
        }
        if (isUnsignedIntLike(src.type)) {
            Value* z64 = b.build_zext_i64(src.val);
            return MirVal{dst == ColType::F32 ? b.build_sitofp_f32_i64(z64)
                                              : b.build_sitofp_f64_i64(z64),
                          dst};
        }
        return MirVal{dst == ColType::F32 ? b.build_sitofp_f32_i32(src.val)
                                          : b.build_sitofp_f64_i32(src.val),
                      dst};
    }

    // Int -> Int
    if (dst == ColType::I64) {
        Value* v64 = isUnsignedIntLike(src.type) ? b.build_zext_i64(src.val)
                                                 : b.build_sext_i64(src.val);
        return MirVal{v64, ColType::I64};
    }
    if (src.type == ColType::I64 && is32IntLike(dst)) {
        return MirVal{b.build_trunc_i32(src.val), dst};
    }
    return MirVal{src.val, dst};
}

BasicBlock* createAttachedBlock(Function* fn, Builder& b, std::string_view name) {
    BasicBlock* bb = b.create_block(name);
    if (fn) fn->append_block(bb);
    return bb;
}

class ExprEmitter {
public:
    ExprEmitter(Builder& b, Function* fn, const QueryPlan& plan,
                const std::vector<Value*>& colPtrs, Value* rowIdx64)
        : b_(b), fn_(fn), plan_(plan), colPtrs_(colPtrs), rowIdx64_(rowIdx64) {}

    MirVal emit(uint32_t exprId) {
        const ExprNode& n = plan_.exprs.nodes[exprId];
        switch (n.op) {
        case ExprOp::ColRef: {
            const size_t col = static_cast<size_t>(n.resolvedCol);
            Value* ptr = colPtrs_[col];
            if (n.valueType == ColType::Bool) {
                Value* raw = b_.build_load_indexed(Type::i8(), ptr, rowIdx64_, 1, 0);
                Value* z = b_.build_trunc_i32(b_.build_zext_i64(raw));
                return MirVal{b_.build_ne(z, b_.build_iconst_i32(0)), ColType::Bool};
            }
            return MirVal{b_.build_load_indexed(mirStorageType(n.valueType), ptr, rowIdx64_,
                                                colScale(n.valueType), 0),
                          n.valueType};
        }
        case ExprOp::Const:
            return emitConst(n.literal);

        case ExprOp::Neg: {
            MirVal v = castMirVal(b_, emit(n.lhs), n.valueType);
            return MirVal{b_.build_neg(v.val), n.valueType};
        }
        case ExprOp::Abs: {
            MirVal v = castMirVal(b_, emit(n.lhs), n.valueType);
            if (n.valueType == ColType::F32) return MirVal{b_.build_fabs_f32(v.val), ColType::F32};
            if (n.valueType == ColType::F64) return MirVal{b_.build_fabs_f64(v.val), ColType::F64};
            Value* zero = (n.valueType == ColType::I64) ? b_.build_iconst_i64(0)
                                                        : b_.build_iconst_i32(0);
            Value* isNeg = b_.build_slt(v.val, zero);
            return MirVal{b_.build_select(isNeg, b_.build_neg(v.val), v.val), n.valueType};
        }
        case ExprOp::Sqrt: {
            MirVal v = castMirVal(b_, emit(n.lhs), n.valueType);
            return MirVal{n.valueType == ColType::F32 ? b_.build_sqrt_f32(v.val)
                                                      : b_.build_sqrt_f64(v.val),
                          n.valueType};
        }
        case ExprOp::Floor: {
            MirVal v = castMirVal(b_, emit(n.lhs), n.valueType);
            return MirVal{n.valueType == ColType::F32 ? b_.build_floor_f32(v.val)
                                                      : b_.build_floor_f64(v.val),
                          n.valueType};
        }
        case ExprOp::Ceil: {
            MirVal v = castMirVal(b_, emit(n.lhs), n.valueType);
            return MirVal{n.valueType == ColType::F32 ? b_.build_ceil_f32(v.val)
                                                      : b_.build_ceil_f64(v.val),
                          n.valueType};
        }
        case ExprOp::BitNot: {
            MirVal v = castMirVal(b_, emit(n.lhs), n.valueType);
            return MirVal{b_.build_not(v.val), n.valueType};
        }
        case ExprOp::Not: {
            MirVal v = emit(n.lhs);
            return MirVal{b_.build_xor(buildTruthy(b_, v), b_.build_iconst_i32(1)), ColType::Bool};
        }

        case ExprOp::Add:
        case ExprOp::Sub:
        case ExprOp::Mul:
        case ExprOp::Div:
        case ExprOp::Mod: {
            MirVal a = castMirVal(b_, emit(n.lhs), n.valueType);
            MirVal r = castMirVal(b_, emit(n.rhs), n.valueType);
            if (n.op == ExprOp::Add) return MirVal{b_.build_add(a.val, r.val), n.valueType};
            if (n.op == ExprOp::Sub) return MirVal{b_.build_sub(a.val, r.val), n.valueType};
            if (n.op == ExprOp::Mul) return MirVal{b_.build_mul(a.val, r.val), n.valueType};
            if (colTypeIsFloat(n.valueType)) {
                return MirVal{b_.build_sdiv(a.val, r.val), n.valueType};
            }
            return emitSafeIntDivMod(n.op == ExprOp::Div, n.valueType, a.val, r.val);
        }

        case ExprOp::BitAnd:
        case ExprOp::BitOr:
        case ExprOp::BitXor:
        case ExprOp::Shl:
        case ExprOp::Shr:
        case ExprOp::UShr: {
            MirVal a = castMirVal(b_, emit(n.lhs), n.valueType);
            MirVal r = castMirVal(b_, emit(n.rhs), n.valueType);
            if (n.op == ExprOp::BitAnd) return MirVal{b_.build_and(a.val, r.val), n.valueType};
            if (n.op == ExprOp::BitOr) return MirVal{b_.build_or(a.val, r.val), n.valueType};
            if (n.op == ExprOp::BitXor) return MirVal{b_.build_xor(a.val, r.val), n.valueType};
            Value* mask = (n.valueType == ColType::I64) ? b_.build_iconst_i64(63)
                                                        : b_.build_iconst_i32(31);
            Value* sh = b_.build_and(r.val, mask);
            if (n.op == ExprOp::Shl) return MirVal{b_.build_shl(a.val, sh), n.valueType};
            if (n.op == ExprOp::Shr) return MirVal{b_.build_ashr(a.val, sh), n.valueType};
            return MirVal{b_.build_lshr(a.val, sh), n.valueType};
        }

        case ExprOp::StrEq:
        case ExprOp::StrNe: {
            const ExprNode& ln = plan_.exprs.nodes[n.lhs];
            const ExprNode& rn = plan_.exprs.nodes[n.rhs];
            MirVal dictVal = emit(n.lhs);
            Value* idConst = b_.build_iconst_i32(static_cast<int32_t>(rn.cachedDictId));
            (void)ln;
            Value* cmp = (n.op == ExprOp::StrEq) ? b_.build_eq(dictVal.val, idConst)
                                                 : b_.build_ne(dictVal.val, idConst);
            return MirVal{cmp, ColType::Bool};
        }

        case ExprOp::Eq:
        case ExprOp::Ne:
        case ExprOp::Lt:
        case ExprOp::Le:
        case ExprOp::Gt:
        case ExprOp::Ge: {
            const ExprNode& ln = plan_.exprs.nodes[n.lhs];
            const ExprNode& rn = plan_.exprs.nodes[n.rhs];
            if (ln.valueType == ColType::Dict && rn.op == ExprOp::Const &&
                rn.literal.type == ColType::String) {
                MirVal lv = emit(n.lhs);
                Value* idConst = b_.build_iconst_i32(static_cast<int32_t>(rn.cachedDictId));
                Value* cmp = (n.op == ExprOp::Eq) ? b_.build_eq(lv.val, idConst)
                                                  : b_.build_ne(lv.val, idConst);
                return MirVal{cmp, ColType::Bool};
            }
            if (rn.valueType == ColType::Dict && ln.op == ExprOp::Const &&
                ln.literal.type == ColType::String) {
                MirVal rv = emit(n.rhs);
                Value* idConst = b_.build_iconst_i32(static_cast<int32_t>(ln.cachedDictId));
                Value* cmp = (n.op == ExprOp::Eq) ? b_.build_eq(rv.val, idConst)
                                                  : b_.build_ne(rv.val, idConst);
                return MirVal{cmp, ColType::Bool};
            }
            ColType cmpType = ln.valueType;
            if (ln.valueType != rn.valueType) {
                if (colTypeIsFloat(ln.valueType) || colTypeIsFloat(rn.valueType)) {
                    cmpType = ColType::F64;
                } else {
                    cmpType = promoteNumericType(ln.valueType, rn.valueType);
                }
            }
            MirVal a = castMirVal(b_, emit(n.lhs), cmpType);
            MirVal r = castMirVal(b_, emit(n.rhs), cmpType);
            const bool unsignedCmp = isUnsignedIntLike(cmpType);
            Value* cmp = nullptr;
            switch (n.op) {
            case ExprOp::Eq: cmp = b_.build_eq(a.val, r.val); break;
            case ExprOp::Ne: cmp = b_.build_ne(a.val, r.val); break;
            case ExprOp::Lt: cmp = unsignedCmp ? b_.build_ult(a.val, r.val) : b_.build_slt(a.val, r.val); break;
            case ExprOp::Le: cmp = unsignedCmp ? b_.build_ule(a.val, r.val) : b_.build_sle(a.val, r.val); break;
            case ExprOp::Gt: cmp = unsignedCmp ? b_.build_ugt(a.val, r.val) : b_.build_sgt(a.val, r.val); break;
            case ExprOp::Ge: cmp = unsignedCmp ? b_.build_uge(a.val, r.val) : b_.build_sge(a.val, r.val); break;
            default: cmp = b_.build_iconst_i32(0); break;
            }
            return MirVal{cmp, ColType::Bool};
        }

        case ExprOp::And: {
            Value* a = buildTruthy(b_, emit(n.lhs));
            Value* r = buildTruthy(b_, emit(n.rhs));
            return MirVal{b_.build_and(a, r), ColType::Bool};
        }
        case ExprOp::Or: {
            Value* a = buildTruthy(b_, emit(n.lhs));
            Value* r = buildTruthy(b_, emit(n.rhs));
            return MirVal{b_.build_or(a, r), ColType::Bool};
        }
        case ExprOp::Cond: {
            Value* c = buildTruthy(b_, emit(n.lhs));
            MirVal t = castMirVal(b_, emit(n.rhs), n.valueType);
            MirVal e = castMirVal(b_, emit(n.third), n.valueType);
            return MirVal{b_.build_select(c, t.val, e.val), n.valueType};
        }
        default:
            return MirVal{b_.build_iconst_i32(0), ColType::I32};
        }
    }

private:
    MirVal emitConst(const ScalarValue& s) {
        switch (s.type) {
        case ColType::I32:
            return MirVal{b_.build_iconst_i32(static_cast<int32_t>(s.i64)), ColType::I32};
        case ColType::U32:
            return MirVal{b_.build_iconst_i32(static_cast<int32_t>(static_cast<uint32_t>(s.u64))),
                          ColType::U32};
        case ColType::Dict:
            return MirVal{b_.build_iconst_i32(static_cast<int32_t>(static_cast<uint32_t>(s.u64))),
                          ColType::Dict};
        case ColType::I64:
            return MirVal{b_.build_iconst_i64(s.i64), ColType::I64};
        case ColType::F32:
            return MirVal{b_.build_fconst_f32(static_cast<float>(s.f64)), ColType::F32};
        case ColType::F64:
            return MirVal{b_.build_fconst_f64(s.f64), ColType::F64};
        case ColType::Bool:
            return MirVal{b_.build_iconst_i32((s.i64 != 0 || s.u64 != 0 || s.f64 != 0.0) ? 1 : 0),
                          ColType::Bool};
        case ColType::String:
            return MirVal{b_.build_iconst_i32(0), ColType::I32};
        }
        return MirVal{b_.build_iconst_i32(0), ColType::I32};
    }

    MirVal emitSafeIntDivMod(bool isDiv, ColType t, Value* lhs, Value* rhs) {
        const bool is64 = (t == ColType::I64);
        const bool isSigned = (t == ColType::I32 || t == ColType::I64);
        Value* zero = is64 ? b_.build_iconst_i64(0) : b_.build_iconst_i32(0);
        Value* isZero = b_.build_eq(rhs, zero);
        Value* isUnsafe = isZero;
        Value* unsafeResult = zero;

        if (isSigned) {
            Value* minV = is64 ? b_.build_iconst_i64(INT64_MIN) : b_.build_iconst_i32(INT32_MIN);
            Value* neg1 = is64 ? b_.build_iconst_i64(-1) : b_.build_iconst_i32(-1);
            Value* isOvf = b_.build_and(b_.build_eq(lhs, minV), b_.build_eq(rhs, neg1));
            isUnsafe = b_.build_or(isZero, isOvf);
            if (isDiv) unsafeResult = b_.build_select(isOvf, minV, zero);
        }

        const int id = divCounter_++;
        BasicBlock* unsafeBb = createAttachedBlock(fn_, b_, "div_unsafe_" + std::to_string(id));
        BasicBlock* safeBb = createAttachedBlock(fn_, b_, "div_safe_" + std::to_string(id));
        BasicBlock* mergeBb = createAttachedBlock(fn_, b_, "div_merge_" + std::to_string(id));
        Value* mergedVal = b_.add_block_param(mergeBb, is64 ? Type::i64() : Type::i32());

        b_.build_br_if(isUnsafe, unsafeBb, {}, safeBb, {});

        b_.position_at_end(unsafeBb);
        b_.build_br(mergeBb, {unsafeResult});

        b_.position_at_end(safeBb);
        Value* q = nullptr;
        if (isDiv) q = isSigned ? b_.build_sdiv(lhs, rhs) : b_.build_udiv(lhs, rhs);
        else q = isSigned ? b_.build_smod(lhs, rhs) : b_.build_umod(lhs, rhs);
        b_.build_br(mergeBb, {q});

        b_.position_at_end(mergeBb);
        return MirVal{mergedVal, t};
    }

    Builder& b_;
    Function* fn_;
    const QueryPlan& plan_;
    const std::vector<Value*>& colPtrs_;
    Value* rowIdx64_;
    int divCounter_ = 0;
};

void storeTypedOutput(Builder& b, MirVal v, ColType dstType, Value* dstPtr, Value* idx64) {
    if (dstType == ColType::Bool) {
        Value* cond = buildTruthy(b, v);
        b.build_store_indexed(Type::i8(), dstPtr, idx64, 1, 0, b.build_trunc_i8(cond));
        return;
    }
    MirVal castV = castMirVal(b, v, dstType);
    b.build_store_indexed(mirStorageType(dstType), dstPtr, idx64, colScale(dstType), 0, castV.val);
}

void collectUsedCols(const ExprArena& arena, uint32_t id, std::vector<bool>& used) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) return;
    const ExprNode& n = arena.nodes[id];
    if (n.op == ExprOp::ColRef && n.tableSide == 0 && n.resolvedCol >= 0 &&
        static_cast<size_t>(n.resolvedCol) < used.size()) {
        used[static_cast<size_t>(n.resolvedCol)] = true;
    }
    collectUsedCols(arena, n.lhs, used);
    collectUsedCols(arena, n.rhs, used);
    collectUsedCols(arena, n.third, used);
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

    try {
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
    const char* modeTag = (requestedMode == JitMode::ScatterUpdate)
                              ? "U"
                              : (requestedMode == JitMode::FilterAndProject ? "FP" : "F");
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
    std::shared_ptr<CompiledKernel> compiled =
        compileKernelImpl(plan, snap, requestedMode, outError);
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

JitCacheStats CompiledPlanCache::stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return JitCacheStats{
        hits_.load(std::memory_order_relaxed),
        misses_.load(std::memory_order_relaxed),
        entries_.size(),
    };
}

void CompiledPlanCache::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_.clear();
    fifoOrder_.clear();
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
}

}  // namespace bro::table
