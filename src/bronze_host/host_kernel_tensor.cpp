#include "bronze_host/host_kernel_tensor.h"

#include "embed/embed.h"
#include "embed/embed_object_builder.h"

#include <brass/gpu/vulkan_driver.hpp>
#include <brokernel/api.h>
#include <brokernel/brokernel.h>
#include <brotensor/api.h>
#include <brotensor/kernel_interop.h>
#include <brotensor/runtime.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using Value = bronze::Value;

struct TensorRegEntry {
    size_t bytes = 0;
    brotensor::Device device = brotensor::Device::CPU;
    uint32_t refs = 0;
};

std::mutex& tensorRegMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<uint64_t, TensorRegEntry>& tensorRegMap() {
    static std::unordered_map<uint64_t, TensorRegEntry> map;
    return map;
}

void registerTensorVkAddress(uint64_t addr, size_t bytes, brotensor::Device dev) {
    if (addr == 0) return;
    std::lock_guard<std::mutex> lk(tensorRegMutex());
    auto& e = tensorRegMap()[addr];
    e.bytes = std::max(e.bytes, bytes);
    e.device = dev;
    ++e.refs;
}

void unregisterTensorVkAddress(uint64_t addr) {
    if (addr == 0) return;
    std::lock_guard<std::mutex> lk(tensorRegMutex());
    auto& m = tensorRegMap();
    auto it = m.find(addr);
    if (it != m.end() && --it->second.refs == 0) {
        m.erase(it);
    }
}

class TensorKernelBuffer final : public brokernel::api::GpuBuffer {
public:
    TensorKernelBuffer(Value jsTensorVal, brotensor::Tensor* t, const brotensor::interop::TensorBufferView& v)
        : jsTensor_(jsTensorVal),
          bytes_(v.bytes),
          isVulkan_(v.is_vulkan() && v.device_address != 0),
          address_(isVulkan_ ? v.device_address : 0),
          hostPtr_(!v.is_gpu() ? v.host_ptr : nullptr),
          device_(v.device) {
        (void)t;
        if (isVulkan_) {
            registerTensorVkAddress(address_, std::max<size_t>(bytes_, 4), device_);
        }
    }

    ~TensorKernelBuffer() override {
        if (isVulkan_) {
            unregisterTensorVkAddress(address_);
        }
    }

    uint64_t address() const override { return address_; }
    size_t bytes() const override { return bytes_; }
    size_t capacity() const override { return bytes_; }
    brokernel::api::BufferResidency residency() const override {
        return isVulkan_ ? brokernel::api::BufferResidency::Gpu : brokernel::api::BufferResidency::Host;
    }
    void* hostDataForCpu() override { return hostPtr_; }
    bool prepareForCpu(std::string* err) override {
        if (hostPtr_ != nullptr || bytes_ == 0) return true;
        if (err) *err = "TensorKernelBuffer is GPU-resident";
        return false;
    }

    brotensor::Tensor* tensorPtr() const {
        return brotensor::api::getTensorFromHandle(jsTensor_.get());
    }

    bool write(size_t offset, const void* data, size_t n, std::string* err) override {
        if (offset > bytes_ || n > (bytes_ - offset)) {
            if (err) *err = "TensorKernelBuffer write out of bounds";
            return false;
        }
        if (n == 0) return true;
        if (!isVulkan_ && hostPtr_) {
            std::memcpy(static_cast<uint8_t*>(hostPtr_) + offset, data, n);
            return true;
        }
        brotensor::Tensor* t = tensorPtr();
        if (!t) {
            if (err) *err = "TensorKernelBuffer backing tensor is no longer valid";
            return false;
        }
        if (offset == 0 && n == bytes_) {
            return brotensor::interop::copy_bytes_to_tensor(data, n, *t, err);
        }
        std::vector<uint8_t> staging(bytes_);
        if (!brotensor::interop::copy_bytes_from_tensor(*t, staging.data(), bytes_, err)) return false;
        std::memcpy(staging.data() + offset, data, n);
        return brotensor::interop::copy_bytes_to_tensor(staging.data(), bytes_, *t, err);
    }

    bool read(size_t offset, void* out, size_t n, std::string* err) override {
        if (offset > bytes_ || n > (bytes_ - offset)) {
            if (err) *err = "TensorKernelBuffer read out of bounds";
            return false;
        }
        if (n == 0) return true;
        if (!isVulkan_ && hostPtr_) {
            std::memcpy(out, static_cast<const uint8_t*>(hostPtr_) + offset, n);
            return true;
        }
        brotensor::Tensor* t = tensorPtr();
        if (!t) {
            if (err) *err = "TensorKernelBuffer backing tensor is no longer valid";
            return false;
        }
        if (offset == 0 && n == bytes_) {
            return brotensor::interop::copy_bytes_from_tensor(*t, out, n, err);
        }
        std::vector<uint8_t> staging(bytes_);
        if (!brotensor::interop::copy_bytes_from_tensor(*t, staging.data(), bytes_, err)) return false;
        std::memcpy(out, staging.data() + offset, n);
        return true;
    }

private:
    ev::Persistent jsTensor_;
    size_t bytes_ = 0;
    bool isVulkan_ = false;
    uint64_t address_ = 0;
    void* hostPtr_ = nullptr;
    brotensor::Device device_ = brotensor::Device::CPU;
};

bool sameTensorBacking(const brokernel::api::GpuBuffer& kb, const brotensor::Tensor& t) {
    const auto* tkb = dynamic_cast<const TensorKernelBuffer*>(&kb);
    if (!tkb) return false;
    if (tkb->tensorPtr() == &t) return true;
    const auto v = brotensor::interop::view(t);
    if (v.is_vulkan() && tkb->address() != 0 && v.device_address == tkb->address()) return true;
    if (!v.is_gpu() && tkb->address() == 0 && v.host_ptr != nullptr &&
        v.host_ptr == const_cast<TensorKernelBuffer*>(tkb)->hostDataForCpu()) {
        return true;
    }
    return false;
}

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

bool parseTensorDtype(const std::string& s, brotensor::Dtype& out) {
    if (s == "f32" || s == "fp32" || s == "float32") { out = brotensor::Dtype::FP32; return true; }
    if (s == "f64" || s == "fp64" || s == "float64") { out = brotensor::Dtype::F64; return true; }
    if (s == "i32" || s == "int32") { out = brotensor::Dtype::INT32; return true; }
    if (s == "i8" || s == "int8" || s == "u8" || s == "uint8") { out = brotensor::Dtype::INT8; return true; }
    if (s == "f16" || s == "fp16" || s == "float16") { out = brotensor::Dtype::FP16; return true; }
    if (s == "bf16" || s == "bfloat16") { out = brotensor::Dtype::BF16; return true; }
    return false;
}

bool parseTensorDevice(const std::string& s, brotensor::Device& out) {
    if (s == "cpu") { out = brotensor::Device::CPU; return true; }
    if (s == "vulkan" || s == "vk") { out = brotensor::Device::vulkan(0); return true; }
    if (s == "cuda") { out = brotensor::Device::cuda(0); return true; }
    if (s == "metal") { out = brotensor::Device::metal(0); return true; }
    if (s == "auto") { out = brotensor::default_device(); return true; }
    return false;
}

Value js_kernelFromTensor(Value, std::span<const Value> args) {
    if (args.empty()) return ev::throwTypeError("bro.kernel.fromTensor: expects (tensor, options?)");
    brotensor::Tensor* t = requireTensor(args[0], "bro.kernel.fromTensor");
    const auto view = brotensor::interop::view(*t);

    bool forceCopy = false;
    bool hasExplicitResidency = false;
    std::string residency;
    if (args.size() >= 2 && ev::isObject(args[1])) {
        ev::Persistent opts(args[1]);
        Value cval = ev::getProperty(opts.get(), "copy");
        if (!cval.isUndefined()) forceCopy = ev::toBool(cval);
        Value zval = ev::getProperty(opts.get(), "zeroCopy");
        if (!zval.isUndefined() && !ev::toBool(zval)) forceCopy = true;
        Value rval = ev::getProperty(opts.get(), "residency");
        if (ev::isString(rval)) {
            residency = ev::toUtf8(rval);
            hasExplicitResidency = true;
        }
    }

    const bool canZeroCopy = (view.is_vulkan() && view.device_address != 0) ||
                             (!view.is_gpu() && view.host_ptr != nullptr);
    if (!forceCopy && !hasExplicitResidency && canZeroCopy) {
        auto tkb = std::make_shared<TensorKernelBuffer>(args[0], t, view);
        return brokernel::api::bufferValue(std::move(tkb));
    }

    const size_t nbytes = t->bytes();
    std::vector<uint8_t> staging(nbytes);
    if (nbytes > 0) {
        std::string err;
        if (!brotensor::interop::copy_bytes_from_tensor(*t, staging.data(), nbytes, &err)) {
            return ev::throwError("bro.kernel.fromTensor: " + err);
        }
    }

    ev::Persistent kernelObj(getKernelRoot("bro.kernel.fromTensor"));
    if (!hasExplicitResidency) {
        residency = ev::toBool(ev::getProperty(kernelObj.get(), "gpu")) ? "shared" : "host";
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

Value js_kernelToTensor(Value, std::span<const Value> args) {
    if (args.size() < 2) return ev::throwTypeError("bro.kernel.toTensor: expects (buffer, shape, options?)");
    auto kb = requireKernelBuffer(args[0], "bro.kernel.toTensor");

    int rows = 1;
    int cols = 1;
    if (args[1].isNumber()) {
        rows = static_cast<int>(args[1].asNumber());
        if (rows < 0) return ev::throwRangeError("bro.kernel.toTensor: shape dimensions must be non-negative");
    } else if (ev::isObject(args[1])) {
        ev::Persistent arr(args[1]);
        Value lenV = ev::getProperty(arr.get(), "length");
        if (!lenV.isNumber()) return ev::throwTypeError("bro.kernel.toTensor: shape must be number or array");
        const uint32_t nd = static_cast<uint32_t>(lenV.asNumber());
        if (nd == 0) return ev::throwRangeError("bro.kernel.toTensor: empty shape");
        std::vector<int> dims(nd);
        for (uint32_t i = 0; i < nd; ++i) {
            const double dv = ev::toDouble(ev::getElement(arr.get(), i));
            if (dv < 0.0) return ev::throwRangeError("bro.kernel.toTensor: negative dimension");
            dims[i] = static_cast<int>(dv);
        }
        if (nd == 1) {
            rows = dims[0];
            cols = 1;
        } else {
            int lead = 1;
            for (uint32_t i = 0; i + 1 < nd; ++i) lead *= dims[i];
            rows = lead;
            cols = dims[nd - 1];
        }
    } else {
        return ev::throwTypeError("bro.kernel.toTensor: invalid shape");
    }

    brotensor::Dtype dt = brotensor::Dtype::FP32;
    brotensor::Device dev = brotensor::default_device();
    if (args.size() >= 3 && ev::isObject(args[2])) {
        ev::Persistent opts(args[2]);
        Value dVal = ev::getProperty(opts.get(), "dtype");
        if (ev::isString(dVal) && !parseTensorDtype(ev::toUtf8(dVal), dt)) {
            return ev::throwTypeError("bro.kernel.toTensor: unsupported dtype '" + ev::toUtf8(dVal) + "'");
        }
        Value devVal = ev::getProperty(opts.get(), "device");
        if (ev::isString(devVal) && !parseTensorDevice(ev::toUtf8(devVal), dev)) {
            return ev::throwTypeError("bro.kernel.toTensor: unsupported device '" + ev::toUtf8(devVal) + "'");
        }
    }

    const size_t numel = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    const size_t elemBytes = static_cast<size_t>(brotensor::dtype_size_bytes(dt));
    const size_t needBytes = numel * elemBytes;
    if (kb->bytes() < needBytes) {
        return ev::throwRangeError("bro.kernel.toTensor: KernelBuffer (" + std::to_string(kb->bytes()) +
                                   " B) is smaller than requested tensor (" + std::to_string(needBytes) + " B)");
    }

    try {
        if (needBytes == 0) {
            return brotensor::api::createGpuTensorValue(brotensor::Tensor::empty_on(dev, rows, cols, dt));
        }
        std::vector<uint8_t> raw(needBytes);
        std::string err;
        if (!kb->read(0, raw.data(), needBytes, &err)) {
            return ev::throwError("bro.kernel.toTensor: " + err);
        }
        brotensor::Tensor t = brotensor::Tensor::from_raw_bytes_on(dev, raw.data(), rows, cols, dt, needBytes);
        return brotensor::api::createGpuTensorValue(std::move(t));
    } catch (const std::exception& ex) {
        return ev::throwError(std::string("bro.kernel.toTensor: ") + ex.what());
    }
}

Value js_kernelCopyToTensor(Value, std::span<const Value> args) {
    if (args.size() < 2) return ev::throwTypeError("bro.kernel.copyToTensor: expects (buffer, tensor)");
    auto kb = requireKernelBuffer(args[0], "bro.kernel.copyToTensor");
    brotensor::Tensor* t = requireTensor(args[1], "bro.kernel.copyToTensor");
    const size_t nbytes = t->bytes();
    if (kb->bytes() < nbytes) {
        return ev::throwRangeError("bro.kernel.copyToTensor: KernelBuffer is smaller than tensor");
    }
    if (nbytes > 0 && !sameTensorBacking(*kb, *t)) {
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
    if (nbytes > 0 && !sameTensorBacking(*kb, *t)) {
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
        if (dv < 1.0 || dv > 4294967295.0) return false;
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

std::string parsePtxEntryName(const std::string& ptx, const std::string& fallback) {
    size_t pos = ptx.find(".entry");
    while (pos != std::string::npos) {
        const size_t after = pos + 6;
        if (after < ptx.size() && std::isspace(static_cast<unsigned char>(ptx[after]))) {
            size_t start = after;
            while (start < ptx.size() && std::isspace(static_cast<unsigned char>(ptx[start]))) ++start;
            size_t end = start;
            while (end < ptx.size() && !std::isspace(static_cast<unsigned char>(ptx[end])) &&
                   ptx[end] != '(' && ptx[end] != '{') {
                ++end;
            }
            if (end > start) return ptx.substr(start, end - start);
        }
        pos = ptx.find(".entry", pos + 6);
    }
    return fallback;
}

bool dispatchPtxOnBrotensor(brotensor::Device cudaDev, const brokernel::CompiledKernel& k,
                            const brokernel::GpuLaunch& launch, std::string* err) {
    const std::string& ptx = k.ensure_ptx(err);
    if (ptx.empty()) {
        if (err && err->empty()) *err = "kernel '" + k.name + "' has no PTX build";
        return false;
    }
    if (!brotensor::interop::cuda_ptx_dispatch_available(cudaDev)) {
        if (err) *err = "brotensor CUDA PTX dispatch is unavailable";
        return false;
    }
    const std::string entry = parsePtxEntryName(ptx, k.name);
    std::vector<uint64_t> paramSlots(launch.args.size());
    std::vector<void*> paramPtrs(launch.args.size());
    for (size_t i = 0; i < launch.args.size(); ++i) {
        paramSlots[i] = launch.args[i].bits();
        paramPtrs[i] = &paramSlots[i];
    }
    brotensor::interop::PtxDispatchDesc desc;
    desc.ptx = ptx.c_str();
    desc.ptx_bytes = ptx.size() + 1;
    desc.entry_name = entry.c_str();
    for (int i = 0; i < 3; ++i) {
        desc.local_size[i] = launch.local_size[i];
        desc.groups[i] = launch.groups[i];
    }
    desc.kernel_params = paramPtrs.empty() ? nullptr : paramPtrs.data();
    return brotensor::interop::dispatch_ptx(cudaDev, desc, err);
}

Value js_kernelDispatchTensors(Value, std::span<const Value> args) {
    if (args.size() < 2) {
        return ev::throwTypeError("bro.kernel.dispatchTensors: expects (kernel, gridShape, ...args)");
    }
    std::shared_ptr<const brokernel::CompiledKernel> k = brokernel::api::kernelOf(args[0]);
    if (!k) return ev::throwTypeError("bro.kernel.dispatchTensors: argument 0 must be a Kernel");
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
    bool sawCuda = false;
    bool sawCpu = false;
    brotensor::Device gpuDev = brotensor::Device::CPU;
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
                if (sawCuda || sawCpu) {
                    return ev::throwTypeError("bro.kernel.dispatchTensors: cannot mix tensors across backends in one dispatch");
                }
                if (sawVulkan && v.device.index != gpuDev.index) {
                    return ev::throwTypeError("bro.kernel.dispatchTensors: tensors must reside on the same Vulkan device");
                }
                sawVulkan = true;
                gpuDev = v.device;
                if (v.bytes > 0 && v.device_address == 0) {
                    return ev::throwError("bro.kernel.dispatchTensors: Vulkan tensor has no BDA device address");
                }
                kargs.push_back(brokernel::KernelArg::device(v.device_address, v.bytes, v.bytes));
            } else if (v.is_cuda()) {
                if (sawVulkan || sawCpu) {
                    return ev::throwTypeError("bro.kernel.dispatchTensors: cannot mix tensors across backends in one dispatch");
                }
                if (sawCuda && v.device.index != gpuDev.index) {
                    return ev::throwTypeError("bro.kernel.dispatchTensors: tensors must reside on the same CUDA device");
                }
                sawCuda = true;
                gpuDev = v.device;
                if (v.bytes > 0 && v.device_address == 0) {
                    return ev::throwError("bro.kernel.dispatchTensors: CUDA tensor has no device pointer");
                }
                kargs.push_back(brokernel::KernelArg::device(v.device_address, v.bytes, v.bytes));
            } else if (!v.is_gpu()) {
                if (sawVulkan || sawCuda) {
                    return ev::throwTypeError("bro.kernel.dispatchTensors: cannot mix tensors across backends in one dispatch");
                }
                sawCpu = true;
                kargs.push_back(brokernel::KernelArg::buffer(v.host_ptr, v.bytes));
            } else {
                return ev::throwTypeError(
                    "bro.kernel.dispatchTensors: tensor GPU backend does not support zero-copy kernel dispatch");
            }
        } else {
            kargs.push_back(makeScalarOrVecKernelArg(p, av));
        }
    }

    std::string err;
    if (sawVulkan) {
        brokernel::GpuLaunch launch;
        if (!k->gpu_launch(shape, kargs, 0, launch, &err)) {
            return ev::throwError("bro.kernel.dispatchTensors: " + err);
        }
        if (!dispatchSpirvOnBrotensor(gpuDev, k->spirv, launch, &err)) {
            return ev::throwError("bro.kernel.dispatchTensors: " + err);
        }
    } else if (sawCuda) {
        brokernel::GpuLaunch launch;
        if (!k->gpu_launch(shape, kargs, 0, launch, &err)) {
            return ev::throwError("bro.kernel.dispatchTensors: " + err);
        }
        if (!dispatchPtxOnBrotensor(gpuDev, *k, launch, &err)) {
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

bool lookupBrotensorVulkanAddress(uint64_t addr, brotensor::Device& outDev) {
    if (addr == 0) return false;
    std::lock_guard<std::mutex> lk(tensorRegMutex());
    for (const auto& [base, e] : tensorRegMap()) {
        if (addr >= base && (addr - base) < e.bytes) {
            outDev = e.device;
            return true;
        }
    }
    return false;
}

bool dispatchSpirvOnBrotensor(brotensor::Device vkDev, const brass::target::SpirvKernel& spirv,
                              const brokernel::GpuLaunch& launch, std::string* err) {
    if (spirv.words.empty()) {
        if (err) *err = "kernel '" + spirv.entry + "' has no SPIR-V build";
        return false;
    }
    if (!brotensor::interop::vulkan_spirv_dispatch_available(vkDev)) {
        if (err) *err = "brotensor Vulkan SPIR-V dispatch is unavailable";
        return false;
    }
    std::vector<uint8_t> push;
    if (std::string bad = brass::gpu::pack_push_constants(spirv, launch.args, push); !bad.empty()) {
        if (err) *err = bad;
        return false;
    }

    std::optional<brotensor::Tensor> spilledBuf;
    if (spirv.spilled_params) {
        try {
            spilledBuf.emplace(brotensor::Tensor::empty_on(
                vkDev, 1, static_cast<int>(push.size()), brotensor::Dtype::INT8));
            if (!brotensor::interop::copy_bytes_to_tensor(push.data(), push.size(), *spilledBuf, err)) {
                return false;
            }
            const uint64_t paramAddr = brotensor::interop::view(*spilledBuf).device_address;
            if (paramAddr == 0) {
                if (err) *err = "brotensor spilled parameter buffer has no BDA device address";
                return false;
            }
            push.resize(8);
            std::memcpy(push.data(), &paramAddr, 8);
        } catch (const std::exception& ex) {
            if (err) *err = ex.what();
            return false;
        }
    }

    brotensor::interop::SpirvDispatchDesc desc;
    desc.words = spirv.words.data();
    desc.word_count = spirv.words.size();
    desc.entry_name = spirv.entry.empty() ? "main" : spirv.entry.c_str();
    for (int i = 0; i < 3; ++i) {
        desc.local_size[i] = launch.local_size[i];
        desc.groups[i] = launch.groups[i];
    }
    desc.push_data = push.empty() ? nullptr : push.data();
    desc.push_bytes = static_cast<uint32_t>(push.size());
    desc.local_size_spec_constants = spirv.local_size_spec_constants;
    const bool ok = brotensor::interop::dispatch_spirv(vkDev, desc, err);
    if (ok && spilledBuf.has_value()) {
        brotensor::sync(vkDev);
    }
    return ok;
}

void installKernelTensorInterop() {
    auto bro = ev::globalValue("bro");
    if (!bro.found || !ev::isObject(bro.value)) return;
    ev::Persistent kernelObj(ev::getProperty(bro.value, "kernel"));
    if (!ev::isObject(kernelObj.get())) return;
    ev::setProperty(kernelObj.get(), "fromTensor", ev::makeFunction(js_kernelFromTensor, 2, "fromTensor"));
    ev::setProperty(kernelObj.get(), "toTensor", ev::makeFunction(js_kernelToTensor, 3, "toTensor"));
    ev::setProperty(kernelObj.get(), "copyToTensor", ev::makeFunction(js_kernelCopyToTensor, 2, "copyToTensor"));
    ev::setProperty(kernelObj.get(), "copyFromTensor", ev::makeFunction(js_kernelCopyFromTensor, 2, "copyFromTensor"));
    ev::setProperty(kernelObj.get(), "dispatchTensors", ev::makeFunction(js_kernelDispatchTensors, 2, "dispatchTensors"));
}

} // namespace bro::bronze_host
