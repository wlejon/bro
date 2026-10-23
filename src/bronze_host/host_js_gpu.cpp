#include "bronze_host/host_js_gpu.h"

#include "codegen-brass/js_gpu_kernel.h"

#include <brass/gpu/vulkan_driver.hpp>
#include <brass/target/spirv/spirv_binary_reader.hpp>
#include <brass/target/spirv/spirv_ir.hpp>
#include <brass/target/spirv_target.hpp>

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

struct CompiledJsKernelEntry {
    bronze::GpuCompileResult compiled;
    uint32_t localSizeX = 64;
    brass::target::SpirvKernel spirvKernel;
    std::unique_ptr<brass::gpu::VulkanModule> vkModule;
    bool vkLoadAttempted = false;
    bool lastUsedVulkan = false;
};

std::mutex& kernelRegistryMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::atomic<uint32_t> s_nextKernelId{1};

std::unordered_map<uint32_t, std::shared_ptr<CompiledJsKernelEntry>>& kernelRegistry() {
    static auto* r = new std::unordered_map<uint32_t, std::shared_ptr<CompiledJsKernelEntry>>();
    return *r;
}

uint32_t alignUpU32(uint32_t v, uint32_t a) {
    return a == 0 ? v : ((v + a - 1) / a) * a;
}

std::string trimWhitespace(std::string s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string normalizeJsKernelSource(const std::string& raw, const std::string& fallbackName) {
    std::string src = trimWhitespace(raw);
    if (src.empty()) return src;

    const std::string fnName = fallbackName.empty() ? "__bro_kernel" : fallbackName;

    // Normalize arrow functions `(x, z) => expr` or `x => { ... }`
    const bool startsWithComment = (src.rfind("//", 0) == 0 || src.rfind("/*", 0) == 0);
    const bool hasFunctionKw = (src.find("function ") != std::string::npos ||
                                src.find("function(") != std::string::npos);
    if (!startsWithComment && !hasFunctionKw) {
        const size_t arrowPos = src.find("=>");
        if (arrowPos != std::string::npos) {
            std::string params = trimWhitespace(src.substr(0, arrowPos));
            std::string body = trimWhitespace(src.substr(arrowPos + 2));
            if (params.empty() || params.front() != '(') {
                params = "(" + params + ")";
            }
            if (!body.empty() && body.front() == '{') {
                src = "function " + fnName + params + " " + body;
            } else {
                if (!body.empty() && body.back() == ';') body.pop_back();
                src = "function " + fnName + params + " { return (" + body + "); }";
            }
        }
    } else if (src.rfind("function(", 0) == 0) {
        src = "function " + fnName + src.substr(8);
    } else if (src.rfind("function (", 0) == 0) {
        src = "function " + fnName + src.substr(9);
    }

    if (src.find("@kernel") == std::string::npos &&
        src.find("@shader") == std::string::npos) {
        src = "// @kernel\n" + src;
    }

    auto replaceAll = [](std::string& s, std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(src, "threadId.x", "global_tid_x()");
    replaceAll(src, "threadId.y", "tid_y()");
    replaceAll(src, "threadId.z", "tid_z()");
    replaceAll(src, ": Float32Array", "");
    replaceAll(src, ": Float64Array", "");
    replaceAll(src, ": Int32Array", "");
    replaceAll(src, ": Uint32Array", "");
    replaceAll(src, ": number", "");
    return src;
}

brass::target::SpirvKernel buildSpirvKernelDescriptor(
    const bronze::GpuCompileResult& res, uint32_t defaultLocalSizeX) {
    brass::target::SpirvKernel k;
    k.entry = res.kernelName.empty() ? "main" : res.kernelName;
    k.stage = res.stage;
    k.words = res.spirvWords;
    k.local_size[0] = defaultLocalSizeX > 0 ? defaultLocalSizeX : 64;
    k.local_size[1] = 1;
    k.local_size[2] = 1;
    k.local_size_spec_constants = false;

    brass::spirv::Module mod;
    if (!res.spirvWords.empty() && brass::spirv::parse_binary(res.spirvWords, mod)) {
        if (!mod.entry_points.empty() && !mod.entry_points[0].name.empty()) {
            k.entry = mod.entry_points[0].name;
        }
        for (const auto& em : mod.execution_modes) {
            if (em.operands.size() >= 5 &&
                em.operands[1].value == static_cast<uint32_t>(spv::ExecutionModeLocalSize)) {
                k.local_size[0] = em.operands[2].value;
                k.local_size[1] = em.operands[3].value;
                k.local_size[2] = em.operands[4].value;
            }
        }

        brass::spirv::Id pcPtrTypeId = 0;
        for (const auto& g : mod.globals) {
            if (g.op == spv::OpVariable && !g.operands.empty() &&
                g.operands[0].value == static_cast<uint32_t>(spv::StorageClassPushConstant)) {
                pcPtrTypeId = g.type;
                break;
            }
        }
        brass::spirv::Id pcStructTypeId = 0;
        if (pcPtrTypeId != 0) {
            if (const auto* ptrTi = mod.type_info(pcPtrTypeId)) {
                pcStructTypeId = ptrTi->elem;
            }
        }
        if (pcStructTypeId != 0) {
            for (const auto& g : mod.globals) {
                if (g.op == spv::OpTypeStruct && g.result == pcStructTypeId) {
                    uint32_t offset = 0;
                    for (const auto& opnd : g.operands) {
                        const auto* ti = mod.type_info(opnd.value);
                        if (!ti) continue;
                        brass::Type paramType = brass::Type::f32();
                        uint32_t size = 4;
                        if (ti->op == spv::OpTypePointer) {
                            paramType = brass::Type::ptr();
                            size = 8;
                        } else if (ti->op == spv::OpTypeInt && ti->width == 64) {
                            paramType = brass::Type::i64();
                            size = 8;
                        } else if (ti->op == spv::OpTypeInt) {
                            paramType = brass::Type::i32();
                            size = 4;
                        } else if (ti->op == spv::OpTypeFloat && ti->width == 64) {
                            paramType = brass::Type::f64();
                            size = 8;
                        } else {
                            paramType = brass::Type::f32();
                            size = 4;
                        }
                        offset = alignUpU32(offset, size);
                        k.params.push_back(brass::target::SpirvParam{paramType, offset, size});
                        offset += size;
                    }
                    k.push_constant_bytes = alignUpU32(offset, 4);
                    break;
                }
            }
        }
    }
    return k;
}

brass::gpu::VulkanModule* ensureVulkanModule(CompiledJsKernelEntry& entry) {
    if (entry.vkLoadAttempted) return entry.vkModule.get();
    entry.vkLoadAttempted = true;
    if (entry.spirvKernel.stage != brass::target::SpirvStage::Compute ||
        entry.spirvKernel.words.empty() ||
        !brass::gpu::vulkan_available()) {
        return nullptr;
    }
    std::string err;
    auto loaded = brass::gpu::VulkanModule::load(entry.spirvKernel, &err);
    if (loaded.valid()) {
        entry.vkModule = std::make_unique<brass::gpu::VulkanModule>(std::move(loaded));
    }
    return entry.vkModule.get();
}

std::shared_ptr<CompiledJsKernelEntry> lookupKernelEntry(Value self) {
    if (!ev::isObject(self)) return nullptr;
    Value idVal = ev::getProperty(self, "__broKernelId");
    if (!idVal.isNumber()) return nullptr;
    const uint32_t id = static_cast<uint32_t>(idVal.asNumber());
    std::lock_guard<std::mutex> lock(kernelRegistryMutex());
    auto& reg = kernelRegistry();
    auto it = reg.find(id);
    return (it != reg.end()) ? it->second : nullptr;
}

std::vector<float> extractScalarArgs(std::span<const Value> args, size_t startIndex = 0) {
    std::vector<float> out;
    for (size_t i = startIndex; i < args.size(); ++i) {
        if (args[i].isNumber()) {
            out.push_back(static_cast<float>(args[i].asNumber()));
        }
    }
    return out;
}

Value implEvalCpu(CompiledJsKernelEntry& entry, std::span<const Value> args) {
    if (!args.empty() && ev::isTypedArray(args[0]) && entry.compiled.hasScalarReturn) {
        auto info = ev::typedArrayInfo(args[0]);
        if (info.data && info.bytesPerElement == sizeof(float)) {
            float* dst = reinterpret_cast<float*>(info.data);
            const size_t n = info.elementCount;
            const std::vector<float> extra = extractScalarArgs(args, 1);
            for (size_t i = 0; i < n; ++i) {
                std::vector<float> callArgs;
                callArgs.reserve(1 + extra.size());
                callArgs.push_back(static_cast<float>(i));
                callArgs.insert(callArgs.end(), extra.begin(), extra.end());
                dst[i] = entry.compiled.evalCpu(callArgs);
            }
            return args[0];
        }
    }
    const std::vector<float> scalarArgs = extractScalarArgs(args, 0);
    return ev::fromDouble(static_cast<double>(entry.compiled.evalCpu(scalarArgs)));
}

Value implDispatch1D(CompiledJsKernelEntry& entry, std::span<const Value> args) {
    entry.lastUsedVulkan = false;
    if (args.empty() || !ev::isTypedArray(args[0])) return ev::fromBool(false);
    auto outInfo = ev::typedArrayInfo(args[0]);
    if (!outInfo.data || outInfo.elementCount == 0) return args[0];
    const size_t count = outInfo.elementCount;

    // Path 1: Buffer compute kernel dispatched on the real Vulkan compute queue
    if (!entry.compiled.hasScalarReturn) {
        if (auto* vkMod = ensureVulkanModule(entry)) {
            const auto& params = entry.spirvKernel.params;
            if (args.size() >= params.size() && !params.empty()) {
                struct BoundBuf {
                    size_t argIndex;
                    brass::gpu::VulkanBuffer buf;
                    void* hostPtr;
                    size_t byteLen;
                };
                std::vector<BoundBuf> boundBuffers;
                std::vector<brass::gpu::VulkanArg> vkArgs;
                bool ok = true;

                for (size_t i = 0; i < params.size(); ++i) {
                    const auto& p = params[i];
                    const Value& arg = args[i];
                    if (p.type.is_pointer()) {
                        if (!ev::isTypedArray(arg)) { ok = false; break; }
                        auto ti = ev::typedArrayInfo(arg);
                        if (!ti.data || ti.byteLength == 0) { ok = false; break; }
                        auto gpuBuf = brass::gpu::VulkanBuffer::alloc(ti.byteLength);
                        if (!gpuBuf.valid() || !gpuBuf.upload(ti.data, ti.byteLength)) {
                            ok = false;
                            break;
                        }
                        vkArgs.emplace_back(gpuBuf);
                        boundBuffers.push_back({i, std::move(gpuBuf), ti.data, ti.byteLength});
                    } else if (p.type.is_i32()) {
                        const int32_t iv = arg.isNumber() ? static_cast<int32_t>(arg.asNumber()) : 0;
                        vkArgs.emplace_back(iv);
                    } else if (p.type.is_i64()) {
                        const int64_t iv = arg.isNumber() ? static_cast<int64_t>(arg.asNumber()) : 0;
                        vkArgs.emplace_back(iv);
                    } else if (p.type.is_float() && p.type.size_in_bytes() == 8) {
                        const double dv = arg.isNumber() ? arg.asNumber() : 0.0;
                        vkArgs.emplace_back(dv);
                    } else {
                        const float fv = arg.isNumber() ? static_cast<float>(arg.asNumber()) : 0.0f;
                        vkArgs.emplace_back(fv);
                    }
                }

                if (ok) {
                    const uint32_t blockSize = std::max(entry.spirvKernel.local_size[0], 1u);
                    const uint32_t gridSize = static_cast<uint32_t>((count + blockSize - 1) / blockSize);
                    brass::gpu::VulkanDispatch dispatch;
                    dispatch.grid[0] = gridSize;
                    dispatch.block[0] = blockSize;
                    std::string err;
                    if (vkMod->launch(dispatch, vkArgs, &err)) {
                        for (const auto& b : boundBuffers) {
                            b.buf.download(b.hostPtr, b.byteLen);
                        }
                        entry.lastUsedVulkan = true;
                        return args[0];
                    }
                }
            }
        }
    }

    // Path 2: Single-source scalar-return kernel batch evaluation
    if (entry.compiled.hasScalarReturn && outInfo.bytesPerElement == sizeof(float)) {
        float* dst = reinterpret_cast<float*>(outInfo.data);
        const float* inData = nullptr;
        size_t inCount = 0;
        size_t scalarStart = 1;
        if (args.size() >= 2 && ev::isTypedArray(args[1])) {
            auto inInfo = ev::typedArrayInfo(args[1]);
            if (inInfo.data && inInfo.bytesPerElement == sizeof(float)) {
                inData = reinterpret_cast<const float*>(inInfo.data);
                inCount = inInfo.elementCount;
                scalarStart = 2;
            }
        }
        const std::vector<float> extra = extractScalarArgs(args, scalarStart);

        // Execute on real Vulkan GPU if available, otherwise verified SPIR-V / CPU
        if (auto* vkMod = ensureVulkanModule(entry)) {
            auto outBuf = brass::gpu::VulkanBuffer::alloc( sizeof(float));
            if (outBuf.valid()) {
                bool allLaunched = true;
                for (size_t i = 0; i < count; ++i) {
                    const float x0 = (inData && i < inCount) ? inData[i] : static_cast<float>(i);
                    std::vector<brass::gpu::VulkanArg> vkArgs;
                    vkArgs.reserve(entry.compiled.scalarParamCount + 1);
                    if (entry.compiled.scalarParamCount >= 1) vkArgs.emplace_back(x0);
                    for (uint32_t p = 1; p < entry.compiled.scalarParamCount; ++p) {
                        const float sv = (p - 1 < extra.size()) ? extra[p - 1] : 0.0f;
                        vkArgs.emplace_back(sv);
                    }
                    vkArgs.emplace_back(outBuf);

                    brass::gpu::VulkanDispatch d;
                    d.grid[0] = 1;
                    d.block[0] = std::max(entry.spirvKernel.local_size[0], 1u);
                    std::string err;
                    if (!vkMod->launch(d, vkArgs, &err) ||
                        !outBuf.download(&dst[i], sizeof(float))) {
                        allLaunched = false;
                        break;
                    }
                }
                if (allLaunched) {
                    entry.lastUsedVulkan = true;
                    return args[0];
                }
            }
        }

        for (size_t i = 0; i < count; ++i) {
            const float x0 = (inData && i < inCount) ? inData[i] : static_cast<float>(i);
            std::vector<float> callArgs;
            callArgs.reserve(1 + extra.size());
            callArgs.push_back(x0);
            callArgs.insert(callArgs.end(), extra.begin(), extra.end());
            dst[i] = entry.compiled.evalSpirv(callArgs);
        }
        return args[0];
    }

    return args[0];
}

Value implEvalGpu(CompiledJsKernelEntry& entry, std::span<const Value> args) {
    entry.lastUsedVulkan = false;
    if (!args.empty() && ev::isTypedArray(args[0])) {
        return implDispatch1D(entry, args);
    }

    const std::vector<float> scalarArgs = extractScalarArgs(args, 0);
    if (entry.compiled.hasScalarReturn) {
        if (auto* vkMod = ensureVulkanModule(entry)) {
            auto outBuf = brass::gpu::VulkanBuffer::alloc(sizeof(float));
            if (outBuf.valid()) {
                std::vector<brass::gpu::VulkanArg> vkArgs;
                vkArgs.reserve(entry.compiled.scalarParamCount + 1);
                for (uint32_t i = 0; i < entry.compiled.scalarParamCount; ++i) {
                    const float fv = i < scalarArgs.size() ? scalarArgs[i] : 0.0f;
                    vkArgs.emplace_back(fv);
                }
                vkArgs.emplace_back(outBuf);

                brass::gpu::VulkanDispatch d;
                d.grid[0] = 1;
                d.block[0] = std::max(entry.spirvKernel.local_size[0], 1u);
                std::string err;
                float gpuVal = 0.0f;
                if (vkMod->launch(d, vkArgs, &err) && outBuf.download(&gpuVal, sizeof(float))) {
                    entry.lastUsedVulkan = true;
                    return ev::fromDouble(static_cast<double>(gpuVal));
                }
            }
        }
        return ev::fromDouble(static_cast<double>(entry.compiled.evalSpirv(scalarArgs)));
    }
    return ev::fromDouble(0.0);
}

} // namespace

std::string normalizeJsKernelSourceString(const std::string& raw, const std::string& fallbackName) {
    return normalizeJsKernelSource(raw, fallbackName);
}

bool extractJsFunctionOrSource(Value v, std::string* outSource, std::string* outName) {
    if (!outSource) return false;
    outSource->clear();
    if (outName) outName->clear();
    if (v.isString()) {
        *outSource = ev::toUtf8(v);
        return !outSource->empty();
    }
    if (ev::isFunction(v)) {
        ev::Persistent fnObj(v);
        Value nameVal = ev::getProperty(fnObj.get(), "name");
        if (nameVal.isString() && outName) *outName = ev::toUtf8(nameVal);
        Value toStrFn = ev::getProperty(fnObj.get(), "toString");
        if (ev::isFunction(toStrFn)) {
            auto r = ev::call(toStrFn, fnObj.get(), {});
            if (!r.thrown && r.value.isString()) {
                *outSource = ev::toUtf8(r.value);
            }
        }
        return !outSource->empty();
    }
    return false;
}

Value compileJsKernelValue(std::span<const Value> args, bool defaultShaderStage) {
    if (args.empty()) return ev::null();

    std::string rawSource;
    std::string inferredName;
    if (!extractJsFunctionOrSource(args[0], &rawSource, &inferredName)) return ev::null();

    bronze::GpuCompileOptions gpuOpts;
    gpuOpts.stage = brass::target::SpirvStage::Compute;
    gpuOpts.enableRelaxedPrecision = true;
    gpuOpts.localSizeX = 64;
    gpuOpts.ptxSmVersion = 80;
    gpuOpts.kernelName = inferredName;
    (void)defaultShaderStage;

    if (args.size() >= 2 && ev::isObject(args[1])) {
        ev::Persistent opts(args[1]);
        Value stageV = ev::getProperty(opts.get(), "stage");
        if (stageV.isString()) {
            const std::string st = ev::toUtf8(stageV);
            if (st == "vertex" || st == "vert") {
                gpuOpts.stage = brass::target::SpirvStage::Vertex;
            } else if (st == "fragment" || st == "frag") {
                gpuOpts.stage = brass::target::SpirvStage::Fragment;
            } else {
                gpuOpts.stage = brass::target::SpirvStage::Compute;
            }
        }
        Value fp16V = ev::getProperty(opts.get(), "fp16");
        if (ev::isBool(fp16V)) gpuOpts.enableRelaxedPrecision = ev::toBool(fp16V);
        Value rpV = ev::getProperty(opts.get(), "relaxedPrecision");
        if (ev::isBool(rpV)) gpuOpts.enableRelaxedPrecision = ev::toBool(rpV);
        Value maxFp16V = ev::getProperty(opts.get(), "maxAbsFp16");
        if (maxFp16V.isNumber()) gpuOpts.maxAbsFp16 = static_cast<float>(maxFp16V.asNumber());
        Value lsxV = ev::getProperty(opts.get(), "localSizeX");
        if (lsxV.isNumber()) gpuOpts.localSizeX = static_cast<uint32_t>(std::max(1.0, lsxV.asNumber()));
        Value smV = ev::getProperty(opts.get(), "smVersion");
        if (smV.isNumber()) gpuOpts.ptxSmVersion = static_cast<uint32_t>(smV.asNumber());
        Value nameV = ev::getProperty(opts.get(), "kernelName");
        if (nameV.isString()) gpuOpts.kernelName = ev::toUtf8(nameV);
    }

    const std::string normalizedSource = normalizeJsKernelSource(rawSource, gpuOpts.kernelName);
    auto entry = std::make_shared<CompiledJsKernelEntry>();
    entry->localSizeX = gpuOpts.localSizeX;
    entry->compiled = bronze::compileJsGpuKernelSource(normalizedSource, "kernel.js", gpuOpts);
    entry->spirvKernel = buildSpirvKernelDescriptor(entry->compiled, gpuOpts.localSizeX);

    const uint32_t kernelId = s_nextKernelId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(kernelRegistryMutex());
        kernelRegistry()[kernelId] = entry;
    }

    const char* stageStr = "compute";
    if (entry->compiled.stage == brass::target::SpirvStage::Vertex) stageStr = "vertex";
    else if (entry->compiled.stage == brass::target::SpirvStage::Fragment) stageStr = "fragment";

    ObjectBuilder out;
    out.set("__broKernelId", ev::fromDouble(static_cast<double>(kernelId)));
    out.set("ok", ev::fromBool(entry->compiled.ok));
    out.set("kernelName", ev::fromUtf8(entry->compiled.kernelName));
    out.set("stage", ev::fromUtf8(stageStr));
    out.set("paramCount", ev::fromDouble(static_cast<double>(entry->compiled.scalarParamCount)));
    out.set("hasScalarReturn", ev::fromBool(entry->compiled.hasScalarReturn));
    out.set("cpuGpuProvenEquivalent", ev::fromBool(entry->compiled.cpuGpuProvenEquivalent));
    out.set("singleSourceParityVerified", ev::fromBool(entry->compiled.singleSourceParityVerified));
    out.set("maxCpuGpuUlpDiff", ev::fromDouble(static_cast<double>(entry->compiled.maxCpuGpuUlpDiff)));
    out.set("relaxedPrecisionPromotedCount",
            ev::fromDouble(static_cast<double>(entry->compiled.relaxedPrecisionReport.promoted_count)));
    out.set("spirvWords", makeUint32Array(entry->compiled.spirvWords));
    out.set("ptx", ev::fromUtf8(entry->compiled.ptxAssembly));
    out.set("spirvDisassembly", ev::fromUtf8(entry->compiled.spirvDisassembly));

    const auto& diags = entry->compiled.diagnostics;
    out.set("diagnostics", hostArrayOf(diags.size(), [&](size_t i) -> Value {
        return ev::fromUtf8(diags[i]);
    }));

    out.accessor("lastUsedVulkanGpu", [](Value self, std::span<const Value>) -> Value {
        auto e = lookupKernelEntry(self);
        return ev::fromBool(e ? e->lastUsedVulkan : false);
    }, nullptr);

    out.def("evalCpu", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupKernelEntry(self);
        return e ? implEvalCpu(*e, a) : ev::fromDouble(0.0);
    });
    out.def("evalGpu", 4, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupKernelEntry(self);
        return e ? implEvalGpu(*e, a) : ev::fromDouble(0.0);
    });
    out.def("dispatch1D", 5, [](Value self, std::span<const Value> a) -> Value {
        auto e = lookupKernelEntry(self);
        return e ? implDispatch1D(*e, a) : ev::fromBool(false);
    });

    return out.get();
}

void attachJsGpuKernelFunctions(ObjectBuilder& b) {
    b.def("compileKernel", 2, [](Value, std::span<const Value> a) -> Value {
        return compileJsKernelValue(a, false);
    });
    b.def("compileShader", 2, [](Value, std::span<const Value> a) -> Value {
        return compileJsKernelValue(a, true);
    });
    b.def("differentiate", 2, [](Value, std::span<const Value> a) -> Value {
        return compileJsAutodiffValue(a);
    });
    b.def("compileGradShader", 2, [](Value, std::span<const Value> a) -> Value {
        return compileJsAutodiffValue(a);
    });
    b.def("solveProcedural", 3, [](Value, std::span<const Value> a) -> Value {
        return solveProceduralValue(a, false);
    });
    b.def("solveSDF", 3, [](Value, std::span<const Value> a) -> Value {
        return solveProceduralValue(a, true);
    });
    b.def("vulkanComputeAvailable", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(brass::gpu::vulkan_available());
    });
}

bool isCompiledJsKernelObject(Value val) {
    return lookupKernelEntry(val) != nullptr || isCompiledJsAutodiffObject(val);
}

bool tryEvalCompiledKernelHeightGrid(
    Value kernelObj,
    float* padded,
    int paddedW,
    int paddedH,
    float cellSize,
    float worldX0,
    float worldZ0) {
    if (!padded || paddedW <= 0 || paddedH <= 0) return false;
    auto entry = lookupKernelEntry(kernelObj);
    if (!entry) {
        return tryEvalCompiledAutodiffHeightGrid(
            kernelObj, padded, paddedW, paddedH, cellSize, worldX0, worldZ0);
    }
    if (!entry->compiled.ok || !entry->compiled.hasScalarReturn) return false;

    for (int gz = 0; gz < paddedH; ++gz) {
        const float wz = worldZ0 + static_cast<float>(gz) * cellSize;
        float* row = padded + static_cast<size_t>(gz) * static_cast<size_t>(paddedW);
        for (int gx = 0; gx < paddedW; ++gx) {
            const float wx = worldX0 + static_cast<float>(gx) * cellSize;
            row[gx] = entry->compiled.evalCpu2D(wx, wz);
        }
    }
    return true;
}

} // namespace bro::bronze_host
