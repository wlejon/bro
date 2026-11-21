#include "bronze_host/host_kernel_tensor.h"

#include "embed/embed.h"
#include "embed/embed_object_builder.h"

#include <brokernel/api.h>
#include <brotensor/api.h>
#include <brotensor/kernel_interop.h>

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

} // namespace

void installKernelTensorInterop() {
    auto bro = ev::globalValue("bro");
    if (!bro.found || !ev::isObject(bro.value)) return;
    ev::Persistent kernelObj(ev::getProperty(bro.value, "kernel"));
    if (!ev::isObject(kernelObj.get())) return;
    ev::setProperty(kernelObj.get(), "fromTensor", ev::makeFunction(js_kernelFromTensor, 2, "fromTensor"));
    ev::setProperty(kernelObj.get(), "copyToTensor", ev::makeFunction(js_kernelCopyToTensor, 2, "copyToTensor"));
    ev::setProperty(kernelObj.get(), "copyFromTensor", ev::makeFunction(js_kernelCopyFromTensor, 2, "copyFromTensor"));
}

} // namespace bro::bronze_host
