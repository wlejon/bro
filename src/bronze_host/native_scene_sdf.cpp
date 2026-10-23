#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_js_gpu.h"
#include "bronze_host/host_values.h"
#include "scene/sdf_gpu_tracer.h"

#include <bromesh/isosurface/jit/sdf_node.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

inline constexpr uint32_t kHostSdfGraphTag = 0x53444647u; // 'SDFG'

struct HostSdfGraphMirror {
    bromesh::SdfGraph graph;
    uint32_t tag = kHostSdfGraphTag;
};

bromesh::SdfGraph* unwrapSdfGraph(Value v) {
    if (!ev::isObject(v)) return nullptr;
    void* raw = ev::handleData(v);
    if (!raw) return nullptr;
    auto* cell = static_cast<HostSdfGraphMirror*>(raw);
    return (cell->tag == kHostSdfGraphTag) ? &cell->graph : nullptr;
}

bool readVec3(Value v, bromath::Vec3& out) {
    if (!ev::isObject(v)) return false;
    const float* ptr = nullptr;
    size_t count = 0;
    SmallBuffer<float, 8> scratch;
    if (floatData(v, scratch, &ptr, &count) && count >= 3) {
        out.x = ptr[0];
        out.y = ptr[1];
        out.z = ptr[2];
        return true;
    }
    ev::Persistent root(v);
    Value vx = ev::getProperty(root.get(), "x");
    Value vy = ev::getProperty(root.get(), "y");
    Value vz = ev::getProperty(root.get(), "z");
    if (vx.isNumber() && vy.isNumber() && vz.isNumber()) {
        out.x = static_cast<float>(vx.asNumber());
        out.y = static_cast<float>(vy.asNumber());
        out.z = static_cast<float>(vz.asNumber());
        return true;
    }
    return false;
}

scene::SdfTraceConfig parseTraceConfig(Value optsVal, int* outRootNode = nullptr) {
    scene::SdfTraceConfig cfg;
    if (!ev::isObject(optsVal)) return cfg;
    ev::Persistent opts(optsVal);

    auto readNum = [&](const char* key, float& target) {
        Value v = ev::getProperty(opts.get(), key);
        if (v.isNumber() && !std::isnan(v.asNumber())) {
            target = static_cast<float>(v.asNumber());
        }
    };
    auto readInt = [&](const char* key, int& target) {
        Value v = ev::getProperty(opts.get(), key);
        if (v.isNumber() && !std::isnan(v.asNumber())) {
            target = static_cast<int>(v.asNumber());
        }
    };
    auto readBool = [&](const char* key, bool& target) {
        Value v = ev::getProperty(opts.get(), key);
        if (!v.isUndefined()) target = ev::toBool(v);
    };

    readInt("maxSteps", cfg.maxSteps);
    readNum("hitEpsilon", cfg.hitEpsilon);
    readNum("normalEpsilon", cfg.normalEpsilon);
    readNum("shadowK", cfg.shadowK);
    readNum("shadowMinT", cfg.shadowMinT);
    readNum("shadowMaxT", cfg.shadowMaxT);
    readInt("shadowSteps", cfg.shadowSteps);
    readInt("aoSteps", cfg.aoSteps);
    readNum("aoStepSize", cfg.aoStepSize);
    readNum("aoIntensity", cfg.aoIntensity);
    readBool("computeShadow", cfg.computeShadow);
    readBool("computeAo", cfg.computeAo);

    Value lightDirV = ev::getProperty(opts.get(), "lightDir");
    if (ev::isObject(lightDirV)) {
        readVec3(lightDirV, cfg.lightDir);
    }

    if (outRootNode) {
        int rn = *outRootNode;
        readInt("rootNode", rn);
        *outRootNode = rn;
    }
    return cfg;
}

Value makeVec3Array(bromath::Vec3 v) {
    return hostArrayOf(3, [v](size_t i) -> Value {
        const float comp = (i == 0) ? v.x : (i == 1 ? v.y : v.z);
        return ev::fromDouble(comp);
    });
}

Value buildAuditObject(const scene::SdfSpirvAuditSummary& s) {
    ObjectBuilder b;
    b.set("ok", ev::fromBool(s.ok));
    b.set("moduleVerified", ev::fromBool(s.moduleVerified));
    b.set("parseOk", ev::fromBool(s.parseOk));
    b.set("structuralOk", ev::fromBool(s.structuralOk));
    b.set("dominanceOk", ev::fromBool(s.dominanceOk));
    b.set("barriersUniform", ev::fromBool(s.barriersUniform));
    b.set("sharedBoundsOk", ev::fromBool(s.sharedBoundsOk));
    b.set("divSqrtOk", ev::fromBool(s.divSqrtOk));
    b.set("wordCount", ev::fromDouble(s.wordCount));
    b.set("numFunctions", ev::fromDouble(s.numFunctions));
    b.set("numBlocks", ev::fromDouble(s.numBlocks));
    b.set("numInstructions", ev::fromDouble(s.numInstructions));
    b.set("hazardCount", ev::fromDouble(s.hazardCount));
    b.set("divisionCount", ev::fromDouble(s.divisionCount));
    b.set("provenSafeDivisions", ev::fromDouble(s.provenSafeDivisions));
    ev::Persistent diagArr(hostArrayOf(s.diagnostics.size(), [&](size_t i) {
        return ev::fromUtf8(s.diagnostics[i]);
    }));
    b.set("diagnostics", diagArr.get());
    return b.get();
}

Value buildHitObject(const scene::SdfTraceHit& h) {
    ObjectBuilder b;
    b.set("hit", ev::fromBool(h.hit));
    b.set("t", ev::fromDouble(h.t));
    b.set("distance", ev::fromDouble(h.distance));
    ev::Persistent posArr(makeVec3Array(h.position));
    ev::Persistent normArr(makeVec3Array(h.normal));
    b.set("position", posArr.get());
    b.set("normal", normArr.get());
    b.set("softShadow", ev::fromDouble(h.softShadow));
    b.set("ambientOcclusion", ev::fromDouble(h.ambientOcclusion));
    b.set("steps", ev::fromDouble(h.steps));
    return b.get();
}

Value implCompileSpirv(const bromesh::SdfGraph& g, std::span<const Value> args) {
    const int rootNode = hasArg(args, 0) ? i32At(args, 0) : -1;
    return makeUint32Array(scene::SdfGpuTracer::compileGridSpirv(g, rootNode));
}

Value implCompileRaymarchSpirv(const bromesh::SdfGraph& g, std::span<const Value> args) {
    int rootNode = hasArg(args, 1) ? i32At(args, 1) : -1;
    scene::SdfTraceConfig cfg = parseTraceConfig(argAt(args, 0), &rootNode);
    return makeUint32Array(scene::SdfGpuTracer::compileRaymarchSpirv(g, cfg, rootNode));
}

Value implAuditSpirv(const bromesh::SdfGraph* g, std::span<const Value> args) {
    if (hasArg(args, 0) && ev::isObject(args[0])) {
        const uint32_t* ptr = nullptr;
        size_t count = 0;
        std::vector<uint32_t> scratch;
        if (uint32Data(args[0], scratch, &ptr, &count) && count > 0) {
            std::vector<uint32_t> copy(ptr, ptr + count);
            return buildAuditObject(scene::SdfGpuTracer::auditSpirvBinary(copy));
        }
    }
    if (!g) return ev::null();
    std::string mode = "raymarch";
    if (hasArg(args, 0) && ev::isString(args[0])) {
        mode = ev::toUtf8(args[0]);
    }
    int rootNode = -1;
    scene::SdfTraceConfig cfg = parseTraceConfig(argAt(args, 1), &rootNode);
    std::vector<uint32_t> words = (mode == "grid")
        ? scene::SdfGpuTracer::compileGridSpirv(*g, rootNode)
        : scene::SdfGpuTracer::compileRaymarchSpirv(*g, cfg, rootNode);
    return buildAuditObject(scene::SdfGpuTracer::auditSpirvBinary(words));
}

Value implEvalPoint(const bromesh::SdfGraph& g, std::span<const Value> args) {
    bromath::Vec3 p{0.0f, 0.0f, 0.0f};
    int rootNode = -1;
    if (hasArg(args, 0) && ev::isObject(args[0])) {
        readVec3(args[0], p);
        if (hasArg(args, 1)) rootNode = i32At(args, 1);
    } else {
        p.x = static_cast<float>(numAt(args, 0));
        p.y = static_cast<float>(numAt(args, 1));
        p.z = static_cast<float>(numAt(args, 2));
        if (hasArg(args, 3)) rootNode = i32At(args, 3);
    }
    return ev::fromDouble(scene::SdfGpuTracer::evalPoint(g, p, rootNode));
}

Value implEvalNormal(const bromesh::SdfGraph& g, std::span<const Value> args) {
    bromath::Vec3 p{0.0f, 0.0f, 0.0f};
    float eps = 1e-3f;
    int rootNode = -1;
    if (hasArg(args, 0) && ev::isObject(args[0])) {
        readVec3(args[0], p);
        if (hasArg(args, 1)) eps = static_cast<float>(numAt(args, 1));
        if (hasArg(args, 2)) rootNode = i32At(args, 2);
    } else {
        p.x = static_cast<float>(numAt(args, 0));
        p.y = static_cast<float>(numAt(args, 1));
        p.z = static_cast<float>(numAt(args, 2));
        if (hasArg(args, 3)) eps = static_cast<float>(numAt(args, 3));
        if (hasArg(args, 4)) rootNode = i32At(args, 4);
    }
    return makeVec3Array(scene::SdfGpuTracer::evalNormal(g, p, eps, rootNode));
}

Value implSoftShadow(const bromesh::SdfGraph& g, std::span<const Value> args) {
    bromath::Vec3 origin{0.0f, 0.0f, 0.0f};
    bromath::Vec3 lightDir{0.57735027f, 0.57735027f, 0.57735027f};
    readVec3(argAt(args, 0), origin);
    readVec3(argAt(args, 1), lightDir);
    const float k = hasArg(args, 2) ? static_cast<float>(numAt(args, 2)) : 16.0f;
    const float tMin = hasArg(args, 3) ? static_cast<float>(numAt(args, 3)) : 0.02f;
    const float tMax = hasArg(args, 4) ? static_cast<float>(numAt(args, 4)) : 20.0f;
    const int maxSteps = hasArg(args, 5) ? i32At(args, 5) : 64;
    const int rootNode = hasArg(args, 6) ? i32At(args, 6) : -1;
    return ev::fromDouble(scene::SdfGpuTracer::evalSoftShadow(
        g, origin, lightDir, k, tMin, tMax, maxSteps, rootNode));
}

Value implAmbientOcclusion(const bromesh::SdfGraph& g, std::span<const Value> args) {
    bromath::Vec3 pos{0.0f, 0.0f, 0.0f};
    bromath::Vec3 normal{0.0f, 1.0f, 0.0f};
    readVec3(argAt(args, 0), pos);
    readVec3(argAt(args, 1), normal);
    const int steps = hasArg(args, 2) ? i32At(args, 2) : 5;
    const float stepSize = hasArg(args, 3) ? static_cast<float>(numAt(args, 3)) : 0.08f;
    const float intensity = hasArg(args, 4) ? static_cast<float>(numAt(args, 4)) : 1.5f;
    const int rootNode = hasArg(args, 5) ? i32At(args, 5) : -1;
    return ev::fromDouble(scene::SdfGpuTracer::evalAmbientOcclusion(
        g, pos, normal, steps, stepSize, intensity, rootNode));
}

Value implTraceRay(const bromesh::SdfGraph& g, std::span<const Value> args) {
    scene::SdfRay ray;
    readVec3(argAt(args, 0), ray.origin);
    readVec3(argAt(args, 1), ray.dir);
    int rootNode = hasArg(args, 3) ? i32At(args, 3) : -1;
    scene::SdfTraceConfig cfg = parseTraceConfig(argAt(args, 2), &rootNode);
    if (ev::isObject(argAt(args, 2))) {
        ev::Persistent opts(argAt(args, 2));
        Value tMinV = ev::getProperty(opts.get(), "tMin");
        Value tMaxV = ev::getProperty(opts.get(), "tMax");
        if (tMinV.isNumber()) ray.tMin = static_cast<float>(tMinV.asNumber());
        if (tMaxV.isNumber()) ray.tMax = static_cast<float>(tMaxV.asNumber());
    }
    return buildHitObject(scene::SdfGpuTracer::traceRay(g, ray, cfg, rootNode));
}

Value implTraceRays(const bromesh::SdfGraph& g, std::span<const Value> args) {
    const float* raw = nullptr;
    size_t count = 0;
    std::vector<float> scratch;
    if (!floatData(argAt(args, 0), scratch, &raw, &count) || count < 6) {
        return makeFloat32Array({});
    }
    std::vector<float> copy(raw, raw + count);
    const size_t stride = (count % 8 == 0) ? 8 : 6;
    const size_t numRays = count / stride;
    std::vector<scene::SdfRay> rays(numRays);
    for (size_t i = 0; i < numRays; ++i) {
        const float* r = copy.data() + i * stride;
        if (stride == 8) {
            rays[i].origin = {r[0], r[1], r[2]};
            rays[i].tMin = r[3];
            rays[i].dir = {r[4], r[5], r[6]};
            rays[i].tMax = r[7];
        } else {
            rays[i].origin = {r[0], r[1], r[2]};
            rays[i].tMin = 0.0f;
            rays[i].dir = {r[3], r[4], r[5]};
            rays[i].tMax = 100.0f;
        }
    }
    int rootNode = hasArg(args, 2) ? i32At(args, 2) : -1;
    scene::SdfTraceConfig cfg = parseTraceConfig(argAt(args, 1), &rootNode);
    std::vector<scene::SdfTraceHit> hits;
    scene::SdfGpuTracer::traceRays(g, rays, hits, cfg, rootNode, true);

    std::vector<float> packed(numRays * 8);
    for (size_t i = 0; i < numRays; ++i) {
        const auto& h = hits[i];
        float* dst = packed.data() + i * 8;
        dst[0] = h.t;
        dst[1] = h.normal.x;
        dst[2] = h.normal.y;
        dst[3] = h.normal.z;
        dst[4] = h.softShadow;
        dst[5] = h.ambientOcclusion;
        dst[6] = h.hit ? 1.0f : 0.0f;
        dst[7] = static_cast<float>(h.steps);
    }
    return makeFloat32Array(packed);
}

Value implUpdateNode(bromesh::SdfGraph& g, std::span<const Value> args) {
    const int idx = i32At(args, 0);
    const float f0 = static_cast<float>(numAt(args, 1));
    const float f1 = hasArg(args, 2) ? static_cast<float>(numAt(args, 2)) : 0.0f;
    const float f2 = hasArg(args, 3) ? static_cast<float>(numAt(args, 3)) : 0.0f;
    const float f3 = hasArg(args, 4) ? static_cast<float>(numAt(args, 4)) : 0.0f;
    return ev::fromBool(scene::SdfGpuTracer::updateNodeParams(g, idx, f0, f1, f2, f3));
}

Value implToMesh(Value selfObj, std::span<const Value> args) {
    if (!ev::isObject(selfObj)) return ev::null();
    ev::Persistent selfRoot(selfObj);
    bool useSurfaceNets = false;
    if (!args.empty() && ev::isObject(args[0])) {
        ev::Persistent opts(args[0]);
        Value modeV = ev::getProperty(opts.get(), "mode");
        if (modeV.isString()) {
            const std::string mode = ev::toUtf8(modeV);
            if (mode == "surfaceNets" || mode == "surface_nets") {
                useSurfaceNets = true;
            }
        }
    }
    const char* methodName = useSurfaceNets ? "surfaceNets" : "marchingCubes";
    Value fn = ev::getProperty(selfRoot.get(), methodName);
    if (!ev::isFunction(fn)) return ev::null();
    auto r = ev::call(fn, selfRoot.get(), args);
    return r.thrown ? ev::null() : r.value;
}

Value implEvaluateGridGpu(const bromesh::SdfGraph& g, std::span<const Value> args) {
    int dimX = 32, dimY = 32, dimZ = 32;
    bromath::AABB3 bounds{{-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}};
    int rootNode = -1;

    if (!args.empty() && ev::isObject(args[0])) {
        ev::Persistent opts(args[0]);
        Value resV = ev::getProperty(opts.get(), "resolution");
        if (resV.isNumber()) {
            const int r = std::max(2, static_cast<int>(resV.asNumber()));
            dimX = dimY = dimZ = r;
        } else if (ev::isObject(resV)) {
            bromath::Vec3 rv{32.0f, 32.0f, 32.0f};
            if (readVec3(resV, rv)) {
                dimX = std::max(2, static_cast<int>(rv.x));
                dimY = std::max(2, static_cast<int>(rv.y));
                dimZ = std::max(2, static_cast<int>(rv.z));
            }
        }
        Value dxV = ev::getProperty(opts.get(), "dimX");
        Value dyV = ev::getProperty(opts.get(), "dimY");
        Value dzV = ev::getProperty(opts.get(), "dimZ");
        if (dxV.isNumber()) dimX = std::max(2, static_cast<int>(dxV.asNumber()));
        if (dyV.isNumber()) dimY = std::max(2, static_cast<int>(dyV.asNumber()));
        if (dzV.isNumber()) dimZ = std::max(2, static_cast<int>(dzV.asNumber()));

        readVec3(ev::getProperty(opts.get(), "boundsMin"), bounds.min);
        readVec3(ev::getProperty(opts.get(), "boundsMax"), bounds.max);
        readVec3(ev::getProperty(opts.get(), "min"), bounds.min);
        readVec3(ev::getProperty(opts.get(), "max"), bounds.max);

        Value rootV = ev::getProperty(opts.get(), "rootNode");
        if (rootV.isNumber()) rootNode = static_cast<int>(rootV.asNumber());
    }

    std::vector<float> field;
    bool usedVulkan = false;
    scene::SdfGpuTracer::evaluateGridGpu(g, dimX, dimY, dimZ, bounds, field, rootNode, &usedVulkan);

    ObjectBuilder out;
    out.set("dimX", ev::fromDouble(dimX));
    out.set("dimY", ev::fromDouble(dimY));
    out.set("dimZ", ev::fromDouble(dimZ));
    out.set("usedVulkanGpu", ev::fromBool(usedVulkan));
    out.set("field", makeFloat32Array(field));
    return out.get();
}

Value implRenderSdfToBuffer(const bromesh::SdfGraph& g, std::span<const Value> args) {
    int width = 32;
    int height = 32;
    bromath::Vec3 eye{0.0f, 0.0f, 5.0f};
    bromath::Vec3 target{0.0f, 0.0f, 0.0f};
    bromath::Vec3 up{0.0f, 1.0f, 0.0f};
    float fovDeg = 60.0f;
    float tMax = 50.0f;
    int rootNode = -1;
    scene::SdfTraceConfig cfg = parseTraceConfig(argAt(args, 0), &rootNode);

    if (!args.empty() && ev::isObject(args[0])) {
        ev::Persistent opts(args[0]);
        Value wV = ev::getProperty(opts.get(), "width");
        Value hV = ev::getProperty(opts.get(), "height");
        Value fovV = ev::getProperty(opts.get(), "fovY");
        Value tMaxV = ev::getProperty(opts.get(), "tMax");
        if (wV.isNumber()) width = std::clamp(static_cast<int>(wV.asNumber()), 1, 2048);
        if (hV.isNumber()) height = std::clamp(static_cast<int>(hV.asNumber()), 1, 2048);
        if (fovV.isNumber()) fovDeg = static_cast<float>(fovV.asNumber());
        if (tMaxV.isNumber()) tMax = static_cast<float>(tMaxV.asNumber());
        readVec3(ev::getProperty(opts.get(), "eye"), eye);
        readVec3(ev::getProperty(opts.get(), "target"), target);
        readVec3(ev::getProperty(opts.get(), "up"), up);
    }

    auto norm3 = [](bromath::Vec3 v) -> bromath::Vec3 {
        const float l2 = v.x * v.x + v.y * v.y + v.z * v.z;
        if (l2 <= 1e-20f) return {0.0f, 0.0f, -1.0f};
        const float inv = 1.0f / std::sqrt(l2);
        return {v.x * inv, v.y * inv, v.z * inv};
    };
    auto cross3 = [](bromath::Vec3 a, bromath::Vec3 b) -> bromath::Vec3 {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        };
    };

    const bromath::Vec3 fwd = norm3({target.x - eye.x, target.y - eye.y, target.z - eye.z});
    const bromath::Vec3 right = norm3(cross3(fwd, up));
    const bromath::Vec3 camUp = norm3(cross3(right, fwd));
    const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const float halfH = std::tan(fovDeg * 0.5f * (3.14159265358979323846f / 180.0f));
    const float halfW = halfH * aspect;

    const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<scene::SdfRay> rays(totalPixels);
    for (int py = 0; py < height; ++py) {
        const float ndcY = 1.0f - 2.0f * (static_cast<float>(py) + 0.5f) / static_cast<float>(height);
        for (int px = 0; px < width; ++px) {
            const float ndcX = 2.0f * (static_cast<float>(px) + 0.5f) / static_cast<float>(width) - 1.0f;
            const bromath::Vec3 dir = norm3({
                fwd.x + right.x * (ndcX * halfW) + camUp.x * (ndcY * halfH),
                fwd.y + right.y * (ndcX * halfW) + camUp.y * (ndcY * halfH),
                fwd.z + right.z * (ndcX * halfW) + camUp.z * (ndcY * halfH),
            });
            scene::SdfRay& r = rays[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)];
            r.origin = eye;
            r.tMin = 0.0f;
            r.dir = dir;
            r.tMax = tMax;
        }
    }

    std::vector<scene::SdfTraceHit> hits;
    bool usedVulkan = false;
    scene::SdfGpuTracer::traceRaysGpu(g, rays, hits, cfg, rootNode, &usedVulkan);

    std::vector<float> packed(totalPixels * 8);
    for (size_t i = 0; i < totalPixels; ++i) {
        const auto& h = hits[i];
        float* dst = packed.data() + i * 8;
        dst[0] = h.t;
        dst[1] = h.normal.x;
        dst[2] = h.normal.y;
        dst[3] = h.normal.z;
        dst[4] = h.softShadow;
        dst[5] = h.ambientOcclusion;
        dst[6] = h.hit ? 1.0f : 0.0f;
        dst[7] = static_cast<float>(h.steps);
    }

    ObjectBuilder out;
    out.set("width", ev::fromDouble(width));
    out.set("height", ev::fromDouble(height));
    out.set("usedVulkanGpu", ev::fromBool(usedVulkan));
    out.set("hits", makeFloat32Array(packed));
    return out.get();
}

void attachSdfFunctionsToObject(ObjectBuilder& b) {
    b.def("compileSdfSpirv", 2, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implCompileSpirv(*g, a.empty() ? a : a.subspan(1)) : ev::null();
    });
    b.def("compileSdfRaymarchSpirv", 3, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implCompileRaymarchSpirv(*g, a.empty() ? a : a.subspan(1)) : ev::null();
    });
    b.def("auditSdfSpirv", 3, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        if (g) return implAuditSpirv(g, a.empty() ? a : a.subspan(1));
        return implAuditSpirv(nullptr, a);
    });
    b.def("evalSdfPoint", 5, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implEvalPoint(*g, a.empty() ? a : a.subspan(1)) : ev::fromDouble(0.0);
    });
    b.def("evalSdfNormal", 6, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implEvalNormal(*g, a.empty() ? a : a.subspan(1)) : ev::null();
    });
    b.def("sdfSoftShadow", 8, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implSoftShadow(*g, a.empty() ? a : a.subspan(1)) : ev::fromDouble(1.0);
    });
    b.def("sdfAmbientOcclusion", 7, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implAmbientOcclusion(*g, a.empty() ? a : a.subspan(1)) : ev::fromDouble(1.0);
    });
    b.def("traceSdfRay", 5, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implTraceRay(*g, a.empty() ? a : a.subspan(1)) : ev::null();
    });
    b.def("traceSdfRays", 4, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implTraceRays(*g, a.empty() ? a : a.subspan(1)) : makeFloat32Array({});
    });
    b.def("traceSdfRaysGpu", 4, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implTraceRays(*g, a.empty() ? a : a.subspan(1)) : makeFloat32Array({});
    });
    b.def("evaluateSdfGridGpu", 2, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implEvaluateGridGpu(*g, a.empty() ? a : a.subspan(1)) : ev::null();
    });
    b.def("renderSdfToBuffer", 2, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implRenderSdfToBuffer(*g, a.empty() ? a : a.subspan(1)) : ev::null();
    });
    b.def("sdfToMesh", 2, [](Value, std::span<const Value> a) -> Value {
        return a.empty() ? ev::null() : implToMesh(a[0], a.subspan(1));
    });
    b.def("sdfVulkanAvailable", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(scene::SdfGpuTracer::vulkanAvailable());
    });
    b.def("sdfVulkanDeviceName", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8(scene::SdfGpuTracer::vulkanDeviceName());
    });
    b.def("sdfLastTraceUsedGpu", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(scene::SdfGpuTracer::lastTraceUsedVulkanGpu());
    });
    b.def("updateSdfNode", 6, [](Value, std::span<const Value> a) -> Value {
        auto* g = unwrapSdfGraph(argAt(a, 0));
        return g ? implUpdateNode(*g, a.empty() ? a : a.subspan(1)) : ev::fromBool(false);
    });
}

} // namespace

void installSdfGraphGpuExtensions() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    ev::Persistent globalObj(gt.value);

    Value sdfCtor = ev::getProperty(globalObj.get(), "SDFGraph");
    if (ev::isObject(sdfCtor)) {
        ev::Persistent ctorRoot(sdfCtor);
        Value protoVal = ev::getProperty(ctorRoot.get(), "prototype");
        if (ev::isObject(protoVal)) {
            ObjectBuilder proto(protoVal);
            proto.def("compileSpirv", 1, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implCompileSpirv(*g, a) : ev::null();
            });
            proto.def("compileRaymarchSpirv", 2, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implCompileRaymarchSpirv(*g, a) : ev::null();
            });
            proto.def("auditSpirv", 2, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return implAuditSpirv(g, a);
            });
            proto.def("evalPoint", 4, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implEvalPoint(*g, a) : ev::fromDouble(0.0);
            });
            proto.def("evalNormal", 5, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implEvalNormal(*g, a) : ev::null();
            });
            proto.def("softShadow", 7, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implSoftShadow(*g, a) : ev::fromDouble(1.0);
            });
            proto.def("ambientOcclusion", 6, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implAmbientOcclusion(*g, a) : ev::fromDouble(1.0);
            });
            proto.def("traceRay", 4, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implTraceRay(*g, a) : ev::null();
            });
            proto.def("traceRays", 3, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implTraceRays(*g, a) : makeFloat32Array({});
            });
            proto.def("traceRaysGpu", 3, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implTraceRays(*g, a) : makeFloat32Array({});
            });
            proto.def("evaluateGridGpu", 1, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implEvaluateGridGpu(*g, a) : ev::null();
            });
            proto.def("renderToBuffer", 1, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implRenderSdfToBuffer(*g, a) : ev::null();
            });
            proto.def("toMesh", 1, [](Value self, std::span<const Value> a) -> Value {
                return implToMesh(self, a);
            });
            proto.def("updateNode", 5, [](Value self, std::span<const Value> a) -> Value {
                auto* g = unwrapSdfGraph(self);
                return g ? implUpdateNode(*g, a) : ev::fromBool(false);
            });
        }
    }

    Value broVal = ev::getProperty(globalObj.get(), "bro");
    if (ev::isObject(broVal)) {
        ev::Persistent broRoot(broVal);
        Value meshVal = ev::getProperty(broRoot.get(), "mesh");
        if (ev::isObject(meshVal)) {
            ObjectBuilder meshBuilder(meshVal);
            attachSdfFunctionsToObject(meshBuilder);
            attachJsGpuKernelFunctions(meshBuilder);
        }
    }
}

void installSceneSdfExtensions() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    ev::Persistent globalObj(gt.value);
    Value broVal = ev::getProperty(globalObj.get(), "bro");
    if (!ev::isObject(broVal)) return;
    ev::Persistent broRoot(broVal);
    Value sceneVal = ev::getProperty(broRoot.get(), "scene");
    if (!ev::isObject(sceneVal)) return;
    ObjectBuilder sceneBuilder(sceneVal);
    attachSdfFunctionsToObject(sceneBuilder);
    attachJsGpuKernelFunctions(sceneBuilder);
}

} // namespace bro::bronze_host
