// bro.gpu — the runtime GPU-backend probe (docs/gpu-api.js), a plain object
// over brotensor's runtime: which backends this binary registered, which one
// is the default, and per-card memory, name and trim.
//
// Every entry calls `brotensor::init()` first. The CPU backend self-registers
// at static-init time but the CUDA / Metal driver probes run only from
// init(), so a read before it would report CPU on a box with a GPU — the
// silent wrong answer bro.lm and its siblings would then default to. init()
// is idempotent, and the probe is what makes the getters LAZY: an app that
// never touches ML pays for no driver load.
//
// Compiled out with BRO_WITH_TENSOR (the minimal profile links no brotensor
// at all): then `bro.gpu` is not registered, like every other compiled-out
// namespace, rather than answering a canned "cpu".

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#if BRO_WITH_TENSOR

#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

const char* backendName(brotensor::Device d) {
    switch (d.type) {
        case brotensor::DeviceType::CUDA:  return "cuda";
        case brotensor::DeviceType::Metal: return "metal";
        case brotensor::DeviceType::CPU:   return "cpu";
    }
    return "cpu";
}

// "cuda", "cuda:1", "metal", "cpu" — a backend name with an optional card
// index. Anything that is not a string (or names no backend) is the DEFAULT
// device: the probe methods never throw over their argument, so a badge can
// call `memoryInfo(whatever)` and get null rather than a stack.
brotensor::Device deviceArg(std::span<const Value> a, size_t idx) {
    brotensor::Device d = brotensor::default_device();
    if (a.size() <= idx || !ev::isString(a[idx])) return d;
    std::string spec = ev::toUtf8(a[idx]);
    int index = 0;
    const std::size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        index = std::atoi(spec.c_str() + colon + 1);
        if (index < 0) index = 0;
        spec.resize(colon);
    }
    if (spec == "cuda")       d = brotensor::Device::cuda(index);
    else if (spec == "metal") d = brotensor::Device::metal(index);
    else if (spec == "cpu")   d = brotensor::Device::cpu();
    return d;
}

bool deviceExists(brotensor::Device want) {
    for (auto d : brotensor::available_devices()) {
        if (d == want) return true;
    }
    return false;
}

Value stringArray(const std::vector<const char*>& names) {
    return hostArrayOf(names.size(), [&names](size_t i) { return ev::fromUtf8(names[i]); });
}

}  // namespace

Value makeBroGpuValue() {
    ObjectBuilder gpu;

    // "Is the default device a GPU" — consistent with `backend` by
    // construction, because both read default_device().
    gpu.accessor("available", [](Value, std::span<const Value>) {
        brotensor::init();
        return ev::fromBool(brotensor::default_device().type != brotensor::DeviceType::CPU);
    }, nullptr);

    gpu.accessor("backend", [](Value, std::span<const Value>) {
        brotensor::init();
        return ev::fromUtf8(backendName(brotensor::default_device()));
    }, nullptr);

    // One name per registered BACKEND, not per card: available_devices()
    // lists cuda:0, cuda:1, ... and a badge wants ["cuda", "cpu"].
    gpu.accessor("devices", [](Value, std::span<const Value>) {
        brotensor::init();
        std::vector<const char*> names;
        for (auto d : brotensor::available_devices()) {
            const char* name = backendName(d);
            if (!names.empty() && std::strcmp(names.back(), name) == 0) continue;
            names.push_back(name);
        }
        return stringArray(names);
    }, nullptr);

    // The build's answer, not the driver's: what this binary COULD register.
    gpu.accessor("compiledBackends", [](Value, std::span<const Value>) {
        std::vector<const char*> names{"cpu"};
#if BRO_WITH_TENSOR_CUDA
        names.push_back("cuda");
#endif
#if BRO_WITH_TENSOR_METAL
        names.push_back("metal");
#endif
        return stringArray(names);
    }, nullptr);

    // Cards per backend: 1 for cpu, the card count for a registered GPU
    // backend, 0 for one that is not registered.
    gpu.def("deviceCount", 1, [](Value, std::span<const Value> a) {
        brotensor::init();
        const brotensor::Device want = deviceArg(a, 0);
        int n = 0;
        for (auto d : brotensor::available_devices()) {
            if (d.type == want.type) ++n;
        }
        return ev::fromDouble(n);
    });

    gpu.def("memoryInfo", 1, [](Value, std::span<const Value> a) {
        brotensor::init();
        const brotensor::Device d = deviceArg(a, 0);
        if (!deviceExists(d)) return ev::null();
        std::size_t freeBytes = 0, totalBytes = 0;
        if (!brotensor::device_mem_info(d, freeBytes, totalBytes)) return ev::null();
        ObjectBuilder o;
        o.set("freeBytes", ev::fromDouble(static_cast<double>(freeBytes)));
        o.set("totalBytes", ev::fromDouble(static_cast<double>(totalBytes)));
        return o.get();
    });

    gpu.def("deviceName", 1, [](Value, std::span<const Value> a) {
        brotensor::init();
        const brotensor::Device d = deviceArg(a, 0);
        if (!deviceExists(d)) return ev::null();
        const std::string name = brotensor::device_product_name(d);
        if (name.empty()) return ev::null();
        return ev::fromUtf8(name);
    });

    gpu.def("trim", 2, [](Value, std::span<const Value> a) {
        brotensor::init();
        const brotensor::Device d = deviceArg(a, 0);
        if (!deviceExists(d)) return ev::fromBool(false);
        std::size_t keepBytes = 0;
        if (a.size() >= 2 && ev::isNumber(a[1])) {
            const double v = ev::toDouble(a[1]);
            if (v > 0) keepBytes = static_cast<std::size_t>(v);
        }
        return ev::fromBool(brotensor::device_mem_trim(d, keepBytes));
    });

    return gpu.get();
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_TENSOR
