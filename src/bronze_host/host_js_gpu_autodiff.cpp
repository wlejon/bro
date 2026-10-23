#include "bronze_host/host_js_gpu.h"
#include "bronze_host/host_values.h"

#include "codegen-brass/js_gpu_autodiff.h"

#include <algorithm>
#include <atomic>
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

Value implEvalGrad(
    const CompiledJsAutodiffEntry& entry,
    std::span<const Value> args,
    bool useSpirv) {
    if (!args.empty() && ev::isTypedArray(args[0])) {
        return implEvalGradBatch(entry, args, useSpirv);
    }
    const std::vector<float> scalarArgs = extractAutodiffScalarArgs(args, 0);
    const auto sample = useSpirv
                            ? entry.compiled.evalGradSpirv(scalarArgs)
                            : entry.compiled.evalGradCpu(scalarArgs);
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
        const auto s = e->compiled.evalGradCpu(extractAutodiffScalarArgs(a, 0));
        return ev::fromDouble(static_cast<double>(s.value));
    });
    out.def("evalGpu", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupAutodiffEntry(self);
        if (!e) return ev::fromDouble(0.0);
        const auto s = e->compiled.evalGradSpirv(extractAutodiffScalarArgs(a, 0));
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

}  // namespace bro::bronze_host
