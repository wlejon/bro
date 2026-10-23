#include "bronze_host/host_js_gpu.h"
#include "bronze_host/host_values.h"

#include "codegen-brass/js_gpu_autodiff.h"
#include <brass/verify/bv_solver.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

struct CompiledJsAutodiffEntry {
    bronze::GpuAutodiffResult compiled;
};

std::mutex& autodiffRegistryMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::atomic<uint32_t> s_nextAutodiffId{1};
thread_local uint32_t tl_activeAutodiffCallId = 0;

std::unordered_map<uint32_t, std::shared_ptr<CompiledJsAutodiffEntry>>& autodiffRegistry() {
    static auto* r = new std::unordered_map<uint32_t, std::shared_ptr<CompiledJsAutodiffEntry>>();
    return *r;
}

std::shared_ptr<CompiledJsAutodiffEntry> lookupAutodiffEntryById(uint32_t id) {
    if (id == 0) return nullptr;
    std::lock_guard<std::mutex> lock(autodiffRegistryMutex());
    auto& reg = autodiffRegistry();
    auto it = reg.find(id);
    return (it != reg.end()) ? it->second : nullptr;
}

std::shared_ptr<CompiledJsAutodiffEntry> lookupAutodiffEntry(Value self) {
    if (ev::isObject(self) || ev::isFunction(self)) {
        Value idVal = ev::getProperty(self, "__broAutodiffId");
        if (idVal.isNumber()) {
            return lookupAutodiffEntryById(static_cast<uint32_t>(idVal.asNumber()));
        }
    }
    if (tl_activeAutodiffCallId != 0) {
        return lookupAutodiffEntryById(tl_activeAutodiffCallId);
    }
    return nullptr;
}

std::vector<float> extractAutodiffScalarArgs(std::span<const Value> args, size_t startIdx = 0) {
    std::vector<float> out;
    for (size_t i = startIdx; i < args.size(); ++i) {
        if (args[i].isNumber()) {
            out.push_back(static_cast<float>(args[i].asNumber()));
        }
    }
    return out;
}

Value hostNumberArray(const double* vals, size_t count) {
    return hostArrayOf(count, [vals](size_t i) -> Value {
        return ev::fromDouble(vals[i]);
    });
}

Value buildGradSampleObject(
    const bronze::GpuAutodiffSample& s,
    const bronze::GpuAutodiffResult& compiled) {
    ObjectBuilder obj;
    obj.set("value", ev::fromDouble(static_cast<double>(s.value)));
    obj.set("dx", ev::fromDouble(static_cast<double>(s.dx)));
    obj.set("dy", ev::fromDouble(static_cast<double>(s.dy)));
    obj.set("dz", ev::fromDouble(static_cast<double>(s.dz)));
    obj.set("nx", ev::fromDouble(static_cast<double>(s.nx)));
    obj.set("ny", ev::fromDouble(static_cast<double>(s.ny)));
    obj.set("nz", ev::fromDouble(static_cast<double>(s.nz)));
    double gvec[3] = {
        static_cast<double>(s.dx),
        static_cast<double>(s.dy),
        static_cast<double>(s.dz)
    };
    double nvec[3] = {
        static_cast<double>(s.nx),
        static_cast<double>(s.ny),
        static_cast<double>(s.nz)
    };
    obj.set("grad", hostNumberArray(gvec, 3));
    obj.set("normal", hostNumberArray(nvec, 3));
    obj.set("spirvWordCount", ev::fromDouble(static_cast<double>(compiled.spirvWords.size())));
    obj.set("provenSafe", ev::fromBool(compiled.provenSafe));
    return obj.get();
}

Value implEvalGradBatch(
    const CompiledJsAutodiffEntry& entry,
    std::span<const Value> args,
    bool useSpirv) {
    if (args.empty() || !ev::isTypedArray(args[0])) return ev::null();
    auto inInfo = ev::typedArrayInfo(args[0]);
    if (!inInfo.data || inInfo.bytesPerElement != sizeof(float)) return ev::null();

    const uint32_t paramCount = std::max(entry.compiled.scalarParamCount, 1u);
    size_t stride = paramCount;
    if (args.size() >= 2 && args[1].isNumber() && args[1].asNumber() >= 1.0) {
        stride = static_cast<size_t>(args[1].asNumber());
    }
    const size_t pointCount = inInfo.elementCount / stride;
    std::vector<float> outFloats(pointCount * 7, 0.0f);
    const float* coords = reinterpret_cast<const float*>(inInfo.data);

    if (!useSpirv) {
        entry.compiled.evalGradBatchCpu(coords, pointCount, stride, outFloats.data());
    } else {
        for (size_t i = 0; i < pointCount; ++i) {
            const float* pt = coords + i * stride;
            std::vector<float> callArgs(stride, 0.0f);
            for (size_t k = 0; k < stride; ++k) callArgs[k] = pt[k];
            auto s = entry.compiled.evalGradSpirv(callArgs);
            float* dst = outFloats.data() + i * 7;
            dst[0] = s.value;
            dst[1] = s.dx;
            dst[2] = s.dy;
            dst[3] = s.dz;
            dst[4] = s.nx;
            dst[5] = s.ny;
            dst[6] = s.nz;
        }
    }

    if (args.size() >= 3 && ev::isTypedArray(args[2])) {
        auto outInfo = ev::typedArrayInfo(args[2]);
        if (outInfo.data && outInfo.byteLength >= outFloats.size() * sizeof(float)) {
            std::memcpy(outInfo.data, outFloats.data(), outFloats.size() * sizeof(float));
            return args[2];
        }
    }
    return makeFloat32Array(outFloats);
}

bronze::GpuAutodiffSample evalAutodiffAnyArity(
    const bronze::GpuAutodiffResult& compiled,
    const std::vector<float>& scalarArgs,
    bool forceSpirv = false) {
    if (!forceSpirv && compiled.scalarParamCount <= 4 && compiled.cpuKernel.is_valid()) {
        return compiled.evalGradCpu(scalarArgs);
    }
    return compiled.evalGradSpirv(scalarArgs);
}

Value implEvalGrad(
    const CompiledJsAutodiffEntry& entry,
    std::span<const Value> args,
    bool useSpirv) {
    if (!args.empty() && ev::isTypedArray(args[0])) {
        return implEvalGradBatch(entry, args, useSpirv);
    }
    const std::vector<float> scalarArgs = extractAutodiffScalarArgs(args, 0);
    const auto sample = evalAutodiffAnyArity(entry.compiled, scalarArgs, useSpirv);
    return buildGradSampleObject(sample, entry.compiled);
}

}  // namespace

Value compileJsAutodiffValue(std::span<const Value> args) {
    if (args.empty()) return ev::null();

    std::string rawSource;
    std::string inferredName;
    if (!extractJsFunctionOrSource(args[0], &rawSource, &inferredName) || rawSource.empty()) {
        return ev::null();
    }

    bronze::GpuAutodiffOptions autodiffOpts;
    autodiffOpts.stage = brass::target::SpirvStage::Compute;
    autodiffOpts.localSizeX = 64;
    autodiffOpts.ptxSmVersion = 80;
    autodiffOpts.kernelName = inferredName;

    if (args.size() >= 2 && ev::isObject(args[1])) {
        ev::Persistent opts(args[1]);
        Value stageV = ev::getProperty(opts.get(), "stage");
        if (stageV.isString()) {
            const std::string st = ev::toUtf8(stageV);
            if (st == "vertex" || st == "vert") {
                autodiffOpts.stage = brass::target::SpirvStage::Vertex;
            } else if (st == "fragment" || st == "frag") {
                autodiffOpts.stage = brass::target::SpirvStage::Fragment;
            } else {
                autodiffOpts.stage = brass::target::SpirvStage::Compute;
            }
        }
        Value fp16V = ev::getProperty(opts.get(), "fp16");
        if (ev::isBool(fp16V)) autodiffOpts.enableRelaxedPrecision = ev::toBool(fp16V);
        Value rpV = ev::getProperty(opts.get(), "relaxedPrecision");
        if (ev::isBool(rpV)) autodiffOpts.enableRelaxedPrecision = ev::toBool(rpV);
        Value lsxV = ev::getProperty(opts.get(), "localSizeX");
        if (lsxV.isNumber()) {
            autodiffOpts.localSizeX = static_cast<uint32_t>(std::max(1.0, lsxV.asNumber()));
        }
        Value nameV = ev::getProperty(opts.get(), "kernelName");
        if (nameV.isString()) autodiffOpts.kernelName = ev::toUtf8(nameV);
        Value hfV = ev::getProperty(opts.get(), "heightfield");
        if (ev::isBool(hfV)) autodiffOpts.forceHeightfieldNormal = ev::toBool(hfV);
    }

    const std::string normalizedSource =
        normalizeJsKernelSourceString(rawSource, autodiffOpts.kernelName);
    auto entry = std::make_shared<CompiledJsAutodiffEntry>();
    entry->compiled = bronze::compileJsGpuAutodiffSource(
        normalizedSource, "autodiff_shader.js", autodiffOpts);

    const uint32_t autodiffId = s_nextAutodiffId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(autodiffRegistryMutex());
        autodiffRegistry()[autodiffId] = entry;
    }

    const char* stageStr = "compute";
    if (entry->compiled.stage == brass::target::SpirvStage::Vertex) stageStr = "vertex";
    else if (entry->compiled.stage == brass::target::SpirvStage::Fragment) stageStr = "fragment";

    ObjectBuilder out;
    out.set("__broAutodiffId", ev::fromDouble(static_cast<double>(autodiffId)));
    out.set("ok", ev::fromBool(entry->compiled.ok));
    out.set("kernelName", ev::fromUtf8(entry->compiled.kernelName));
    out.set("stage", ev::fromUtf8(stageStr));
    out.set("paramCount", ev::fromDouble(static_cast<double>(entry->compiled.scalarParamCount)));
    out.set("isHeightfield2D", ev::fromBool(entry->compiled.isHeightfield2D));
    out.set("provenSafe", ev::fromBool(entry->compiled.provenSafe));
    out.set("cpuGpuProvenEquivalent", ev::fromBool(entry->compiled.cpuGpuProvenEquivalent));
    out.set("singleSourceParityVerified", ev::fromBool(entry->compiled.singleSourceParityVerified));
    out.set("maxCpuGpuUlpDiff", ev::fromDouble(static_cast<double>(entry->compiled.maxCpuGpuUlpDiff)));
    out.set("gvnExpressionsEliminated",
            ev::fromDouble(static_cast<double>(entry->compiled.gvnExpressionsEliminated)));
    out.set("sinCallCountBeforeGvn",
            ev::fromDouble(static_cast<double>(entry->compiled.sinCallCountBeforeGvn)));
    out.set("cosCallCountBeforeGvn",
            ev::fromDouble(static_cast<double>(entry->compiled.cosCallCountBeforeGvn)));
    out.set("sinCallCountAfterGvn",
            ev::fromDouble(static_cast<double>(entry->compiled.sinCallCountAfterGvn)));
    out.set("cosCallCountAfterGvn",
            ev::fromDouble(static_cast<double>(entry->compiled.cosCallCountAfterGvn)));
    out.set("totalInstCountBeforeOpt",
            ev::fromDouble(static_cast<double>(entry->compiled.totalInstCountBeforeOpt)));
    out.set("totalInstCountAfterOpt",
            ev::fromDouble(static_cast<double>(entry->compiled.totalInstCountAfterOpt)));
    out.set("spirvWordCount",
            ev::fromDouble(static_cast<double>(entry->compiled.spirvWords.size())));
    out.set("spirvWords", makeUint32Array(entry->compiled.spirvWords));
    out.set("spirvDisassembly", ev::fromUtf8(entry->compiled.spirvDisassembly));
    out.set("ptx", ev::fromUtf8(entry->compiled.ptxAssembly));

    const auto& diags = entry->compiled.diagnostics;
    out.set("diagnostics", hostArrayOf(diags.size(), [&](size_t i) -> Value {
        return ev::fromUtf8(diags[i]);
    }));

    out.def("evalGrad", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        return e ? implEvalGrad(*e, a, false) : ev::null();
    });
    out.def("evalGradCpu", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        return e ? implEvalGrad(*e, a, false) : ev::null();
    });
    out.def("evalGradGpu", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        return e ? implEvalGrad(*e, a, true) : ev::null();
    });
    out.def("evalNormal", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        if (!e) {
            double defN[3] = {0.0, 1.0, 0.0};
            return hostNumberArray(defN, 3);
        }
        const auto s = e->compiled.evalGradCpu(extractAutodiffScalarArgs(a, 0));
        double n[3] = {
            static_cast<double>(s.nx),
            static_cast<double>(s.ny),
            static_cast<double>(s.nz)
        };
        return hostNumberArray(n, 3);
    });
    out.def("evalGradBatch", 3, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        return e ? implEvalGradBatch(*e, a, false) : ev::null();
    });
    out.def("evalGradBatchCpu", 3, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        return e ? implEvalGradBatch(*e, a, false) : ev::null();
    });
    out.def("evalBatch", 3, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        return e ? implEvalGradBatch(*e, a, false) : ev::null();
    });
    out.def("evalCpu", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        if (!e) return ev::fromDouble(0.0);
        const auto s = evalAutodiffAnyArity(e->compiled, extractAutodiffScalarArgs(a, 0), false);
        return ev::fromDouble(static_cast<double>(s.value));
    });
    out.def("evalGpu", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        if (!e) return ev::fromDouble(0.0);
        const auto s = evalAutodiffAnyArity(e->compiled, extractAutodiffScalarArgs(a, 0), true);
        return ev::fromDouble(static_cast<double>(s.value));
    });

    return out.get();
}

bool isCompiledJsAutodiffObject(Value val) {
    if (!ev::isObject(val) && !ev::isFunction(val)) return false;
    Value idVal = ev::getProperty(val, "__broAutodiffId");
    return idVal.isNumber() && lookupAutodiffEntryById(static_cast<uint32_t>(idVal.asNumber())) != nullptr;
}

bool tryEvalCompiledAutodiffHeightGrid(
    Value autodiffObj,
    float* padded,
    int paddedW,
    int paddedH,
    float cellSize,
    float worldX0,
    float worldZ0) {
    if (!padded || paddedW <= 0 || paddedH <= 0) return false;
    auto entry = lookupAutodiffEntry(autodiffObj);
    if (!entry || !entry->compiled.ok) return false;

    for (int gz = 0; gz < paddedH; ++gz) {
        const float wz = worldZ0 + static_cast<float>(gz) * cellSize;
        float* row = padded + static_cast<size_t>(gz) * static_cast<size_t>(paddedW);
        for (int gx = 0; gx < paddedW; ++gx) {
            const float wx = worldX0 + static_cast<float>(gx) * cellSize;
            row[gx] = entry->compiled.evalGradCpu(wx, wz).value;
        }
    }
    return true;
}

bool tryEvalCompiledKernelNormalAt(
    Value kernelObj,
    float x,
    float z,
    double outNormal[3]) {
    if (!outNormal) return false;
    auto entry = lookupAutodiffEntry(kernelObj);
    if (!entry || !entry->compiled.ok) return false;
    const auto s = entry->compiled.evalGradCpu(x, z);
    outNormal[0] = static_cast<double>(s.nx);
    outNormal[1] = static_cast<double>(s.ny);
    outNormal[2] = static_cast<double>(s.nz);
    return true;
}

namespace {

struct ProceduralConstraint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool hasY = false;
    bool hasZ = false;

    bool hasValue = false;
    double targetValue = 0.0;

    bool hasMinVal = false;
    double minVal = 0.0;

    bool hasMaxVal = false;
    double maxVal = 0.0;

    bool hasMaxSlope = false;
    double maxSlope = 0.0;

    bool hasTargetNormal = false;
    double targetNormal[3] = {0.0, 1.0, 0.0};

    double weight = 1.0;
};

std::vector<std::string> parseParamNamesFromSource(const std::string& src) {
    std::vector<std::string> names;
    const size_t openPos = src.find('(');
    if (openPos == std::string::npos) return names;
    const size_t closePos = src.find(')', openPos + 1);
    if (closePos == std::string::npos) return names;
    std::string cur;
    for (size_t i = openPos + 1; i <= closePos; ++i) {
        const char ch = (i < closePos) ? src[i] : ',';
        if (ch == ',') {
            size_t b = 0, e = cur.size();
            while (b < e && std::isspace(static_cast<unsigned char>(cur[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(cur[e - 1]))) --e;
            if (e > b) names.push_back(cur.substr(b, e - b));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    return names;
}

bool solveLinearDense(int n, std::vector<double>& A, std::vector<double>& b, std::vector<double>& x) {
    x.assign(static_cast<size_t>(n), 0.0);
    for (int col = 0; col < n; ++col) {
        int piv = col;
        double maxAbs = std::fabs(A[static_cast<size_t>(col * n + col)]);
        for (int r = col + 1; r < n; ++r) {
            double v = std::fabs(A[static_cast<size_t>(r * n + col)]);
            if (v > maxAbs) { maxAbs = v; piv = r; }
        }
        if (maxAbs < 1e-14) return false;
        if (piv != col) {
            for (int c = col; c < n; ++c) std::swap(A[static_cast<size_t>(col * n + c)], A[static_cast<size_t>(piv * n + c)]);
            std::swap(b[static_cast<size_t>(col)], b[static_cast<size_t>(piv)]);
        }
        const double invDiag = 1.0 / A[static_cast<size_t>(col * n + col)];
        for (int c = col; c < n; ++c) A[static_cast<size_t>(col * n + c)] *= invDiag;
        b[static_cast<size_t>(col)] *= invDiag;
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = A[static_cast<size_t>(r * n + col)];
            for (int c = col; c < n; ++c) A[static_cast<size_t>(r * n + c)] -= f * A[static_cast<size_t>(col * n + c)];
            b[static_cast<size_t>(r)] -= f * b[static_cast<size_t>(col)];
        }
    }
    x = b;
    return true;
}

void parseConstraintItem(Value itemVal, std::vector<ProceduralConstraint>& outList) {
    if (!ev::isObject(itemVal)) return;
    ev::Persistent cObj(itemVal);
    ProceduralConstraint c;
    auto readNum = [&](const char* k, double* dst) -> bool {
        Value v = ev::getProperty(cObj.get(), k);
        if (v.isNumber()) { *dst = v.asNumber(); return true; }
        return false;
    };
    Value posArr = ev::getProperty(cObj.get(), "pos");
    if (!hostIsArray(posArr) && !ev::isTypedArray(posArr)) posArr = ev::getProperty(cObj.get(), "point");
    if (hostIsArray(posArr) || ev::isTypedArray(posArr)) {
        ev::Persistent pa(posArr);
        Value lv = ev::getProperty(pa.get(), "length");
        const uint32_t len = lv.isNumber() ? static_cast<uint32_t>(lv.asNumber()) : 0;
        if (len > 0) { Value e = ev::getElement(pa.get(), 0); if (e.isNumber()) c.x = e.asNumber(); }
        if (len > 1) { Value e = ev::getElement(pa.get(), 1); if (e.isNumber()) { c.y = e.asNumber(); c.hasY = true; } }
        if (len > 2) { Value e = ev::getElement(pa.get(), 2); if (e.isNumber()) { c.z = e.asNumber(); c.hasZ = true; } }
    }
    readNum("x", &c.x);
    if (readNum("y", &c.y)) c.hasY = true;
    if (readNum("z", &c.z)) c.hasZ = true;
    if (readNum("value", &c.targetValue) || readNum("targetValue", &c.targetValue) ||
        readNum("target", &c.targetValue) || readNum("height", &c.targetValue) || readNum("sdf", &c.targetValue)) {
        c.hasValue = true;
    }
    if (readNum("minValue", &c.minVal) || readNum("minVal", &c.minVal) || readNum("min", &c.minVal)) c.hasMinVal = true;
    if (readNum("maxValue", &c.maxVal) || readNum("maxVal", &c.maxVal) || readNum("max", &c.maxVal)) c.hasMaxVal = true;
    if (readNum("maxSlope", &c.maxSlope) || readNum("slope", &c.maxSlope)) c.hasMaxSlope = true;

    Value normV = ev::getProperty(cObj.get(), "targetNormal");
    if (!hostIsArray(normV) && !ev::isTypedArray(normV)) normV = ev::getProperty(cObj.get(), "normal");
    if (hostIsArray(normV) || ev::isTypedArray(normV)) {
        ev::Persistent na(normV);
        Value lv = ev::getProperty(na.get(), "length");
        const uint32_t len = lv.isNumber() ? static_cast<uint32_t>(lv.asNumber()) : 0;
        if (len >= 3) {
            for (uint32_t j = 0; j < 3; ++j) {
                Value e = ev::getElement(na.get(), j);
                c.targetNormal[j] = e.isNumber() ? e.asNumber() : 0.0;
            }
            c.hasTargetNormal = true;
        }
    }
    readNum("weight", &c.weight);
    if (c.weight <= 0.0) c.weight = 1.0;
    if (c.hasValue || c.hasMinVal || c.hasMaxVal || c.hasMaxSlope || c.hasTargetNormal) outList.push_back(c);
}

void parseConstraintArray(Value arrVal, std::vector<ProceduralConstraint>& outList) {
    if (!hostIsArray(arrVal)) return;
    ev::Persistent arr(arrVal);
    Value lenV = ev::getProperty(arr.get(), "length");
    const uint32_t len = lenV.isNumber() ? static_cast<uint32_t>(lenV.asNumber()) : 0;
    for (uint32_t i = 0; i < len; ++i) parseConstraintItem(ev::getElement(arr.get(), i), outList);
}

} // namespace

Value solveProceduralValue(std::span<const Value> args, bool defaultSdfMode) {
    if (args.empty()) return ev::null();
    std::shared_ptr<CompiledJsAutodiffEntry> entry = lookupAutodiffEntry(args[0]);
    std::string rawSource, inferredName;
    if (!entry) {
        if (!extractJsFunctionOrSource(args[0], &rawSource, &inferredName) || rawSource.empty()) return ev::null();
        const std::string normalizedSource = normalizeJsKernelSourceString(rawSource, inferredName);
        bronze::GpuAutodiffOptions autodiffOpts;
        autodiffOpts.stage = brass::target::SpirvStage::Compute;
        autodiffOpts.localSizeX = 64;
        autodiffOpts.ptxSmVersion = 80;
        autodiffOpts.kernelName = inferredName;
        autodiffOpts.forceHeightfieldNormal = !defaultSdfMode;
        entry = std::make_shared<CompiledJsAutodiffEntry>();
        entry->compiled = bronze::compileJsGpuAutodiffSource(normalizedSource, "solve_procedural.js", autodiffOpts);
        const uint32_t autodiffId = s_nextAutodiffId.fetch_add(1);
        std::lock_guard<std::mutex> lock(autodiffRegistryMutex());
        autodiffRegistry()[autodiffId] = entry;
    }

    const auto& compiled = entry->compiled;
    if (!compiled.ok) return ev::null();

    std::vector<ProceduralConstraint> constraints;
    std::vector<double> initialParams;
    std::vector<std::pair<double, double>> paramBounds;
    std::vector<double> seedCandidates;
    int explicitSpatialDims = 0;
    double tolerance = 1e-4;
    uint32_t maxIterations = 50;

    auto parseOptsFromObject = [&](Value objVal) {
        if (!ev::isObject(objVal) || hostIsArray(objVal)) return;
        ev::Persistent o(objVal);
        parseConstraintArray(ev::getProperty(o.get(), "targets"), constraints);
        parseConstraintArray(ev::getProperty(o.get(), "constraints"), constraints);
        parseConstraintArray(ev::getProperty(o.get(), "surfaceConstraints"), constraints);
        parseConstraintArray(ev::getProperty(o.get(), "points"), constraints);

        auto readNumArr = [&](const char* key, std::vector<double>& dst) {
            Value v = ev::getProperty(o.get(), key);
            if (!hostIsArray(v) && !ev::isTypedArray(v)) return;
            ev::Persistent a(v);
            Value lv = ev::getProperty(a.get(), "length");
            const uint32_t len = lv.isNumber() ? static_cast<uint32_t>(lv.asNumber()) : 0;
            dst.clear();
            for (uint32_t i = 0; i < len; ++i) {
                Value e = ev::getElement(a.get(), i);
                if (e.isNumber()) dst.push_back(e.asNumber());
            }
        };
        readNumArr("initialParams", initialParams);
        if (initialParams.empty()) readNumArr("params", initialParams);
        if (initialParams.empty()) readNumArr("initParams", initialParams);
        readNumArr("seedCandidates", seedCandidates);
        if (seedCandidates.empty()) readNumArr("seeds", seedCandidates);
        if (seedCandidates.empty()) readNumArr("candidates", seedCandidates);

        Value srV = ev::getProperty(o.get(), "seedRange");
        if (hostIsArray(srV) && seedCandidates.empty()) {
            ev::Persistent sra(srV);
            Value s0 = ev::getElement(sra.get(), 0), s1 = ev::getElement(sra.get(), 1);
            if (s0.isNumber() && s1.isNumber()) {
                const int lo = static_cast<int>(std::round(s0.asNumber()));
                const int hi = static_cast<int>(std::round(s1.asNumber()));
                brass::verify::BVContext bv;
                auto sv = bv.make_var("seed", 32);
                auto ge = bv.make_binary(brass::verify::BVOp::Sge, sv, bv.make_const(static_cast<uint64_t>(lo), 32), 1);
                auto le = bv.make_binary(brass::verify::BVOp::Sle, sv, bv.make_const(static_cast<uint64_t>(hi), 32), 1);
                auto cond = bv.make_binary(brass::verify::BVOp::And, ge, le, 1);
                if (brass::verify::solve_nonzero(bv, cond).status == brass::verify::SolveOutcome::Sat) {
                    for (int s = lo; s <= hi && static_cast<int>(seedCandidates.size()) < 64; ++s) {
                        seedCandidates.push_back(static_cast<double>(s));
                    }
                }
            }
        }

        Value pbV = ev::getProperty(o.get(), "paramBounds");
        if (!hostIsArray(pbV)) pbV = ev::getProperty(o.get(), "bounds");
        if (hostIsArray(pbV)) {
            ev::Persistent pba(pbV);
            Value lv = ev::getProperty(pba.get(), "length");
            const uint32_t len = lv.isNumber() ? static_cast<uint32_t>(lv.asNumber()) : 0;
            paramBounds.clear();
            for (uint32_t i = 0; i < len; ++i) {
                Value bItem = ev::getElement(pba.get(), i);
                double lo = -1e6, hi = 1e6;
                if (hostIsArray(bItem) || ev::isTypedArray(bItem)) {
                    ev::Persistent ba(bItem);
                    Value e0 = ev::getElement(ba.get(), 0), e1 = ev::getElement(ba.get(), 1);
                    if (e0.isNumber()) lo = e0.asNumber();
                    if (e1.isNumber()) hi = e1.asNumber();
                } else if (ev::isObject(bItem)) {
                    ev::Persistent bo(bItem);
                    Value e0 = ev::getProperty(bo.get(), "min"), e1 = ev::getProperty(bo.get(), "max");
                    if (e0.isNumber()) lo = e0.asNumber();
                    if (e1.isNumber()) hi = e1.asNumber();
                }
                if (lo > hi) std::swap(lo, hi);
                paramBounds.push_back({lo, hi});
            }
        }

        Value dimV = ev::getProperty(o.get(), "dimensions");
        if (!dimV.isNumber()) dimV = ev::getProperty(o.get(), "spatialDims");
        if (dimV.isNumber()) explicitSpatialDims = static_cast<int>(dimV.asNumber());
        Value modeV = ev::getProperty(o.get(), "mode");
        if (modeV.isString()) {
            const std::string ms = ev::toUtf8(modeV);
            if (ms == "sdf" || ms == "3d") explicitSpatialDims = 3;
            else if (ms == "heightfield" || ms == "terrain" || ms == "2d") explicitSpatialDims = 2;
        }
        Value tolV = ev::getProperty(o.get(), "tolerance");
        if (!tolV.isNumber()) tolV = ev::getProperty(o.get(), "tol");
        if (tolV.isNumber() && tolV.asNumber() > 0.0) tolerance = tolV.asNumber();
        Value iterV = ev::getProperty(o.get(), "maxIterations");
        if (iterV.isNumber() && iterV.asNumber() >= 1.0) maxIterations = static_cast<uint32_t>(iterV.asNumber());
    };

    if (args.size() >= 2) {
        if (hostIsArray(args[1])) {
            parseConstraintArray(args[1], constraints);
        } else if (ev::isObject(args[1])) {
            parseOptsFromObject(args[1]);
        }
    }
    if (args.size() >= 3 && ev::isObject(args[2])) {
        parseOptsFromObject(args[2]);
    }

    const std::vector<std::string> paramNames = parseParamNamesFromSource(rawSource);
    const int totalScalarParams = static_cast<int>(compiled.scalarParamCount);
    const bool hasSeedSlot = !seedCandidates.empty() &&
        ((!paramNames.empty() && (paramNames.back() == "seed" || paramNames.back() == "s")) ||
         (!initialParams.empty() && (totalScalarParams == 2 + static_cast<int>(initialParams.size()) + 1 ||
                                     totalScalarParams == 3 + static_cast<int>(initialParams.size()) + 1)));

    int spatialDims = explicitSpatialDims;
    if (spatialDims != 2 && spatialDims != 3) {
        if (defaultSdfMode) {
            spatialDims = 3;
        } else if (paramNames.size() >= 3 && paramNames[0] == "x" && paramNames[1] == "y" && paramNames[2] == "z") {
            spatialDims = 3;
        } else if (paramNames.size() >= 2 && paramNames[0] == "x" && (paramNames[1] == "z" || paramNames[1] == "y")) {
            spatialDims = 2;
        } else if (!initialParams.empty()) {
            const int rem = totalScalarParams - static_cast<int>(initialParams.size()) - (hasSeedSlot ? 1 : 0);
            spatialDims = (rem == 3) ? 3 : 2;
        } else {
            spatialDims = defaultSdfMode ? 3 : 2;
        }
    }

    int numDesignParams = static_cast<int>(initialParams.size());
    if (numDesignParams == 0) {
        if (!paramBounds.empty()) {
            numDesignParams = static_cast<int>(paramBounds.size());
        } else {
            numDesignParams = std::max(0, totalScalarParams - spatialDims - (hasSeedSlot ? 1 : 0));
        }
        initialParams.resize(static_cast<size_t>(numDesignParams), 1.0);
        for (int k = 0; k < numDesignParams; ++k) {
            if (static_cast<size_t>(k) < paramBounds.size()) {
                initialParams[static_cast<size_t>(k)] =
                    0.5 * (paramBounds[static_cast<size_t>(k)].first + paramBounds[static_cast<size_t>(k)].second);
            }
        }
    }

    auto clampParams = [&](std::vector<double>& p) {
        for (size_t k = 0; k < p.size() && k < paramBounds.size(); ++k) {
            p[k] = std::clamp(p[k], paramBounds[k].first, paramBounds[k].second);
        }
    };
    clampParams(initialParams);

    if (seedCandidates.empty()) seedCandidates.push_back(0.0);

    auto evalPointWithParams = [&](double x, double y, double z,
                                   const std::vector<double>& p,
                                   double seedVal) -> bronze::GpuAutodiffSample {
        std::vector<float> callArgs;
        callArgs.reserve(static_cast<size_t>(totalScalarParams));
        if (spatialDims == 2) {
            callArgs.push_back(static_cast<float>(x));
            callArgs.push_back(static_cast<float>(z));
        } else {
            callArgs.push_back(static_cast<float>(x));
            callArgs.push_back(static_cast<float>(y));
            callArgs.push_back(static_cast<float>(z));
        }
        for (double pk : p) callArgs.push_back(static_cast<float>(pk));
        if (hasSeedSlot) callArgs.push_back(static_cast<float>(seedVal));
        return evalAutodiffAnyArity(compiled, callArgs, false);
    };

    auto evalResiduals = [&](const std::vector<double>& p,
                             double seedVal,
                             std::vector<double>& residuals,
                             double* outMaxErr) -> double {
        residuals.clear();
        double maxErr = 0.0;
        for (const auto& c : constraints) {
            const double coordZ = (spatialDims == 2) ? (c.hasZ ? c.z : c.y) : c.z;
            const auto s = evalPointWithParams(c.x, c.y, coordZ, p, seedVal);
            const double val = static_cast<double>(s.value);
            const double sw = std::sqrt(std::max(c.weight, 1e-6));

            if (c.hasValue) {
                const double diff = val - c.targetValue;
                maxErr = std::max(maxErr, std::fabs(diff));
                residuals.push_back(sw * diff);
            }
            if (c.hasMinVal) {
                const double viol = std::max(0.0, c.minVal - val);
                maxErr = std::max(maxErr, viol);
                residuals.push_back(sw * viol);
            }
            if (c.hasMaxVal) {
                const double viol = std::max(0.0, val - c.maxVal);
                maxErr = std::max(maxErr, viol);
                residuals.push_back(sw * viol);
            }
            if (c.hasMaxSlope) {
                const double gx = static_cast<double>(s.dx);
                const double gz = (spatialDims == 2)
                    ? ((compiled.scalarParamCount == 2 && compiled.isHeightfield2D)
                           ? static_cast<double>(s.dz)
                           : static_cast<double>(s.dy))
                    : static_cast<double>(s.dz);
                const double slope = std::sqrt(gx * gx + gz * gz);
                const double viol = std::max(0.0, slope - c.maxSlope);
                maxErr = std::max(maxErr, viol);
                if (slope > c.maxSlope && slope > 1e-12) {
                    const double factor = (slope - c.maxSlope) / slope;
                    residuals.push_back(sw * factor * gx);
                    residuals.push_back(sw * factor * gz);
                } else {
                    residuals.push_back(0.0);
                    residuals.push_back(0.0);
                }
            }
            if (c.hasTargetNormal) {
                double nx = static_cast<double>(s.nx);
                double ny = static_cast<double>(s.ny);
                double nz = static_cast<double>(s.nz);
                if (spatialDims == 2 && compiled.scalarParamCount > 2) {
                    const double gx = static_cast<double>(s.dx);
                    const double gz = static_cast<double>(s.dy);
                    const double invLen = 1.0 / std::sqrt(gx * gx + 1.0 + gz * gz);
                    nx = -gx * invLen;
                    ny = invLen;
                    nz = -gz * invLen;
                }
                const double enx = nx - c.targetNormal[0];
                const double eny = ny - c.targetNormal[1];
                const double enz = nz - c.targetNormal[2];
                maxErr = std::max(maxErr, std::sqrt(enx * enx + eny * eny + enz * enz));
                residuals.push_back(sw * enx);
                residuals.push_back(sw * eny);
                residuals.push_back(sw * enz);
            }
        }
        double loss = 0.0;
        for (double r : residuals) loss += r * r;
        if (outMaxErr) *outMaxErr = maxErr;
        return loss;
    };

    std::vector<double> rInit;
    double initMaxErr = 0.0;
    const double lossInitial = evalResiduals(initialParams, seedCandidates[0], rInit, &initMaxErr);

    auto optimizeContinuousParams = [&](std::vector<double>& p,
                                        double seedVal,
                                        double* outLoss,
                                        double* outMaxErr,
                                        uint32_t* outIters) {
        const int K = static_cast<int>(p.size());
        std::vector<double> r;
        double maxErr = 0.0;
        double curLoss = evalResiduals(p, seedVal, r, &maxErr);
        if (K == 0 || r.empty()) {
            if (outLoss) *outLoss = curLoss;
            if (outMaxErr) *outMaxErr = maxErr;
            if (outIters) *outIters = 0;
            return;
        }

        double lambda = 1e-3;
        uint32_t iter = 0;
        const int M = static_cast<int>(r.size());
        std::vector<double> J(static_cast<size_t>(M * K), 0.0);
        std::vector<double> rPlus, rMinus;

        for (; iter < maxIterations; ++iter) {
            if (maxErr <= tolerance || curLoss <= tolerance * tolerance) break;

            for (int k = 0; k < K; ++k) {
                const double orig = p[static_cast<size_t>(k)];
                const double h = 1e-4 * std::max(1.0, std::fabs(orig));
                p[static_cast<size_t>(k)] = orig + h;
                evalResiduals(p, seedVal, rPlus, nullptr);
                p[static_cast<size_t>(k)] = orig - h;
                evalResiduals(p, seedVal, rMinus, nullptr);
                p[static_cast<size_t>(k)] = orig;
                const double inv2h = 0.5 / h;
                for (int i = 0; i < M; ++i) {
                    J[static_cast<size_t>(i * K + k)] =
                        (rPlus[static_cast<size_t>(i)] - rMinus[static_cast<size_t>(i)]) * inv2h;
                }
            }

            std::vector<double> JTJ(static_cast<size_t>(K * K), 0.0);
            std::vector<double> negGrad(static_cast<size_t>(K), 0.0);
            for (int k1 = 0; k1 < K; ++k1) {
                double g = 0.0;
                for (int i = 0; i < M; ++i) g -= J[static_cast<size_t>(i * K + k1)] * r[static_cast<size_t>(i)];
                negGrad[static_cast<size_t>(k1)] = g;
                for (int k2 = 0; k2 < K; ++k2) {
                    double s = 0.0;
                    for (int i = 0; i < M; ++i) {
                        s += J[static_cast<size_t>(i * K + k1)] * J[static_cast<size_t>(i * K + k2)];
                    }
                    JTJ[static_cast<size_t>(k1 * K + k2)] = s;
                }
            }

            bool accepted = false;
            for (int dampTry = 0; dampTry < 6 && !accepted; ++dampTry) {
                std::vector<double> H = JTJ;
                for (int d = 0; d < K; ++d) {
                    H[static_cast<size_t>(d * K + d)] += lambda * (1.0 + JTJ[static_cast<size_t>(d * K + d)]);
                }
                std::vector<double> rhs = negGrad;
                std::vector<double> dp;
                if (!solveLinearDense(K, H, rhs, dp)) {
                    lambda *= 10.0;
                    continue;
                }

                double stepNorm = 0.0;
                for (double v : dp) stepNorm += v * v;
                if (std::sqrt(stepNorm) < 1e-10) {
                    accepted = true;
                    break;
                }

                double alpha = 1.0;
                for (int ls = 0; ls < 8; ++ls) {
                    std::vector<double> candP = p;
                    for (int k = 0; k < K; ++k) candP[static_cast<size_t>(k)] += alpha * dp[static_cast<size_t>(k)];
                    clampParams(candP);
                    std::vector<double> candR;
                    double candMaxErr = 0.0;
                    double candLoss = evalResiduals(candP, seedVal, candR, &candMaxErr);
                    if (candLoss < curLoss) {
                        p = std::move(candP);
                        r = std::move(candR);
                        curLoss = candLoss;
                        maxErr = candMaxErr;
                        lambda = std::max(1e-7, lambda * 0.3);
                        accepted = true;
                        break;
                    }
                    alpha *= 0.5;
                }
                if (!accepted) lambda *= 10.0;
            }
            if (!accepted) break;
        }

        if (outLoss) *outLoss = curLoss;
        if (outMaxErr) *outMaxErr = maxErr;
        if (outIters) *outIters = std::max(iter, 1u);
    };

    std::vector<double> bestParams = initialParams;
    double bestSeed = seedCandidates[0];
    double bestLoss = lossInitial;
    double bestMaxErr = initMaxErr;
    uint32_t totalIterations = 0;

    for (double seedCand : seedCandidates) {
        std::vector<double> candP = initialParams;
        double candLoss = 0.0, candMaxErr = 0.0;
        uint32_t candIters = 0;
        optimizeContinuousParams(candP, seedCand, &candLoss, &candMaxErr, &candIters);
        totalIterations += candIters;
        if (candLoss < bestLoss || seedCand == seedCandidates[0]) {
            bestLoss = candLoss;
            bestMaxErr = candMaxErr;
            bestParams = candP;
            bestSeed = seedCand;
        }

        // If bounded and not yet converged, probe multi-start interior points
        if (bestMaxErr > tolerance && !paramBounds.empty()) {
            for (int ms = 1; ms <= 8; ++ms) {
                std::vector<double> probeP = initialParams;
                for (size_t k = 0; k < probeP.size() && k < paramBounds.size(); ++k) {
                    // Low-discrepancy Halton-like radical inverse on prime bases
                    const int primes[6] = {2, 3, 5, 7, 11, 13};
                    const int base = primes[k % 6];
                    double f = 1.0, r = 0.0;
                    int idx = ms;
                    while (idx > 0) {
                        f /= static_cast<double>(base);
                        r += f * static_cast<double>(idx % base);
                        idx /= base;
                    }
                    probeP[k] = paramBounds[k].first + r * (paramBounds[k].second - paramBounds[k].first);
                }
                double pLoss = 0.0, pMaxErr = 0.0;
                uint32_t pIters = 0;
                optimizeContinuousParams(probeP, seedCand, &pLoss, &pMaxErr, &pIters);
                totalIterations += pIters;
                if (pLoss < bestLoss) {
                    bestLoss = pLoss;
                    bestMaxErr = pMaxErr;
                    bestParams = std::move(probeP);
                    bestSeed = seedCand;
                }
                if (bestMaxErr <= tolerance) break;
            }
        }
    }

    auto makeSample = [entry, spatialDims, bestParams, hasSeedSlot, bestSeed](std::span<const Value> a) {
        double x = (a.size() >= 1 && a[0].isNumber()) ? a[0].asNumber() : 0.0;
        double y = (a.size() >= 2 && a[1].isNumber()) ? a[1].asNumber() : 0.0;
        double z = (a.size() >= 3 && a[2].isNumber()) ? a[2].asNumber() : y;
        std::vector<float> callArgs;
        callArgs.push_back(static_cast<float>(x));
        if (spatialDims == 3) callArgs.push_back(static_cast<float>(y));
        callArgs.push_back(static_cast<float>(z));
        for (double pk : bestParams) callArgs.push_back(static_cast<float>(pk));
        if (hasSeedSlot) callArgs.push_back(static_cast<float>(bestSeed));
        return evalAutodiffAnyArity(entry->compiled, callArgs, false);
    };

    const ev::Persistent evalFn(ev::makeFunction(
        [makeSample](Value, std::span<const Value> a) -> Value {
            return ev::fromDouble(static_cast<double>(makeSample(a).value));
        }, 3));

    ObjectBuilder out;
    out.set("ok", ev::fromBool(true));
    out.set("converged", ev::fromBool(bestMaxErr <= tolerance || bestLoss <= tolerance * tolerance));
    out.set("params", hostNumberArray(bestParams.data(), bestParams.size()));
    out.set("bestSeed", ev::fromDouble(bestSeed));
    out.set("lossInitial", ev::fromDouble(lossInitial));
    out.set("lossFinal", ev::fromDouble(bestLoss));
    out.set("iterations", ev::fromDouble(static_cast<double>(std::max(totalIterations, 1u))));
    out.set("maxError", ev::fromDouble(bestMaxErr));
    out.set("provenSafe", ev::fromBool(compiled.provenSafe));
    out.set("cpuGpuProvenEquivalent", ev::fromBool(compiled.cpuGpuProvenEquivalent));
    out.set("spirvWordCount", ev::fromDouble(static_cast<double>(compiled.spirvWords.size())));
    out.set("evaluate", evalFn.get());
    out.set("evaluatedFn", evalFn.get());
    out.def("evalGrad", 3, [entry, makeSample](Value, std::span<const Value> a) -> Value {
        return buildGradSampleObject(makeSample(a), entry->compiled);
    });
    return out.get();
}

}  // namespace bro::bronze_host
