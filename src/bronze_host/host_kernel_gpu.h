#pragma once

#include "embed/embed.h"

#include <cstddef>
#include <memory>
#include <string>

namespace bro::engine { class Engine; }
namespace bro::render { class ComputeBuffer; }
namespace brokernel::api { class GpuBuffer; }

namespace bro::bronze_host {

/// The engine buffer behind a KernelBuffer (bro.kernel.createBuffer), for
/// the engine to read in place (a custom shader's `buffer`); null for any
/// other value, a view included.
std::shared_ptr<render::ComputeBuffer> computeBufferOf(bronze::Value v);

/// A KernelBuffer's storage made by the engine (brokernel::api::bufferValue
/// gives the script one), reading zero; null (and `err`) without a device.
std::shared_ptr<brokernel::api::GpuBuffer> createEngineBuffer(size_t bytes, std::string* err);

/// `bytes` of an engine buffer from byte `offset`, as a buffer of its own: a
/// store passed it stays inside that range. Null when `buffer` is not one
/// createEngineBuffer made or the range leaves it.
std::shared_ptr<brokernel::api::GpuBuffer> engineBufferView(const std::shared_ptr<brokernel::api::GpuBuffer>& buffer,
                                                            size_t offset, size_t bytes);

/// bro.kernel's GPU half: kernels dispatched on the engine's own Vulkan
/// device, in its graphics stream (render::VulkanCompute), into buffers the
/// rest of the engine can read. Sets brokernel's device for the main thread
/// before its realms install bro.kernel; none without a GPU (Workers have
/// none either), and then `bro.kernel.gpu` is false.
void installKernelGpu(engine::Engine& engine);

} // namespace bro::bronze_host
