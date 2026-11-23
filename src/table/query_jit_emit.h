#pragma once

#include "brass/mir/builder.hpp"
#include "brass/mir/function.hpp"
#include "brass/mir/module.hpp"
#include "table/query_ir.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::table::jit_detail {

using namespace brass;

struct MirVal {
    Value* val = nullptr;
    ColType type = ColType::I32;
};

inline Type mirStorageType(ColType t) {
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

inline uint8_t colScale(ColType t) {
    return static_cast<uint8_t>(colTypeElemSize(t));
}

inline bool is32IntLike(ColType t) {
    return t == ColType::I32 || t == ColType::U32 || t == ColType::Dict || t == ColType::Bool;
}

inline bool isUnsignedIntLike(ColType t) {
    return t == ColType::U32 || t == ColType::Dict || t == ColType::Bool;
}

inline Value* buildTruthy(Builder& b, MirVal v) {
    switch (v.type) {
    case ColType::Bool:
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

inline MirVal castMirVal(Builder& b, MirVal src, ColType dst) {
    if (src.type == dst) return src;
    if (dst == ColType::Bool) {
        return MirVal{buildTruthy(b, src), ColType::Bool};
    }
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
        if (dst == ColType::I32) {
            return MirVal{src.type == ColType::F32 ? b.build_fptosi_i32_f32(src.val)
                                                   : b.build_fptosi_i32(src.val),
                          ColType::I32};
        }
        Value* i64v = (src.type == ColType::F32) ? b.build_fptosi_i64_f32(src.val)
                                                 : b.build_fptosi_i64(src.val);
        return MirVal{b.build_trunc_i32(i64v), dst};
    }
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

inline BasicBlock* createAttachedBlock(Function* fn, Builder& b, std::string_view name) {
    BasicBlock* bb = b.create_block(name);
    if (fn) fn->append_block(bb);
    return bb;
}

class ExprEmitter {
public:
    ExprEmitter(Builder& b, Function* fn, const QueryPlan& plan,
                const std::vector<Value*>& colPtrs, Value* rowIdx64)
        : b_(b), fn_(fn), plan_(plan), colPtrs_(colPtrs), rowIdx64_(rowIdx64) {}

    void setRowIdx64(Value* r64) { rowIdx64_ = r64; }

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
            if (colTypeIsFloat(n.valueType)) return MirVal{b_.build_sdiv(a.val, r.val), n.valueType};
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
            const ExprNode& rn = plan_.exprs.nodes[n.rhs];
            MirVal dictVal = emit(n.lhs);
            Value* idConst = b_.build_iconst_i32(static_cast<int32_t>(rn.cachedDictId));
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
                cmpType = (colTypeIsFloat(ln.valueType) || colTypeIsFloat(rn.valueType))
                              ? ColType::F64
                              : promoteNumericType(ln.valueType, rn.valueType);
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
        case ColType::I32: return MirVal{b_.build_iconst_i32(static_cast<int32_t>(s.i64)), ColType::I32};
        case ColType::U32: return MirVal{b_.build_iconst_i32(static_cast<int32_t>(static_cast<uint32_t>(s.u64))), ColType::U32};
        case ColType::Dict: return MirVal{b_.build_iconst_i32(static_cast<int32_t>(static_cast<uint32_t>(s.u64))), ColType::Dict};
        case ColType::I64: return MirVal{b_.build_iconst_i64(s.i64), ColType::I64};
        case ColType::F32: return MirVal{b_.build_fconst_f32(static_cast<float>(s.f64)), ColType::F32};
        case ColType::F64: return MirVal{b_.build_fconst_f64(s.f64), ColType::F64};
        case ColType::Bool: return MirVal{b_.build_iconst_i32((s.i64 != 0 || s.u64 != 0 || s.f64 != 0.0) ? 1 : 0), ColType::Bool};
        case ColType::String: return MirVal{b_.build_iconst_i32(0), ColType::I32};
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
        Value* q = isDiv ? (isSigned ? b_.build_sdiv(lhs, rhs) : b_.build_udiv(lhs, rhs))
                         : (isSigned ? b_.build_smod(lhs, rhs) : b_.build_umod(lhs, rhs));
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

inline void storeTypedOutput(Builder& b, MirVal v, ColType dstType, Value* dstPtr, Value* idx64) {
    if (dstType == ColType::Bool) {
        Value* cond = buildTruthy(b, v);
        b.build_store_indexed(Type::i8(), dstPtr, idx64, 1, 0, b.build_trunc_i8(cond));
        return;
    }
    MirVal castV = castMirVal(b, v, dstType);
    b.build_store_indexed(mirStorageType(dstType), dstPtr, idx64, colScale(dstType), 0, castV.val);
}

inline void collectUsedCols(const ExprArena& arena, uint32_t id, std::vector<bool>& used) {
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

}  // namespace bro::table::jit_detail
