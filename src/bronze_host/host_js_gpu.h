#pragma once

#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"

#include <span>

namespace bro::bronze_host {

/// Compile a JS function or JS source string into a single-source CPU + SPIR-V + PTX
/// kernel/shader object via `bronze::compileJsGpuKernelSource` (#101, #112, #116).
Value compileJsKernelValue(std::span<const Value> args, bool defaultShaderStage = false);

/// Attach `compileKernel` and `compileShader` onto `bro.gpu` or `bro.scene`.
void attachJsGpuKernelFunctions(ObjectBuilder& b);

/// Returns true if `val` is a compiled JS GPU kernel object created by `compileKernel` /
/// `compileShader`.
bool isCompiledJsKernelObject(Value val);

/// Evaluate a compiled JS GPU kernel object across a 2D terrain height grid `(worldX, worldZ)`
/// using the verified single-source kernel.
bool tryEvalCompiledKernelHeightGrid(
    Value kernelObj,
    float* padded,
    int paddedW,
    int paddedH,
    float cellSize,
    float worldX0,
    float worldZ0);

} // namespace bro::bronze_host
