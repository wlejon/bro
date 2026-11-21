#include "bronze_host/host_kernel_tensor.h"

#include "embed/embed.h"
#include "embed/embed_object_builder.h"

#include <brass/gpu/vulkan_driver.hpp>
#include <brokernel/api.h>
#include <brokernel/brokernel.h>
#include <brotensor/api.h>
#include <brotensor/kernel_interop.h>

#include <algorithm>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using Value = bronze::Value;

brotensor::Tensor* requireTensor(Value v, const char* what) {
    brotensor::Tensor* t = brotensor::api::getTensorFromHandle(v);
    if (!t) ev::throwTypeError(std::string(what) + ": expected a GpuTensor");
    return t;
}

std::shared_ptr<brokernel::api::GpuBuffer> requireKernelBuffer(Value v, const char* what) {
    auto b = brokernel::api::bufferOf(v);
    if (!b) ev::throwTypeError(std::string(what) + ": expected a KernelBuffer");
    return b;
}

Value getKernelRoot(const char* what) {
    auto bro = ev::globalValue("bro");
    if (!bro.found || !ev::isObject(bro.value)) ev::throwError(std::string(what) + ": missing bro global");
    Value kernel = ev::getProperty(bro.value, "kernel");
    if (!ev::isObject(kernel)) ev::throwError(std::string(what) + ": missing bro.kernel");
    return kernel;
}

Value js_kernelFromTensor(Value, std::span<const Value> args) {
    if (args.empty()) return ev::throwTypeError("bro.kernel.fromTensor: expects (tensor, options?)");
    brotensor::Tensor* t = requireTensor(args[0], "bro.kernel.fromTensor");
    const size_t nbytes = t->bytes();

    std::vector<uint8_t> staging(nbytes);
    if (nbytes > 0) {
        std::string err;
        if (!brotensor::interop::copy_bytes_from_tensor(*t, staging.data(), nbytes, &err)) {
            return ev::throwError("bro.kernel.fromTensor: " + err);
        }
    }

    ev::Persistent kernelObj(getKernelRoot("bro.kernel.fromTensor"));
    std::string residency = ev::toBool(ev::getProperty(kernelObj.get(), "gpu")) ? "shared" : "host";
    if (args.size() >= 2 && ev::isObject(args[1])) {
        Value rval = ev::getProperty(args[1], "residency");
        if (ev::isString(rval)) residency = ev::toUtf8(rval);
    }

    ev::Persistent createFn(ev::getProperty(kernelObj.get(), "createBuffer"));
    if (!ev::isFunction(createFn.get())) {
        return ev::throwError("bro.kernel.fromTensor: bro.kernel.createBuffer unavailable");
    }

    ev::ObjectBuilder optBuilder;
    optBuilder.set("residency", residency);
    ev::Persistent optObj(optBuilder.get());
    const Value callArgs[2] = {ev::fromDouble(static_cast<double>(nbytes)), optObj.get()};
    const ev::CallResult cr = ev::call(createFn.get(), kernelObj.get(), callArgs);
    if (cr.thrown) return ev::throwValue(cr.value);

    ev::Persistent bufHold(cr.value);
    if (nbytes > 0) {
        auto kb = brokernel::api::bufferOf(bufHold.get());
        std::string err;
        if (!kb || !kb->write(0, staging.data(), nbytes, &err)) {
            return ev::throwError("bro.kernel.fromTensor: " + (err.empty() ? "write failed" : err));
        }
    }
    return bufHold.get();
}

Value js_kernelCopyToTensor(Value, std::span<const Value> args) {
    if (args.size() < 2) return ev::throwTypeError("bro.kernel.copyToTensor: expects (buffer, tensor)");
    auto kb = requireKernelBuffer(args[0], "bro.kernel.copyToTensor");
    brotensor::Tensor* t = requireTensor(args[1], "bro.kernel.copyToTensor");
    const size_t nbytes = t->bytes();
    if (kb->bytes() < nbytes) {
        return ev::throwRangeError("bro.kernel.copyToTensor: KernelBuffer is smaller than tensor");
    }
    if (nbytes > 0) {
        std::vector<uint8_t> staging(nbytes);
        std::string err;
        if (!kb->read(0, staging.data(), nbytes, &err)) {
            return ev::throwError("bro.kernel.copyToTensor: " + err);
        }
        if (!brotensor::interop::copy_bytes_to_tensor(staging.data(), nbytes, *t, &err)) {
            return ev::throwError("bro.kernel.copyToTensor: " + err);
        }
    }
    return args[1];
}

Value js_kernelCopyFromTensor(Value, std::span<const Value> args) {
    if (args.size() < 2) return ev::throwTypeError("bro.kernel.copyFromTensor: expects (tensor, buffer)");
    brotensor::Tensor* t = requireTensor(args[0], "bro.kernel.copyFromTensor");
    auto kb = requireKernelBuffer(args[1], "bro.kernel.copyFromTensor");
    const size_t nbytes = t->bytes();
    if (kb->bytes() < nbytes) {
        return ev::throwRangeError("bro.kernel.copyFromTensor: KernelBuffer is smaller than tensor");
    }
    if (nbytes > 0) {
        std::vector<uint8_t> staging(nbytes);
        std::string err;
        if (!brotensor::interop::copy_bytes_from_tensor(*t, staging.data(), nbytes, &err)) {
            return ev::throwError("bro.kernel.copyFromTensor: " + err);
        }
        if (!kb->write(0, staging.data(), nbytes, &err)) {
            return ev::throwError("bro.kernel.copyFromTensor: " + err);
        }
    }
    return args[1];
}

bool parseTensorGridShape(Value v, uint32_t gridDim, brokernel::GridShape& out) {
    auto dimVal = [](Value d, uint32_t& res) -> bool {
        if (!d.isNumber()) return false;
        const double dv = d.asNumber();
        if (dv < 1.0 || dv > 65535.0) return false;
        res = static_cast<uint32_t>(dv);
        return true;
    };
    if (v.isNumber()) {
        uint32_t w = 1;
        if (!dimVal(v, w)) return false;
        out = brokernel::GridShape(w, 1, 1);
        return true;
    }
    if (!ev::isObject(v)) return false;
    ev::Persistent arr(v);
    Value lenV = ev::getProperty(arr.get(), "length");
    if (!lenV.isNumber()) return false;
    const uint32_t n = static_cast<uint32_t>(lenV.asNumber());
    if (n == 0 || n > 3 || (gridDim > 0 && n > gridDim)) return false;
    uint32_t dims[3] = {1, 1, 1};
    for (uint32_t i = 0; i < n; ++i) {
        if (!dimVal(ev::getElement(arr.get(), i), dims[i])) return false;
    }
    out = brokernel::GridShape(dims[0], dims[1], dims[2]);
    return true;
}

brokernel::KernelArg makeScalarOrVecKernelArg(const brokernel::KernelParamInfo& p, Value v) {
    if (p.role == brokernel::ParamRole::Scalar) {
        switch (p.type.scalar) {
            case brokernel::ScalarKind::F32: return brokernel::KernelArg(static_cast<float>(ev::toDouble(v)));
            case brokernel::ScalarKind::F64: return brokernel::KernelArg(ev::toDouble(v));
            case brokernel::ScalarKind::U32: return brokernel::KernelArg(static_cast<uint32_t>(ev::toUint64(v)));
            case brokernel::ScalarKind::I64: return brokernel::KernelArg(ev::toInt64(v));
            case brokernel::ScalarKind::U64: return brokernel::KernelArg(ev::toUint64(v));
            case brokernel::ScalarKind::Bool: return brokernel::KernelArg(ev::toBool(v));
            default: return brokernel::KernelArg(static_cast<int32_t>(ev::toInt64(v)));
        }
    }
    const uint32_t dim = p.type.dimension;
    if (!ev::isObject(v)) {
        ev::throwTypeError("bro.kernel.dispatchTensors: parameter '" + p.name + "' requires a vector array");
        return brokernel::KernelArg(0);
    }
    ev::Persistent arr(v);
    double vals[4] = {0.0, 0.0, 0.0, 0.0};
    for (uint32_t c = 0; c < std::min(dim, 4u); ++c) {
        vals[c] = ev::toDouble(ev::getElement(arr.get(), c));
    }
    if (p.type.scalar == brokernel::ScalarKind::F64) {
        if (dim == 2) return brokernel::KernelArg::vec2d(vals[0], vals[1]);
        if (dim == 3) return brokernel::KernelArg::vec3d(vals[0], vals[1], vals[2]);
        return brokernel::KernelArg::vec4d(vals[0], vals[1], vals[2], vals[3]);
    }
    if (p.type.scalar == brokernel::ScalarKind::I32 || p.type.scalar == brokernel::ScalarKind::U32) {
        if (dim == 2) return brokernel::KernelArg::vec2i(int32_t(vals[0]), int32_t(vals[1]));
        if (dim == 3) return brokernel::KernelArg::vec3i(int32_t(vals[0]), int32_t(vals[1]), int32_t(vals[2]));
        return brokernel::KernelArg::vec4i(int32_t(vals[0]), int32_t(vals[1]), int32_t(vals[2]), int32_t(vals[3]));
    }
    if (dim == 2) return brokernel::KernelArg::vec2f(float(vals[0]), float(vals[1]));
    if (dim == 3) return brokernel::KernelArg::vec3f(float(vals[0]), float(vals[1]), float(vals[2]));
    return brokernel::KernelArg::vec4f(float(vals[0]), float(vals[1]), float(vals[2]), float(vals[3]));
}

Value js_kernelDispatchTensors(Value, std::span<const Value> args) {
    if (args.size() < 2) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: expects (kernel, gridShape, ...args)");
    }
    std::shared_ptr<const brokernel::CompiledKernel> k = brokernel::api::kernelOf(args[0]);
    if (!k) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: argument 0 must be a Kernel");
    }
    if (k->stage != brokernel::Stage::Compute || k->grid_dim == 0) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: kernel must be a @compute @grid(D) kernel");
    }
    if (!k->return_type.is_void()) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: kernel must write outputs to tensor store(s) and return void");
    }
    if (!k->block_bindings.empty()) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: descriptor-bound @binding uniforms/buffers are not supported");
    }

    brokernel::GridShape shape;
    if (!parseTensorGridShape(args[1], k->grid_dim, shape)) {
        return ev::throwRangeError("bro.kernel.dispatchTensors: invalid gridShape");
    }

    std::vector<const brokernel::KernelParamInfo*> passed;
    for (const auto& p : k->params) {
        if (!brokernel::is_bound_param(p.role)) passed.push_back(&p);
    }
    if (args.size() - 2 != passed.size()) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: kernel '" + k->name + "' expects " +
                                  std::to_string(passed.size()) + " argument(s)");
    }

    bool sawVulkan = false;
    bool sawCpu = false;
    brotensor::Device vkDev = brotensor::Device::CPU;
    std::vector<brokernel::KernelArg> kargs;
    kargs.reserve(passed.size());

    std::vector<ev::Persistent> heldArgs;
    heldArgs.reserve(passed.size());
    for (size_t i = 0; i < passed.size(); ++i) heldArgs.emplace_back(args[2 + i]);

    for (size_t i = 0; i < passed.size(); ++i) {
        const brokernel::KernelParamInfo& p = *passed[i];
        Value av = heldArgs[i].get();
        if (p.role == brokernel::ParamRole::Store || p.role == brokernel::ParamRole::Stencil) {
            brotensor::Tensor* t = requireTensor(av, "bro.kernel.dispatchTensors");
            const auto v = brotensor::interop::view(*t);
            if (v.is_vulkan()) {
                if (sawVulkan && v.device.index != vkDev.index) {
                    return ev::throwTypeError("bro.kernel.dispatchTensors: tensors must reside on the same Vulkan device");
                }
                sawVulkan = true;
                vkDev = v.device;
                if (v.bytes > 0 && v.device_address == 0) {
                    return ev::throwError("bro.kernel.dispatchTensors: Vulkan tensor has no BDA device address");
                }
                kargs.push_back(brokernel::KernelArg::device(v.device_address, v.bytes, v.bytes));
            } else if (!v.is_gpu()) {
                sawCpu = true;
                kargs.push_back(brokernel::KernelArg::buffer(v.host_ptr, v.bytes));
            } else {
                return ev::throwTypeError("bro.kernel.dispatchTensors: unsupported tensor backend (expected CPU or Vulkan)");
            }
        } else {
            kargs.push_back(makeScalarOrVecKernelArg(p, av));
        }
    }

    if (sawVulkan && sawCpu) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: cannot mix CPU and Vulkan tensors in one dispatch");
    }

    std::string err;
    if (sawVulkan) {
        if (k->spirv.words.empty()) {
            return ev::throwError("bro.kernel.dispatchTensors: kernel '" + k->name + "' has no SPIR-V build");
        }
        if (!brotensor::interop::vulkan_spirv_dispatch_available(vkDev)) {
            return ev::throwError("bro.kernel.dispatchTensors: brotensor Vulkan SPIR-V dispatch is unavailable");
        }
        brokernel::GpuLaunch launch;
        if (!k->gpu_launch(shape, kargs, 0, launch, &err)) {
            return ev::throwError("bro.kernel.dispatchTensors: " + err);
        }
        std::vector<uint8_t> push;
        if (std::string bad = brass::gpu::pack_push_constants(k->spirv, launch.args, push); !bad.empty()) {
            return ev::throwError("bro.kernel.dispatchTensors: " + bad);
        }
        brotensor::interop::SpirvDispatchDesc desc;
        desc.words = k->spirv.words.data();
        desc.word_count = k->spirv.words.size();
        desc.entry_name = k->spirv.entry.empty() ? "main" : k->spirv.entry.c_str();
        for (int i = 0; i < 3; ++i) {
            desc.local_size[i] = launch.local_size[i];
            desc.groups[i] = launch.groups[i];
        }
        desc.push_data = push.empty() ? nullptr : push.data();
        desc.push_bytes = static_cast<uint32_t>(push.size());
        desc.local_size_spec_constants = k->spirv.local_size_spec_constants;
        if (!brotensor::interop::dispatch_spirv(vkDev, desc, &err)) {
            return ev::throwError("bro.kernel.dispatchTensors: " + err);
        }
    } else {
        if (!k->dispatch_cpu(shape, kargs, nullptr, &err)) {
            return ev::throwError("bro.kernel.dispatchTensors: " + err);
        }
    }
    return ev::undefined();
}

} // namespace

void installKernelTensorInterop() {
    auto bro = ev::globalValue("bro");
    if (!bro.found || !ev::isObject(bro.value)) return;
    ev::Persistent kernelObj(ev::getProperty(bro.value, "kernel"));
    if (!ev::isObject(kernelObj.get())) return;
    ev::setProperty(kernelObj.get(), "fromTensor", ev::makeFunction(js_kernelFromTensor, 2, "fromTensor"));
    ev::setProperty(kernelObj.get(), "copyToTensor", ev::makeFunction(js_kernelCopyToTensor, 2, "copyToTensor"));
    ev::setProperty(kernelObj.get(), "copyFromTensor", ev::makeFunction(js_kernelCopyFromTensor, 2, "copyFromTensor"));
    ev::setProperty(kernelObj.get(), "dispatchTensors", ev::makeFunction(js_kernelDispatchTensors, 2, "dispatchTensors"));
}

} // namespace bro::bronze_host
