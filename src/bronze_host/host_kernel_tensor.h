#pragma once
// Tensor <-> KernelBuffer interop (`bro.kernel.fromTensor`, `copyToTensor`, `copyFromTensor`).

namespace bro::bronze_host {

void installKernelTensorInterop();

} // namespace bro::bronze_host
