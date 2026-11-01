#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bro::render {

enum class ShaderStage { Vertex, Fragment, Compute };

/// Summary of brass::spirv::promote_relaxed_precision (#113) applied to a
/// compiled SPIR-V module.
struct GlslRelaxedPrecisionStats {
    bool enabled = false;
    bool parseOk = false;
    uint32_t totalFloatDefs = 0;
    uint32_t provenBoundedDefs = 0;
    uint32_t promotedCount = 0;
    uint32_t alreadyRelaxedCount = 0;
    uint32_t keptFp32Count = 0;
    std::string error;
};

/// Summary of brass::spirv::audit_compute_shader_binary (#87) applied to a
/// compiled SPIR-V compute shader module.
struct GlslComputeAuditStats {
    bool audited = false;
    bool valid = false;
    bool provenSafe = false;
    bool parseOk = false;
    bool structuralOk = false;
    bool dominanceOk = false;
    bool barriersUniform = false;
    bool sharedBoundsOk = false;
    bool divSqrtOk = false;
    uint32_t numFunctions = 0;
    uint32_t numBlocks = 0;
    uint32_t numInstructions = 0;
    uint32_t hazardCount = 0;
    uint32_t divisionCount = 0;
    uint32_t provenSafeDivisions = 0;
    uint32_t barrierCount = 0;
    std::array<uint32_t, 3> localSize = {1, 1, 1};
    std::vector<std::string> findings;
};

/// Explicit options for compiling GLSL to SPIR-V and running brass SPIR-V
/// post-processing / static verification passes.
struct GlslCompileOptions {
    bool promoteRelaxedPrecision = false;
    float maxAbsFp16 = 1024.0f;
    bool assumeStageInputsUnitRange = false;
    bool auditComputeShader = false;
    bool failOnUnprovenCompute = false;
};

/// Result of compileGlslToSpirvWithOptions / GlslCompiler::compileAndOptimize.
struct GlslCompileResult {
    std::vector<uint32_t> spirv;
    std::string errorLog;
    GlslRelaxedPrecisionStats relaxedPrecision;
    GlslComputeAuditStats computeAudit;
    bool ok() const { return !spirv.empty(); }
};

/// Compile Vulkan-flavoured GLSL (`#version 450` with explicit layouts) to
/// SPIR-V 1.0 for a Vulkan 1.0 client, in process, with glslang — the one
/// shader compiler bro has. The scene's built-in shaders go through it at
/// build time (bro_spirv_embed), and the worlds' modules and custom scene
/// shaders at run time; WebGL programs use glslang directly (acquireGlslang)
/// because linking them needs its reflection.
///
/// When `BRO_SHADER_RELAXED_PRECISION=1` is set in the environment, fragment
/// and compute shaders are automatically passed through
/// `brass::spirv::promote_relaxed_precision` (#113). When
/// `BRO_AUDIT_COMPUTE_SHADERS=1` is set, compute shaders are audited with
/// `brass::spirv::audit_compute_shader_binary` (#87).
///
/// Returns the SPIR-V words, or an empty vector with glslang's diagnostics in
/// `*log` (when given) if the source does not compile. Successful results are
/// memoised by (stage, flags, source), since the same variant is often requested
/// again (a material rebuilt, a terrain recreated). Thread-safe, and compiles
/// on several threads at once run in parallel.
std::vector<uint32_t> compileGlslToSpirv(std::string_view source, ShaderStage stage,
                                         std::string* log = nullptr);

/// Compile GLSL to SPIR-V with explicit RelaxedPrecision promotion (#113) and
/// static compute shader verification (#87) options.
GlslCompileResult compileGlslToSpirvWithOptions(
    std::string_view source, ShaderStage stage,
    const GlslCompileOptions& options = {});

/// Run brass::spirv::promote_relaxed_precision (#113) in-place on a SPIR-V
/// binary word buffer.
GlslRelaxedPrecisionStats promoteSpirvRelaxedPrecision(
    std::vector<uint32_t>& spirv,
    float maxAbsFp16 = 1024.0f,
    bool assumeStageInputsUnitRange = false);

/// Run brass::spirv::audit_compute_shader_binary (#87) on a SPIR-V compute
/// shader binary word buffer.
GlslComputeAuditStats auditComputeSpirv(const std::vector<uint32_t>& spirv);

/// High-level shader compiler facade exposing compilation, FP16 RelaxedPrecision
/// promotion, and static compute verification telemetry.
struct GlslCompiler {
    static GlslCompileResult compileAndOptimize(
        std::string_view source, ShaderStage stage,
        const GlslCompileOptions& options = {});
    static GlslRelaxedPrecisionStats promoteRelaxedPrecision(
        std::vector<uint32_t>& spirv,
        float maxAbsFp16 = 1024.0f,
        bool assumeStageInputsUnitRange = false);
    static GlslComputeAuditStats auditCompute(const std::vector<uint32_t>& spirv);
    static GlslRelaxedPrecisionStats lastRelaxedPrecisionStats();
    static GlslComputeAuditStats lastComputeAuditStats();
};

/// The source bro compiles for a GLSL file on disk: the file with each line
/// `#include "name"` replaced by the file `name` (recursively), and each of
/// `defines` made `#define NAME 1` right after the #version line, so one file
/// builds several variants. Every `name`, in the file and in what it
/// includes, is a path from `includeDir` (a directory, with or without its
/// trailing slash), or from the file's own directory when `includeDir` is
/// empty. bro_spirv_embed compiles the scene's built-in shaders from exactly
/// this text at build time, and the scene does at run time under
/// BRO_SCENE_SHADER_SOURCE, so the two agree word for word. `deps`, when
/// given, receives every file read (the input first). False, with the reason
/// in `*err`, when a file cannot be read or the text has no #version line to
/// define after.
bool preprocessGlslFile(const std::string& path, const std::vector<std::string>& defines,
                        std::string& out, std::vector<std::string>* deps = nullptr,
                        std::string* err = nullptr, const std::string& includeDir = {});

/// For code that drives glslang's C++ API itself (WebGL's program linker
/// needs its reflection): the lock compileGlslToSpirv holds, taken with the
/// process initialised. glslang may be used for as long as the returned lock
/// is held; the lock is empty (owns_lock() false) if initialisation failed.
std::unique_lock<std::mutex> acquireGlslang();

} // namespace bro::render
