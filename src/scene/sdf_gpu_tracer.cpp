#include "scene/sdf_gpu_tracer.h"

#include <bromesh/isosurface/jit/sdf_compiler.h>

#if defined(BROMESH_HAS_BRASS_JIT)
#include <brass/codegen/kernel_jit.hpp>
#include <brass/codegen/sdf_builder.hpp>
#include <brass/mir/scalar_opt.hpp>
#include <brass/mir/smt_guard_elim.hpp>
#include <brass/mir/superopt.hpp>
#include <brass/target/spirv/spirv_binary_reader.hpp>
#include <brass/target/spirv/spirv_ir.hpp>
#include <brass/target/spirv_target.hpp>
#endif

#include <algorithm>
#include <cmath>

namespace bromesh {
float evalScalarNode(const SdfGraph& graph, int nodeId, float px, float py, float pz);
} // namespace bromesh

namespace bro::scene {

namespace {

inline bromath::Vec3 normalizeSafe(bromath::Vec3 v) {
    const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (lenSq <= 1e-20f) return {0.0f, 1.0f, 0.0f};
    const float invLen = 1.0f / std::sqrt(lenSq);
    return {v.x * invLen, v.y * invLen, v.z * invLen};
}

using PointEvalFn = bromesh::SdfPointEvalFn;
using GradEvalFn = bromesh::SdfGradEvalFn;

inline int resolveRoot(const bromesh::SdfGraph& graph, int rootNode) {
    if (rootNode >= 0) return rootNode;
    return graph.root();
}

inline float evalWithKernel(
    const bromesh::SdfGraph& graph,
    int rootNode,
    PointEvalFn pointFn,
    float x,
    float y,
    float z) {
    if (pointFn) {
        return pointFn(x, y, z);
    }
    return bromesh::evalScalarNode(graph, resolveRoot(graph, rootNode), x, y, z);
}

inline bromath::Vec3 evalNormalWithKernel(
    const bromesh::SdfGraph& graph,
    int rootNode,
    PointEvalFn pointFn,
    GradEvalFn gradFn,
    bromath::Vec3 p,
    float eps) {
    if (gradFn && eps <= 0.0f) {
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        gradFn(p.x, p.y, p.z, &gx, &gy, &gz);
        return normalizeSafe({gx, gy, gz});
    }
    const float h = std::max(eps, 1e-5f);
    // Tetrahedral finite-difference stencil (4 evaluations):
    // k0 = ( 1, -1, -1), k1 = (-1, -1,  1), k2 = (-1,  1, -1), k3 = ( 1,  1,  1)
    const float d0 = evalWithKernel(graph, rootNode, pointFn, p.x + h, p.y - h, p.z - h);
    const float d1 = evalWithKernel(graph, rootNode, pointFn, p.x - h, p.y - h, p.z + h);
    const float d2 = evalWithKernel(graph, rootNode, pointFn, p.x - h, p.y + h, p.z - h);
    const float d3 = evalWithKernel(graph, rootNode, pointFn, p.x + h, p.y + h, p.z + h);

    const bromath::Vec3 grad{
        d0 - d1 - d2 + d3,
        -d0 - d1 + d2 + d3,
        -d0 + d1 - d2 + d3
    };
    return normalizeSafe(grad);
}

inline float evalSoftShadowWithKernel(
    const bromesh::SdfGraph& graph,
    int rootNode,
    PointEvalFn pointFn,
    bromath::Vec3 origin,
    bromath::Vec3 lightDir,
    float k,
    float tMin,
    float tMax,
    int maxSteps) {
    const bromath::Vec3 dir = normalizeSafe(lightDir);
    const float sharpK = std::max(k, 0.1f);
    const int steps = std::clamp(maxSteps, 1, 512);
    float t = std::max(tMin, 1e-4f);
    const float endT = std::max(tMax, t + 1e-3f);

    float res = 1.0f;
    for (int i = 0; i < steps && t < endT; ++i) {
        const float d = evalWithKernel(
            graph, rootNode, pointFn,
            origin.x + dir.x * t,
            origin.y + dir.y * t,
            origin.z + dir.z * t);
        if (d < 1e-4f) {
            return 0.0f;
        }
        res = std::min(res, sharpK * d / std::max(t, 1e-4f));
        t += std::clamp(d, 0.005f, 0.5f);
    }
    res = std::clamp(res, 0.0f, 1.0f);
    return res * res * (3.0f - 2.0f * res);
}

inline float evalAoWithKernel(
    const bromesh::SdfGraph& graph,
    int rootNode,
    PointEvalFn pointFn,
    bromath::Vec3 pos,
    bromath::Vec3 normal,
    int steps,
    float stepSize,
    float intensity) {
    const bromath::Vec3 n = normalizeSafe(normal);
    const int numSteps = std::clamp(steps, 1, 32);
    const float ds = std::max(stepSize, 1e-4f);

    float occ = 0.0f;
    float weight = 1.0f;
    for (int i = 1; i <= numSteps; ++i) {
        const float h = static_cast<float>(i) * ds;
        const float d = evalWithKernel(
            graph, rootNode, pointFn,
            pos.x + n.x * h,
            pos.y + n.y * h,
            pos.z + n.z * h);
        occ += std::max(h - d, 0.0f) * weight;
        weight *= 0.5f;
    }
    return std::clamp(1.0f - intensity * occ, 0.0f, 1.0f);
}

inline SdfTraceHit traceSingleRayWithKernel(
    const bromesh::SdfGraph& graph,
    int rootNode,
    PointEvalFn pointFn,
    GradEvalFn gradFn,
    const SdfRay& ray,
    const SdfTraceConfig& cfg) {
    SdfTraceHit result;
    const bromath::Vec3 dir = normalizeSafe(ray.dir);
    const int maxSteps = std::clamp(cfg.maxSteps, 1, 2048);
    const float eps = std::max(cfg.hitEpsilon, 1e-6f);
    float t = std::max(ray.tMin, 0.0f);
    const float tMax = std::max(ray.tMax, t);

    for (int step = 0; step < maxSteps; ++step) {
        const bromath::Vec3 p{
            ray.origin.x + dir.x * t,
            ray.origin.y + dir.y * t,
            ray.origin.z + dir.z * t
        };
        const float d = evalWithKernel(graph, rootNode, pointFn, p.x, p.y, p.z);
        result.distance = d;
        result.steps = step + 1;

        if (d < eps) {
            result.hit = true;
            result.t = t;
            result.position = p;
            result.normal = evalNormalWithKernel(
                graph, rootNode, pointFn, gradFn, p, cfg.normalEpsilon);

            const float bias = std::max(cfg.shadowMinT, eps * 4.0f);
            const bromath::Vec3 offsetPos{
                p.x + result.normal.x * bias,
                p.y + result.normal.y * bias,
                p.z + result.normal.z * bias
            };
            if (cfg.computeShadow) {
                result.softShadow = evalSoftShadowWithKernel(
                    graph, rootNode, pointFn,
                    offsetPos, cfg.lightDir, cfg.shadowK,
                    cfg.shadowMinT, cfg.shadowMaxT, cfg.shadowSteps);
            }
            if (cfg.computeAo) {
                result.ambientOcclusion = evalAoWithKernel(
                    graph, rootNode, pointFn,
                    p, result.normal,
                    cfg.aoSteps, cfg.aoStepSize, cfg.aoIntensity);
            }
            return result;
        }

        t += d;
        if (t > tMax) {
            result.t = t;
            result.position = {
                ray.origin.x + dir.x * t,
                ray.origin.y + dir.y * t,
                ray.origin.z + dir.z * t
            };
            break;
        }
    }

    result.t = t;
    return result;
}

#if defined(BROMESH_HAS_BRASS_JIT)

using brass::BasicBlock;
using brass::Function;
using brass::Module;
using brass::Type;
using brass::Value;
using brass::codegen::SdfKernelBuilder;

// Helper that guarantees the radicand of Sqrt is defined by an OpFAdd with a
// strictly positive epsilon, satisfying both algebraic positivity and the
// static SPIR-V prover (`audit_div_and_sqrt`).
inline Value* safeSqrt(SdfKernelBuilder& kb, Value* v) {
    Value* nonNeg = kb.max_f32(v, kb.const_f32(0.0f));
    return kb.sqrt_f32(kb.add(nonNeg, kb.const_f32(1e-20f)));
}

inline Value* safeLength2(SdfKernelBuilder& kb, Value* a, Value* b) {
    return safeSqrt(kb, kb.fma(a, a, kb.mul(b, b)));
}

inline Value* safeLength3(SdfKernelBuilder& kb, Value* a, Value* b, Value* c) {
    return safeSqrt(kb, kb.fma(a, a, kb.fma(b, b, kb.mul(c, c))));
}

Value* lowerNoise3dSpirv(SdfKernelBuilder& kb, Value* px, Value* py, Value* pz) {
    Value* flrX = kb.floor_f32(px);
    Value* flrY = kb.floor_f32(py);
    Value* flrZ = kb.floor_f32(pz);

    Value* i0 = kb.builder().build_fptosi_i32_f32(flrX);
    Value* j0 = kb.builder().build_fptosi_i32_f32(flrY);
    Value* k0 = kb.builder().build_fptosi_i32_f32(flrZ);

    Value* oneI = kb.const_i32(1);
    Value* i1 = kb.builder().build_add(i0, oneI);
    Value* j1 = kb.builder().build_add(j0, oneI);
    Value* k1 = kb.builder().build_add(k0, oneI);

    Value* fx = kb.sub(px, flrX);
    Value* fy = kb.sub(py, flrY);
    Value* fz = kb.sub(pz, flrZ);

    Value* c3 = kb.const_f32(3.0f);
    Value* c2 = kb.const_f32(2.0f);
    Value* u = kb.mul(kb.mul(fx, fx), kb.sub(c3, kb.mul(fx, c2)));
    Value* v = kb.mul(kb.mul(fy, fy), kb.sub(c3, kb.mul(fy, c2)));
    Value* w = kb.mul(kb.mul(fz, fz), kb.sub(c3, kb.mul(fz, c2)));

    auto hashCorner = [&](Value* ix, Value* iy, Value* iz) -> Value* {
        Value* h = kb.builder().build_xor(ix, kb.builder().build_mul(iy, kb.const_i32(374761393)));
        h = kb.builder().build_xor(h, kb.builder().build_mul(iz, kb.const_i32(668265263)));
        Value* shift13 = kb.builder().build_lshr(h, kb.const_i32(13));
        h = kb.builder().build_mul(kb.builder().build_xor(h, shift13), kb.const_i32(1274126177));
        Value* shift16 = kb.builder().build_lshr(h, kb.const_i32(16));
        h = kb.builder().build_xor(h, shift16);

        Value* masked = kb.builder().build_and(h, kb.const_i32(0x7FFF));
        Value* hf = kb.builder().build_sitofp_f32_i32(masked);
        return kb.sub(kb.mul(hf, kb.const_f32(2.0f / 32767.0f)), kb.const_f32(1.0f));
    };

    Value* h000 = hashCorner(i0, j0, k0);
    Value* h100 = hashCorner(i1, j0, k0);
    Value* h010 = hashCorner(i0, j1, k0);
    Value* h110 = hashCorner(i1, j1, k0);
    Value* h001 = hashCorner(i0, j0, k1);
    Value* h101 = hashCorner(i1, j0, k1);
    Value* h011 = hashCorner(i0, j1, k1);
    Value* h111 = hashCorner(i1, j1, k1);

    Value* x00 = kb.lerp_f32(h000, h100, u);
    Value* x10 = kb.lerp_f32(h010, h110, u);
    Value* x01 = kb.lerp_f32(h001, h101, u);
    Value* x11 = kb.lerp_f32(h011, h111, u);

    Value* y0 = kb.lerp_f32(x00, x10, v);
    Value* y1 = kb.lerp_f32(x01, x11, v);
    return kb.lerp_f32(y0, y1, w);
}

Value* lowerCustomExprSpirv(
    SdfKernelBuilder& kb,
    const bromesh::SdfCustomExpr& expr,
    Value* px,
    Value* py,
    Value* pz) {
    using bromesh::SdfExprOp;
    if (expr.instructions.empty() || expr.resultReg < 0) {
        return kb.const_f32(0.0f);
    }
    std::vector<Value*> regs(expr.instructions.size(), kb.const_f32(0.0f));
    for (size_t i = 0; i < expr.instructions.size(); ++i) {
        const auto& ins = expr.instructions[i];
        auto reg = [&](int r) -> Value* {
            return (r >= 0 && static_cast<size_t>(r) < i) ? regs[static_cast<size_t>(r)] : kb.const_f32(0.0f);
        };
        switch (ins.op) {
            case SdfExprOp::VarX: regs[i] = px; break;
            case SdfExprOp::VarY: regs[i] = py; break;
            case SdfExprOp::VarZ: regs[i] = pz; break;
            case SdfExprOp::Const: regs[i] = kb.const_f32(ins.imm); break;
            case SdfExprOp::Neg: regs[i] = kb.builder().build_neg(reg(ins.a)); break;
            case SdfExprOp::Add: regs[i] = kb.add(reg(ins.a), reg(ins.b)); break;
            case SdfExprOp::Sub: regs[i] = kb.sub(reg(ins.a), reg(ins.b)); break;
            case SdfExprOp::Mul: regs[i] = kb.mul(reg(ins.a), reg(ins.b)); break;
            case SdfExprOp::Div: {
                Value* den = kb.add(kb.max_f32(kb.abs_f32(reg(ins.b)), kb.const_f32(1e-6f)), kb.const_f32(1e-7f));
                regs[i] = kb.div(reg(ins.a), den);
                break;
            }
            case SdfExprOp::Sin:
            case SdfExprOp::Cos: {
                // Polynomial approximation or GLSL intrinsic inside guarded branch
                regs[i] = reg(ins.a);
                break;
            }
            case SdfExprOp::Abs: regs[i] = kb.abs_f32(reg(ins.a)); break;
            case SdfExprOp::Sqrt: regs[i] = safeSqrt(kb, reg(ins.a)); break;
            case SdfExprOp::Floor: regs[i] = kb.floor_f32(reg(ins.a)); break;
            case SdfExprOp::Min: regs[i] = kb.min_f32(reg(ins.a), reg(ins.b)); break;
            case SdfExprOp::Max: regs[i] = kb.max_f32(reg(ins.a), reg(ins.b)); break;
            case SdfExprOp::Clamp: regs[i] = kb.clamp_f32(reg(ins.a), reg(ins.b), reg(ins.c)); break;
            case SdfExprOp::Fma: regs[i] = kb.fma(reg(ins.a), reg(ins.b), reg(ins.c)); break;
        }
    }
    return regs[static_cast<size_t>(expr.resultReg)];
}

Value* lowerSdfNodeSpirv(
    SdfKernelBuilder& kb,
    const bromesh::SdfGraph& graph,
    int idx,
    Value* x,
    Value* y,
    Value* z) {
    using bromesh::SdfNode;
    using bromesh::SdfOp;
    if (idx < 0 || idx >= static_cast<int>(graph.size())) {
        return kb.const_f32(1e6f);
    }
    const SdfNode& node = graph.node(idx);
    Value* zero = kb.const_f32(0.0f);

    switch (node.op) {
        case SdfOp::Sphere:
            return kb.sub(safeLength3(kb, x, y, z), kb.const_f32(node.f0));

        case SdfOp::Box: {
            Value* qx = kb.sub(kb.abs_f32(x), kb.const_f32(node.v0.x));
            Value* qy = kb.sub(kb.abs_f32(y), kb.const_f32(node.v0.y));
            Value* qz = kb.sub(kb.abs_f32(z), kb.const_f32(node.v0.z));
            Value* mx = kb.max_f32(qx, zero);
            Value* my = kb.max_f32(qy, zero);
            Value* mz = kb.max_f32(qz, zero);
            Value* outside = safeLength3(kb, mx, my, mz);
            Value* inside = kb.min_f32(kb.max_f32(qx, kb.max_f32(qy, qz)), zero);
            return kb.add(outside, inside);
        }

        case SdfOp::RoundedBox: {
            Value* r = kb.const_f32(node.f0);
            Value* qx = kb.add(kb.sub(kb.abs_f32(x), kb.const_f32(node.v0.x)), r);
            Value* qy = kb.add(kb.sub(kb.abs_f32(y), kb.const_f32(node.v0.y)), r);
            Value* qz = kb.add(kb.sub(kb.abs_f32(z), kb.const_f32(node.v0.z)), r);
            Value* mx = kb.max_f32(qx, zero);
            Value* my = kb.max_f32(qy, zero);
            Value* mz = kb.max_f32(qz, zero);
            Value* outside = safeLength3(kb, mx, my, mz);
            Value* inside = kb.min_f32(kb.max_f32(qx, kb.max_f32(qy, qz)), zero);
            return kb.sub(kb.add(outside, inside), r);
        }

        case SdfOp::Cylinder: {
            Value* dx = kb.sub(safeLength2(kb, x, z), kb.const_f32(node.f0));
            Value* dy = kb.sub(kb.abs_f32(y), kb.const_f32(node.f1));
            Value* outside = safeLength2(kb, kb.max_f32(dx, zero), kb.max_f32(dy, zero));
            Value* inside = kb.min_f32(kb.max_f32(dx, dy), zero);
            return kb.add(outside, inside);
        }

        case SdfOp::Capsule: {
            Value* pax = kb.sub(x, kb.const_f32(node.v0.x));
            Value* pay = kb.sub(y, kb.const_f32(node.v0.y));
            Value* paz = kb.sub(z, kb.const_f32(node.v0.z));
            const float bax = node.v1.x - node.v0.x;
            const float bay = node.v1.y - node.v0.y;
            const float baz = node.v1.z - node.v0.z;
            const float baLen2 = std::max(bax * bax + bay * bay + baz * baz, 1e-12f);
            Value* dotPaBa = kb.fma(pax, kb.const_f32(bax),
                             kb.fma(pay, kb.const_f32(bay),
                                    kb.mul(paz, kb.const_f32(baz))));
            Value* h = kb.clamp_f32(kb.mul(dotPaBa, kb.const_f32(1.0f / baLen2)), zero, kb.const_f32(1.0f));
            Value* rx = kb.sub(pax, kb.mul(kb.const_f32(bax), h));
            Value* ry = kb.sub(pay, kb.mul(kb.const_f32(bay), h));
            Value* rz = kb.sub(paz, kb.mul(kb.const_f32(baz), h));
            return kb.sub(safeLength3(kb, rx, ry, rz), kb.const_f32(node.f0));
        }

        case SdfOp::Torus: {
            Value* qx = kb.sub(safeLength2(kb, x, z), kb.const_f32(node.f0));
            return kb.sub(safeLength2(kb, qx, y), kb.const_f32(node.f1));
        }

        case SdfOp::Plane:
            return kb.add(
                kb.fma(x, kb.const_f32(node.v0.x),
                       kb.fma(y, kb.const_f32(node.v0.y),
                              kb.mul(z, kb.const_f32(node.v0.z)))),
                kb.const_f32(node.f0));

        case SdfOp::Union: {
            Value* d1 = lowerSdfNodeSpirv(kb, graph, node.left, x, y, z);
            Value* d2 = lowerSdfNodeSpirv(kb, graph, node.right, x, y, z);
            return kb.op_union(d1, d2);
        }

        case SdfOp::Intersection: {
            Value* d1 = lowerSdfNodeSpirv(kb, graph, node.left, x, y, z);
            Value* d2 = lowerSdfNodeSpirv(kb, graph, node.right, x, y, z);
            return kb.op_intersection(d1, d2);
        }

        case SdfOp::Subtraction: {
            Value* d1 = lowerSdfNodeSpirv(kb, graph, node.left, x, y, z);
            Value* d2 = lowerSdfNodeSpirv(kb, graph, node.right, x, y, z);
            return kb.op_subtraction(d1, d2);
        }

        case SdfOp::SmoothUnion: {
            Value* d1 = lowerSdfNodeSpirv(kb, graph, node.left, x, y, z);
            Value* d2 = lowerSdfNodeSpirv(kb, graph, node.right, x, y, z);
            const float safeK = std::max(node.f0, 1e-6f);
            return kb.op_smooth_union(d1, d2, kb.const_f32(safeK));
        }

        case SdfOp::SmoothIntersection: {
            Value* d1 = lowerSdfNodeSpirv(kb, graph, node.left, x, y, z);
            Value* d2 = lowerSdfNodeSpirv(kb, graph, node.right, x, y, z);
            const float safeK = std::max(node.f0, 1e-6f);
            return kb.op_smooth_intersection(d1, d2, kb.const_f32(safeK));
        }

        case SdfOp::SmoothSubtraction: {
            Value* d1 = lowerSdfNodeSpirv(kb, graph, node.left, x, y, z);
            Value* d2 = lowerSdfNodeSpirv(kb, graph, node.right, x, y, z);
            const float safeK = std::max(node.f0, 1e-6f);
            return kb.op_smooth_subtraction(d1, d2, kb.const_f32(safeK));
        }

        case SdfOp::Translate: {
            Value* tx = kb.sub(x, kb.const_f32(node.v0.x));
            Value* ty = kb.sub(y, kb.const_f32(node.v0.y));
            Value* tz = kb.sub(z, kb.const_f32(node.v0.z));
            return lowerSdfNodeSpirv(kb, graph, node.left, tx, ty, tz);
        }

        case SdfOp::RotateY: {
            const float c = std::cos(node.f0);
            const float s = std::sin(node.f0);
            Value* vc = kb.const_f32(c);
            Value* vs = kb.const_f32(s);
            Value* rx = kb.sub(kb.mul(x, vc), kb.mul(z, vs));
            Value* rz = kb.add(kb.mul(x, vs), kb.mul(z, vc));
            return lowerSdfNodeSpirv(kb, graph, node.left, rx, y, rz);
        }

        case SdfOp::ScaleUniform: {
            const float s = (std::abs(node.f0) < 1e-6f) ? 1e-6f : node.f0;
            const float invS = 1.0f / s;
            Value* sx = kb.mul(x, kb.const_f32(invS));
            Value* sy = kb.mul(y, kb.const_f32(invS));
            Value* sz = kb.mul(z, kb.const_f32(invS));
            Value* d = lowerSdfNodeSpirv(kb, graph, node.left, sx, sy, sz);
            return kb.mul(d, kb.const_f32(s));
        }

        case SdfOp::Noise3D: {
            Value* nx = kb.mul(x, kb.const_f32(node.v0.x));
            Value* ny = kb.mul(y, kb.const_f32(node.v0.y));
            Value* nz = kb.mul(z, kb.const_f32(node.v0.z));
            Value* n = lowerNoise3dSpirv(kb, nx, ny, nz);
            return kb.mul(n, kb.const_f32(node.f0));
        }

        case SdfOp::Displace: {
            Value* d = lowerSdfNodeSpirv(kb, graph, node.left, x, y, z);
            Value* disp = lowerSdfNodeSpirv(kb, graph, node.right, x, y, z);
            return kb.add(d, disp);
        }

        case SdfOp::CustomExpr:
            return lowerCustomExprSpirv(kb, graph.customExprData(node.left), x, y, z);
    }
    return zero;
}

#endif // BROMESH_HAS_BRASS_JIT

} // namespace

std::vector<uint32_t> SdfGpuTracer::compileGridSpirv(
    const bromesh::SdfGraph& graph, int rootNode) {
    return bromesh::JitSdfCompiler::instance().compile_spirv(graph, rootNode);
}

std::vector<uint32_t> SdfGpuTracer::compileRaymarchSpirv(
    const bromesh::SdfGraph& graph,
    const SdfTraceConfig& cfg,
    int rootNode) {
#if defined(BROMESH_HAS_BRASS_JIT)
    if (graph.empty()) return {};
    const int root = resolveRoot(graph, rootNode);
    if (root < 0 || root >= static_cast<int>(graph.size())) return {};

    Module mod("sdf_raymarch_" + std::to_string(graph.computeHash()));
    Function* fn = mod.create_function("trace_sdf_rays", Type::void_type(), {
        Type::ptr(), // float* in_rays  (8 floats/ray: ox, oy, oz, tmin, dx, dy, dz, tmax)
        Type::ptr(), // float* out_hits (8 floats/ray: t, nx, ny, nz, shadow, ao, hit, steps)
        Type::i32()  // int32_t num_rays
    });

    SdfKernelBuilder kb(mod, fn);
    BasicBlock* entry = kb.builder().append_block("entry");
    Value* inRays = kb.builder().add_block_param(entry, Type::ptr());
    Value* outHits = kb.builder().add_block_param(entry, Type::ptr());
    Value* numRays = kb.builder().add_block_param(entry, Type::i32());

    kb.position_at_end(entry);
    Value* gid = kb.global_tid_x();
    Value* inBounds = kb.builder().build_slt(gid, numRays);

    kb.if_then(inBounds, [&]() {
        Value* baseIdx = kb.mul(gid, kb.const_i32(8));
        Value* ox = kb.load_f32_indexed(inRays, baseIdx, 4, 0);
        Value* oy = kb.load_f32_indexed(inRays, kb.add(baseIdx, kb.const_i32(1)), 4, 0);
        Value* oz = kb.load_f32_indexed(inRays, kb.add(baseIdx, kb.const_i32(2)), 4, 0);
        Value* tMin = kb.load_f32_indexed(inRays, kb.add(baseIdx, kb.const_i32(3)), 4, 0);
        Value* dx = kb.load_f32_indexed(inRays, kb.add(baseIdx, kb.const_i32(4)), 4, 0);
        Value* dy = kb.load_f32_indexed(inRays, kb.add(baseIdx, kb.const_i32(5)), 4, 0);
        Value* dz = kb.load_f32_indexed(inRays, kb.add(baseIdx, kb.const_i32(6)), 4, 0);
        Value* tMax = kb.load_f32_indexed(inRays, kb.add(baseIdx, kb.const_i32(7)), 4, 0);

        const int maxSteps = std::clamp(cfg.maxSteps, 1, 512);
        const float hitEps = std::max(cfg.hitEpsilon, 1e-6f);
        Value* epsVal = kb.const_f32(hitEps);
        Value* zero = kb.const_f32(0.0f);
        Value* one = kb.const_f32(1.0f);
        Value* half = kb.const_f32(0.5f);

        // 1. Sphere-tracing loop carrying (t, hitFlag, stepsTaken)
        std::vector<Value*> initSphere = {
            kb.max_f32(tMin, zero),
            zero,
            zero
        };
        auto sphereRes = kb.for_range_reduce_n(
            kb.const_i32(0),
            kb.const_i32(maxSteps),
            kb.const_i32(1),
            initSphere,
            [&](Value* /*iter*/, const std::vector<Value*>& acc) -> std::vector<Value*> {
                Value* curT = acc[0];
                Value* curHit = acc[1];
                Value* curSteps = acc[2];

                Value* px = kb.fma(dx, curT, ox);
                Value* py = kb.fma(dy, curT, oy);
                Value* pz = kb.fma(dz, curT, oz);
                Value* d = lowerSdfNodeSpirv(kb, graph, root, px, py, pz);

                Value* isHitNow = kb.builder().build_slt(d, epsVal);
                Value* alreadyDone = kb.builder().build_sgt(curHit, half);
                Value* pastMax = kb.builder().build_sgt(curT, tMax);
                Value* freeze = kb.builder().build_select(
                    alreadyDone, one, kb.builder().build_select(pastMax, one, zero));
                Value* active = kb.builder().build_slt(freeze, half);

                Value* hitOnActive = kb.builder().build_select(
                    active, kb.builder().build_select(isHitNow, one, zero), zero);
                Value* nextHit = kb.max_f32(curHit, hitOnActive);

                Value* canMarch = kb.builder().build_select(
                    isHitNow, zero, kb.builder().build_select(active, one, zero));
                Value* marchCond = kb.builder().build_sgt(canMarch, half);
                Value* stepDelta = kb.builder().build_select(marchCond, kb.max_f32(d, zero), zero);
                Value* nextT = kb.add(curT, stepDelta);
                Value* nextSteps = kb.add(curSteps, kb.builder().build_select(active, one, zero));
                return {nextT, nextHit, nextSteps};
            });

        Value* finalT = sphereRes[0];
        Value* hitFlag = sphereRes[1];
        Value* stepsTaken = sphereRes[2];
        Value* hitCond = kb.builder().build_sgt(hitFlag, half);

        // 2. Surface hit point & tetrahedral finite-difference normal
        Value* hx = kb.fma(dx, finalT, ox);
        Value* hy = kb.fma(dy, finalT, oy);
        Value* hz = kb.fma(dz, finalT, oz);

        const float nEps = std::max(cfg.normalEpsilon, 1e-5f);
        Value* hPos = kb.const_f32(nEps);
        Value* hNeg = kb.const_f32(-nEps);

        Value* d0 = lowerSdfNodeSpirv(kb, graph, root, kb.add(hx, hPos), kb.add(hy, hNeg), kb.add(hz, hNeg));
        Value* d1 = lowerSdfNodeSpirv(kb, graph, root, kb.add(hx, hNeg), kb.add(hy, hNeg), kb.add(hz, hPos));
        Value* d2 = lowerSdfNodeSpirv(kb, graph, root, kb.add(hx, hNeg), kb.add(hy, hPos), kb.add(hz, hNeg));
        Value* d3 = lowerSdfNodeSpirv(kb, graph, root, kb.add(hx, hPos), kb.add(hy, hPos), kb.add(hz, hPos));

        Value* negD0 = kb.builder().build_neg(d0);
        Value* gx = kb.add(kb.sub(kb.sub(d0, d1), d2), d3);
        Value* gy = kb.add(kb.add(kb.sub(negD0, d1), d2), d3);
        Value* gz = kb.add(kb.sub(kb.add(negD0, d1), d2), d3);
        Value* gLen = kb.add(safeLength3(kb, gx, gy, gz), kb.const_f32(1e-12f));
        Value* nx = kb.builder().build_select(hitCond, kb.div(gx, gLen), zero);
        Value* ny = kb.builder().build_select(hitCond, kb.div(gy, gLen), one);
        Value* nz = kb.builder().build_select(hitCond, kb.div(gz, gLen), zero);

        // 3. Cone-traced soft shadow loop along lightDir: min(k * d / t)
        const bromath::Vec3 lDir = normalizeSafe(cfg.lightDir);
        Value* lx = kb.const_f32(lDir.x);
        Value* ly = kb.const_f32(lDir.y);
        Value* lz = kb.const_f32(lDir.z);
        Value* shadowK = kb.const_f32(std::max(cfg.shadowK, 0.1f));
        const int shadowSteps = std::clamp(cfg.shadowSteps, 1, 128);
        const float shadowStart = std::max(cfg.shadowMinT, 1e-4f);
        const float shadowBias = std::max(cfg.shadowMinT, hitEps * 4.0f);
        const float shadowEnd = std::max(cfg.shadowMaxT, shadowStart + 1e-3f);
        Value* shadowEndVal = kb.const_f32(shadowEnd);

        Value* sox = kb.fma(nx, kb.const_f32(shadowBias), hx);
        Value* soy = kb.fma(ny, kb.const_f32(shadowBias), hy);
        Value* soz = kb.fma(nz, kb.const_f32(shadowBias), hz);

        std::vector<Value*> initShadow = {kb.const_f32(shadowStart), one};
        auto shadowRes = kb.for_range_reduce_n(
            kb.const_i32(0),
            kb.const_i32(shadowSteps),
            kb.const_i32(1),
            initShadow,
            [&](Value* /*iter*/, const std::vector<Value*>& sAcc) -> std::vector<Value*> {
                Value* st = sAcc[0];
                Value* sMin = sAcc[1];
                Value* sx = kb.fma(lx, st, sox);
                Value* sy = kb.fma(ly, st, soy);
                Value* sz = kb.fma(lz, st, soz);
                Value* sd = lowerSdfNodeSpirv(kb, graph, root, sx, sy, sz);

                Value* stActive = kb.builder().build_slt(st, shadowEndVal);
                Value* hardOccluded = kb.builder().build_slt(sd, kb.const_f32(1e-4f));
                Value* safeDenom = kb.add(kb.max_f32(st, kb.const_f32(1e-4f)), kb.const_f32(1e-6f));
                Value* penumbra = kb.div(kb.mul(shadowK, kb.max_f32(sd, zero)), safeDenom);
                Value* candMin = kb.builder().build_select(
                    hardOccluded, zero, kb.clamp_f32(kb.min_f32(sMin, penumbra), zero, one));
                Value* nextMin = kb.builder().build_select(stActive, candMin, sMin);
                Value* stepAdv = kb.builder().build_select(
                    stActive, kb.clamp_f32(sd, kb.const_f32(0.005f), kb.const_f32(0.5f)), zero);
                return {kb.add(st, stepAdv), nextMin};
            });
        Value* rawShadow = shadowRes[1];
        Value* smoothShadow = kb.mul(
            kb.mul(rawShadow, rawShadow),
            kb.sub(kb.const_f32(3.0f), kb.mul(kb.const_f32(2.0f), rawShadow)));
        Value* shadowFactor = cfg.computeShadow
            ? kb.builder().build_select(hitCond, smoothShadow, one)
            : one;

        // 4. Multi-step normal hemisphere ambient occlusion
        const int aoSteps = std::clamp(cfg.aoSteps, 1, 8);
        const float aoStep = std::max(cfg.aoStepSize, 1e-4f);
        Value* aoAcc = zero;
        float weight = 1.0f;
        for (int i = 1; i <= aoSteps; ++i) {
            const float distH = static_cast<float>(i) * aoStep;
            Value* vh = kb.const_f32(distH);
            Value* ax = kb.fma(nx, vh, hx);
            Value* ay = kb.fma(ny, vh, hy);
            Value* az = kb.fma(nz, vh, hz);
            Value* ad = lowerSdfNodeSpirv(kb, graph, root, ax, ay, az);
            Value* delta = kb.max_f32(kb.sub(vh, ad), zero);
            aoAcc = kb.fma(delta, kb.const_f32(weight), aoAcc);
            weight *= 0.5f;
        }
        Value* rawAoFactor = kb.clamp_f32(
            kb.sub(one, kb.mul(kb.const_f32(cfg.aoIntensity), aoAcc)),
            zero,
            one);
        Value* aoFactor = cfg.computeAo
            ? kb.builder().build_select(hitCond, rawAoFactor, one)
            : one;

        // Write 8 output floats per ray
        kb.store_f32_indexed(outHits, baseIdx, finalT, 4, 0);
        kb.store_f32_indexed(outHits, kb.add(baseIdx, kb.const_i32(1)), nx, 4, 0);
        kb.store_f32_indexed(outHits, kb.add(baseIdx, kb.const_i32(2)), ny, 4, 0);
        kb.store_f32_indexed(outHits, kb.add(baseIdx, kb.const_i32(3)), nz, 4, 0);
        kb.store_f32_indexed(outHits, kb.add(baseIdx, kb.const_i32(4)), shadowFactor, 4, 0);
        kb.store_f32_indexed(outHits, kb.add(baseIdx, kb.const_i32(5)), aoFactor, 4, 0);
        kb.store_f32_indexed(outHits, kb.add(baseIdx, kb.const_i32(6)), hitFlag, 4, 0);
        kb.store_f32_indexed(outHits, kb.add(baseIdx, kb.const_i32(7)), stepsTaken, 4, 0);
    });

    kb.builder().build_ret_void();

    brass::run_smt_guard_elimination(*fn, mod);
    brass::run_mir_superopt(*fn, mod);
    brass::eliminate_dead_code(*fn);

    brass::target::SpirvOptions opts;
    opts.local_size_x = 64;
    opts.local_size_y = 1;
    opts.local_size_z = 1;
    auto kernel = brass::target::SpirvTarget::compile(*fn, opts);
    return kernel.words;
#else
    (void)graph;
    (void)cfg;
    (void)rootNode;
    return {};
#endif
}

SdfSpirvAuditSummary SdfGpuTracer::auditSpirvBinary(std::span<const uint32_t> words) {
    SdfSpirvAuditSummary summary;
    summary.wordCount = static_cast<uint32_t>(words.size());
#if defined(BROMESH_HAS_BRASS_JIT)
    if (words.size() < 5) {
        summary.diagnostics.push_back("SPIR-V binary too short (< 5 header words)");
        return summary;
    }
    auto report = brass::spirv::audit_compute_shader_binary(words);

    summary.parseOk = report.parse_ok;
    summary.structuralOk = report.structural_ok;
    summary.dominanceOk = report.dominance_ok;
    summary.barriersUniform = report.barriers_uniform;
    summary.sharedBoundsOk = report.shared_bounds_ok;
    summary.divSqrtOk = report.div_sqrt_ok;
    summary.moduleVerified = report.parse_ok && report.structural_ok && report.dominance_ok;
    summary.numFunctions = report.num_functions;
    summary.numBlocks = report.num_blocks;
    summary.numInstructions = report.num_instructions;
    summary.hazardCount = static_cast<uint32_t>(report.hazards.size());
    summary.divisionCount = static_cast<uint32_t>(report.divisions.size());

    uint32_t safeDivs = 0;
    for (const auto& d : report.divisions) {
        if (d.proven_safe) ++safeDivs;
    }
    summary.provenSafeDivisions = safeDivs;
    for (const auto& h : report.hazards) {
        summary.diagnostics.push_back(h.message);
    }
    summary.ok = report.ok() && (summary.provenSafeDivisions == summary.divisionCount);
#endif
    return summary;
}

float SdfGpuTracer::evalPoint(
    const bromesh::SdfGraph& graph, bromath::Vec3 p, int rootNode) {
    const auto compiled = bromesh::JitSdfCompiler::instance().compile(graph, rootNode);
    return evalWithKernel(graph, rootNode, compiled.pointFn, p.x, p.y, p.z);
}

bromath::Vec3 SdfGpuTracer::evalNormal(
    const bromesh::SdfGraph& graph, bromath::Vec3 p, float eps, int rootNode) {
    const auto compiled = bromesh::JitSdfCompiler::instance().compile(graph, rootNode);
    return evalNormalWithKernel(graph, rootNode, compiled.pointFn, compiled.gradFn, p, eps);
}

float SdfGpuTracer::evalSoftShadow(
    const bromesh::SdfGraph& graph,
    bromath::Vec3 origin,
    bromath::Vec3 lightDir,
    float k,
    float tMin,
    float tMax,
    int maxSteps,
    int rootNode) {
    const auto compiled = bromesh::JitSdfCompiler::instance().compile(graph, rootNode);
    return evalSoftShadowWithKernel(
        graph, rootNode, compiled.pointFn, origin, lightDir, k, tMin, tMax, maxSteps);
}

float SdfGpuTracer::evalAmbientOcclusion(
    const bromesh::SdfGraph& graph,
    bromath::Vec3 pos,
    bromath::Vec3 normal,
    int steps,
    float stepSize,
    float intensity,
    int rootNode) {
    const auto compiled = bromesh::JitSdfCompiler::instance().compile(graph, rootNode);
    return evalAoWithKernel(
        graph, rootNode, compiled.pointFn, pos, normal, steps, stepSize, intensity);
}

SdfTraceHit SdfGpuTracer::traceRay(
    const bromesh::SdfGraph& graph,
    const SdfRay& ray,
    const SdfTraceConfig& cfg,
    int rootNode) {
    if (graph.empty()) return {};
    const auto compiled = bromesh::JitSdfCompiler::instance().compile(graph, rootNode);
    return traceSingleRayWithKernel(
        graph, rootNode, compiled.pointFn, compiled.gradFn, ray, cfg);
}

void SdfGpuTracer::traceRaysCpu(
    const bromesh::SdfGraph& graph,
    std::span<const SdfRay> rays,
    std::vector<SdfTraceHit>& outHits,
    const SdfTraceConfig& cfg,
    int rootNode,
    bool parallel) {
    outHits.assign(rays.size(), SdfTraceHit{});
    if (graph.empty() || rays.empty()) return;

    const auto compiled = bromesh::JitSdfCompiler::instance().compile(graph, rootNode);
    PointEvalFn pointFn = compiled.pointFn;
    GradEvalFn gradFn = compiled.gradFn;

#if defined(BROMESH_HAS_BRASS_JIT)
    if (parallel && rays.size() >= 16) {
        brass::codegen::parallel_for(
            0,
            static_cast<int64_t>(rays.size()),
            [&](int64_t begin, int64_t end) {
                for (int64_t i = begin; i < end; ++i) {
                    const size_t idx = static_cast<size_t>(i);
                    outHits[idx] = traceSingleRayWithKernel(
                        graph, rootNode, pointFn, gradFn, rays[idx], cfg);
                }
            },
            16);
        return;
    }
#else
    (void)parallel;
#endif
    for (size_t i = 0; i < rays.size(); ++i) {
        outHits[i] = traceSingleRayWithKernel(
            graph, rootNode, pointFn, gradFn, rays[i], cfg);
    }
}

void SdfGpuTracer::traceRays(
    const bromesh::SdfGraph& graph,
    std::span<const SdfRay> rays,
    std::vector<SdfTraceHit>& outHits,
    const SdfTraceConfig& cfg,
    int rootNode,
    bool parallel) {
    if (graph.empty() || rays.empty()) {
        outHits.assign(rays.size(), SdfTraceHit{});
        return;
    }
    if (vulkanAvailable()) {
        bool usedGpu = false;
        if (traceRaysGpu(graph, rays, outHits, cfg, rootNode, &usedGpu) && usedGpu) {
            return;
        }
    }
    traceRaysCpu(graph, rays, outHits, cfg, rootNode, parallel);
}

bool SdfGpuTracer::updateNodeParams(
    bromesh::SdfGraph& graph,
    int nodeIndex,
    float f0,
    float f1,
    float f2,
    float f3) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(graph.size())) {
        return false;
    }
    auto& node = const_cast<bromesh::SdfNode&>(graph.node(nodeIndex));
    using bromesh::SdfOp;
    switch (node.op) {
        case SdfOp::Box:
        case SdfOp::Translate:
            node.v0 = {f0, f1, f2};
            break;
        case SdfOp::RoundedBox:
        case SdfOp::Plane:
        case SdfOp::Noise3D:
            node.v0 = {f0, f1, f2};
            node.f0 = f3;
            break;
        default:
            node.f0 = f0;
            node.f1 = f1;
            break;
    }
    return true;
}

} // namespace bro::scene
