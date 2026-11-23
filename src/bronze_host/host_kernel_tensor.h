#pragma once
// Tensor <-> KernelBuffer interop (`bro.kernel.fromTensor`, `toTensor`, `copyToTensor`, `copyFromTensor`, `dispatchTensors`).

#include <cstdint>
#include <string>

namespace brass::target { struct SpirvKernel; }
namespace brokernel { struct GpuLaunch; }
namespace brotensor { struct Device; }

namespace bro::bronze_host {

void installKernelTensorInterop();

#if BRO_WITH_TENSOR
bool lookupBrotensorVulkanAddress(uint64_t addr, brotensor::Device& outDev);
bool dispatchSpirvOnBrotensor(brotensor::Device vkDev, const brass::target::SpirvKernel& spirv,
                              const brokernel::GpuLaunch& launch, std::string* err);
#endif

} // namespace bro::bronze_host
