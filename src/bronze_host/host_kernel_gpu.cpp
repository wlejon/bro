#include "bronze_host/host_kernel_gpu.h"

#include "engine/engine.h"
#include "render/vulkan_context.h"

#include <brass/gpu/vulkan_driver.hpp>
#include <brokernel/api.h>
#include <brokernel/brokernel.h>

#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// The engine while it runs: the device, its buffers and programs hold this,
// and the shutdown hook clears it. The context is looked up at each use,
// since realms are installed before the engine's Vulkan context comes up.
struct Device {
    engine::Engine* engine = nullptr;

    render::VulkanContext* context() const {
        render::VulkanContext* ctx = engine ? engine->vulkanContext() : nullptr;
        return ctx && ctx->isValid() && ctx->compute().available() ? ctx : nullptr;
    }
};

bool lost(const Device& d, std::string* err) {
    if (d.context()) return false;
    if (err) *err = "no GPU device";
    return true;
}

class Buffer final : public brokernel::api::GpuBuffer {
public:
    Buffer(std::shared_ptr<Device> device, std::shared_ptr<render::ComputeBuffer> buffer)
        : device_(std::move(device)), buffer_(std::move(buffer)) {}

    uint64_t address() const override { return buffer_->address(); }
    size_t bytes() const override { return static_cast<size_t>(buffer_->size()); }
    size_t capacity() const override { return static_cast<size_t>(buffer_->capacity()); }

    bool write(size_t offset, const void* data, size_t n, std::string* err) override {
        if (lost(*device_, err)) return false;
        if (device_->context()->compute().write(*buffer_, offset, data, n)) return true;
        if (err) *err = "the GPU buffer write failed";
        return false;
    }
    bool read(size_t offset, void* out, size_t n, std::string* err) override {
        if (lost(*device_, err)) return false;
        if (device_->context()->compute().read(*buffer_, offset, out, n)) return true;
        if (err) *err = "the GPU buffer read failed";
        return false;
    }

private:
    std::shared_ptr<Device> device_;
    std::shared_ptr<render::ComputeBuffer> buffer_;

public:
    const std::shared_ptr<render::ComputeBuffer>& computeBuffer() const { return buffer_; }
};

// `bytes` of a Buffer from byte `offset`: a store bounded to that range.
class View final : public brokernel::api::GpuBuffer {
public:
    View(std::shared_ptr<Buffer> buffer, size_t offset, size_t bytes)
        : buffer_(std::move(buffer)), offset_(offset), bytes_(bytes) {}

    uint64_t address() const override { return buffer_->address() + offset_; }
    size_t bytes() const override { return bytes_; }
    size_t capacity() const override { return bytes_; }
    bool write(size_t offset, const void* data, size_t n, std::string* err) override {
        return buffer_->write(offset_ + offset, data, n, err);
    }
    bool read(size_t offset, void* out, size_t n, std::string* err) override {
        return buffer_->read(offset_ + offset, out, n, err);
    }

private:
    std::shared_ptr<Buffer> buffer_;
    size_t offset_;
    size_t bytes_;
};

struct Program final : brokernel::api::GpuProgram {
    std::shared_ptr<render::ComputePipeline> pipeline;
};

// What the device can run, in brass's terms: a kernel's SPIR-V capabilities
// are checked against it (VulkanDeviceCaps::missing_for).
brass::gpu::VulkanDeviceCaps capsOf(const render::VulkanContext& ctx) {
    const render::VulkanComputeFeatures& f = ctx.computeFeatures();
    const VkPhysicalDeviceLimits& limits = ctx.deviceProperties().limits;
    brass::gpu::VulkanDeviceCaps caps;
    caps.info.name = ctx.deviceProperties().deviceName;
    caps.buffer_device_address = f.bufferDeviceAddress;
    caps.shader_int64 = f.shaderInt64;
    caps.shader_float64 = f.shaderFloat64;
    caps.shader_int16 = f.shaderInt16;
    caps.storage_buffer_8bit = f.storageBuffer8Bit;
    caps.storage_buffer_16bit = f.storageBuffer16Bit;
    caps.subgroup_basic = f.subgroupBasic;
    caps.subgroup_shuffle = f.subgroupShuffle;
    caps.buffer_float32_atomic_add = f.bufferFloat32AtomicAdd;
    caps.shared_float32_atomic_add = f.sharedFloat32AtomicAdd;
    caps.buffer_float64_atomic_add = f.bufferFloat64AtomicAdd;
    caps.buffer_int64_atomics = f.bufferInt64Atomics;
    caps.shared_int64_atomics = f.sharedInt64Atomics;
    caps.subgroup_size = f.subgroupSize;
    caps.max_push_constant_bytes = limits.maxPushConstantsSize;
    caps.max_shared_bytes = limits.maxComputeSharedMemorySize;
    caps.max_workgroup_invocations = limits.maxComputeWorkGroupInvocations;
    for (int i = 0; i < 3; ++i) caps.max_workgroup_size[i] = limits.maxComputeWorkGroupSize[i];
    return caps;
}

class KernelGpu final : public brokernel::api::GpuDevice {
public:
    explicit KernelGpu(std::shared_ptr<Device> device) : device_(std::move(device)) {}

    bool available() const override { return device_->context() != nullptr; }

    std::shared_ptr<brokernel::api::GpuBuffer> createBuffer(size_t bytes, std::string* err) override {
        if (lost(*device_, err)) return nullptr;
        auto buffer = device_->context()->compute().createBuffer(bytes, err);
        if (!buffer) return nullptr;
        return std::make_shared<Buffer>(device_, std::move(buffer));
    }

    std::shared_ptr<brokernel::api::GpuProgram> createProgram(const brokernel::CompiledKernel& kernel,
                                                              std::string* err) override {
        if (lost(*device_, err)) return nullptr;
        render::VulkanContext& ctx = *device_->context();
        const brass::target::SpirvKernel& spirv = kernel.spirv;
        if (std::string missing = capsOf(ctx).missing_for(spirv); !missing.empty()) {
            if (err) *err = "kernel '" + kernel.name + "' cannot run on " + ctx.deviceProperties().deviceName + ":\n" + missing;
            return nullptr;
        }
        render::ComputePipelineDesc desc;
        desc.spirv = spirv.words;
        desc.entry = spirv.entry;
        desc.pushConstantBytes = spirv.push_constant_bytes;
        for (int i = 0; i < 3; ++i) desc.localSize[i] = kernel.local_size[i] ? kernel.local_size[i] : 1;
        desc.localSizeSpecConstants = spirv.local_size_spec_constants;
        desc.label = kernel.name;
        auto program = std::make_shared<Program>();
        program->pipeline = ctx.compute().createPipeline(desc, err);
        if (!program->pipeline) return nullptr;
        return program;
    }

    bool dispatch(brokernel::api::GpuProgram& program, const brokernel::CompiledKernel& kernel,
                  const brokernel::GpuLaunch& launch, std::string* err) override {
        if (lost(*device_, err)) return false;
        std::vector<uint8_t> block;
        if (std::string bad = brass::gpu::pack_push_constants(kernel.spirv, launch.args, block); !bad.empty()) {
            if (err) *err = bad;
            return false;
        }
        const auto& pipeline = *static_cast<Program&>(program).pipeline;
        if (device_->context()->compute().dispatch(pipeline, block, launch.groups)) return true;
        if (err) *err = "the GPU dispatch of '" + kernel.name + "' failed";
        return false;
    }

private:
    std::shared_ptr<Device> device_;
};

// One device for every main-thread realm (the page and its iframes).
std::shared_ptr<Device> s_device;

} // namespace

std::shared_ptr<render::ComputeBuffer> computeBufferOf(bronze::Value v) {
    return computeBufferOf(brokernel::api::bufferOf(v));
}

std::shared_ptr<render::ComputeBuffer> computeBufferOf(const std::shared_ptr<brokernel::api::GpuBuffer>& buffer) {
    const auto* own = dynamic_cast<const Buffer*>(buffer.get());
    return own ? own->computeBuffer() : nullptr;
}

std::shared_ptr<brokernel::api::GpuBuffer> createEngineBuffer(size_t bytes, std::string* err) {
    if (!s_device || lost(*s_device, err)) return nullptr;
    auto buffer = s_device->context()->compute().createBuffer(bytes, err);
    if (!buffer) return nullptr;
    return std::make_shared<Buffer>(s_device, std::move(buffer));
}

std::shared_ptr<brokernel::api::GpuBuffer> engineBufferView(const std::shared_ptr<brokernel::api::GpuBuffer>& buffer,
                                                            size_t offset, size_t bytes) {
    auto own = std::dynamic_pointer_cast<Buffer>(buffer);
    if (!own || offset > own->bytes() || bytes > own->bytes() - offset) return nullptr;
    return std::make_shared<View>(std::move(own), offset, bytes);
}

std::shared_ptr<render::ComputeReadback> engineReadLater(const std::shared_ptr<brokernel::api::GpuBuffer>& buffer,
                                                         size_t offset, size_t bytes) {
    auto own = std::dynamic_pointer_cast<Buffer>(buffer);
    if (!own || !s_device || !s_device->context() || offset > own->bytes() || bytes > own->bytes() - offset)
        return nullptr;
    return s_device->context()->compute().readLater(*own->computeBuffer(), offset, bytes);
}

render::VulkanCompute* engineCompute() {
    return s_device && s_device->context() ? &s_device->context()->compute() : nullptr;
}

void installKernelGpu(engine::Engine& engine) {
    if (s_device && s_device->engine == &engine) return;
    s_device = std::make_shared<Device>();
    s_device->engine = &engine;
    brokernel::api::setGpuDevice(std::make_shared<KernelGpu>(s_device));
    engine.addShutdownHook([device = s_device] {
        device->engine = nullptr;
        brokernel::api::setGpuDevice(nullptr);
    });
}

} // namespace bro::bronze_host
