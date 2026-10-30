#pragma once

#include "embed/embed.h"

#include <memory>

namespace bro::engine { class Engine; }
namespace bro::render { class ComputeBuffer; }

namespace bro::bronze_host {

/// The engine buffer behind a KernelBuffer (bro.kernel.createBuffer), for
/// the engine to read in place (a custom shader's `buffer`); null for any
/// other value.
std::shared_ptr<render::ComputeBuffer> computeBufferOf(bronze::Value v);

/// bro.kernel's GPU half: kernels dispatched on the engine's own Vulkan
/// device, in its graphics stream (render::VulkanCompute), into buffers the
/// rest of the engine can read. Sets brokernel's device for the main thread
/// before its realms install bro.kernel; none without a GPU (Workers have
/// none either), and then `bro.kernel.gpu` is false.
void installKernelGpu(engine::Engine& engine);

} // namespace bro::bronze_host
