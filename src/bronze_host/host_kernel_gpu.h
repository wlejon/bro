#pragma once

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

/// bro.kernel's GPU half: kernels dispatched on the engine's own Vulkan
/// device, in its graphics stream (render::VulkanCompute), into buffers the
/// rest of the engine can read. Sets brokernel's device for the main thread
/// before its realms install bro.kernel; none without a GPU (Workers have
/// none either), and then `bro.kernel.gpu` is false.
void installKernelGpu(engine::Engine& engine);

} // namespace bro::bronze_host
