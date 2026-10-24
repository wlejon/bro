#pragma once

#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"

#include <span>
#include <string>

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

/// Release every compiled kernel's Vulkan module and close brass's Vulkan
/// runtime when `engine` shuts down, while the driver is still loaded (brass's
/// own exit handler runs in libbro's DLL detach, after the driver's). Once per
/// engine; later calls for the same engine do nothing.
void registerJsGpuShutdown(engine::Engine& engine);

std::string normalizeJsKernelSourceString(
    const std::string& raw,
    const std::string& fallbackName = "__bro_kernel");

bool extractJsFunctionOrSource(
    Value v,
    std::string* outRawSource,
    std::string* outInferredName);

/// Compile a JS function or JS source string into a single-source CPU + SPIR-V + PTX
/// kernel/shader object via `bronze::compileJsGpuKernelSource` (#101, #112, #116).
Value compileJsKernelValue(std::span<const Value> args, bool defaultShaderStage = false);

/// Compile a scalar JS `@kernel` / `@shader` function into a forward-mode symbolic
/// automatic differentiation CPU + SPIR-V shader (`bro.gpu.differentiate` /
/// `bro.scene.compileGradShader`, #131).
Value compileJsAutodiffValue(std::span<const Value> args);

/// Inverse procedural generation & SDF parameter solver (`bro.scene.solveProcedural`,
/// `bro.gpu.solveProcedural`, `bro.mesh.solveSDF`, #146).
Value solveProceduralValue(std::span<const Value> args, bool defaultSdfMode = false);

/// Attach `compileKernel`, `compileShader`, `differentiate`, and `compileGradShader`
/// onto `bro.gpu` or `bro.scene`.
void attachJsGpuKernelFunctions(ObjectBuilder& b);

/// Returns true if `val` is a compiled JS GPU kernel or autodiff shader object.
bool isCompiledJsKernelObject(Value val);
bool isCompiledJsAutodiffObject(Value val);

/// Evaluate a compiled JS GPU kernel or autodiff shader object across a 2D terrain
/// height grid `(worldX, worldZ)` using the verified single-source kernel.
bool tryEvalCompiledKernelHeightGrid(
    Value kernelObj,
    float* padded,
    int paddedW,
    int paddedH,
    float cellSize,
    float worldX0,
    float worldZ0);

bool tryEvalCompiledAutodiffHeightGrid(
    Value autodiffObj,
    float* padded,
    int paddedW,
    int paddedH,
    float cellSize,
    float worldX0,
    float worldZ0);

/// Evaluate the analytical surface normal `[nx, ny, nz]` at `(x, z)` if `kernelObj`
/// is a compiled autodiff heightfield shader.
bool tryEvalCompiledKernelNormalAt(
    Value kernelObj,
    float x,
    float z,
    double outNormal[3]);

} // namespace bro::bronze_host
