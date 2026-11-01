// WebGL's GLSL -> SPIR-V compiler (webgl/glsl_compiler.h): diagnostics and
// the memo, and the brass SPIR-V passes it can run (RelaxedPrecision
// promotion, the static compute audit), by option.

#include "webgl/glsl_compiler.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace bro::webgl;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

void testCompileAndDiagnostics() {
    const char* good =
        "#version 450\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    std::string log;
    auto spirv = compileGlslToSpirv(good, ShaderStage::Fragment, &log);
    check(spirv.size() > 5 && spirv[0] == 0x07230203u, "a fragment shader compiles to SPIR-V");
    // Memoised: the same source answers the same words.
    check(compileGlslToSpirv(good, ShaderStage::Fragment) == spirv, "the same source answers the same words");

    log.clear();
    auto bad = compileGlslToSpirv("#version 450\nvoid main() { undeclared = 1; }\n", ShaderStage::Fragment, &log);
    check(bad.empty(), "a shader that does not compile answers nothing");
    check(log.find("undeclared") != std::string::npos, "its diagnostics name the error");
}

void testRelaxedPrecisionAndComputeAudit() {
    // A fragment shader with bounded math (sin, cos, clamp, mix) and an
    // unbounded world position.
    const char* fragSrc = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() {
    float wave = sin(vUv.x * 6.2831853) * 0.5 + 0.5;
    float rim = clamp(cos(vUv.y * 3.1415926), 0.0, 1.0);
    float tonemapped = mix(wave, rim, 0.35);
    float worldDist = vWorldPos.x * 4096.0;
    outColor = vec4(tonemapped, wave * rim, clamp(worldDist, 0.0, 1.0), 1.0);
}
)";
    GlslCompileOptions fragOpts;
    fragOpts.promoteRelaxedPrecision = true;
    fragOpts.maxAbsFp16 = 1024.0f;
    fragOpts.assumeStageInputsUnitRange = true;
    const auto fragRes = GlslCompiler::compileAndOptimize(fragSrc, ShaderStage::Fragment, fragOpts);
    check(fragRes.ok(), "compileAndOptimize compiles the fragment shader");
    check(fragRes.relaxedPrecision.enabled && fragRes.relaxedPrecision.parseOk,
          "the RelaxedPrecision pass parses the fragment SPIR-V");
    check(fragRes.relaxedPrecision.promotedCount > 0, "it promotes bounded fragment float ops to FP16");

    const char* compSrc = R"(#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 0) buffer OutBuf {
    float data[];
} outBuf;
shared float tile[64];
void main() {
    uint lid = gl_LocalInvocationID.x & 63u;
    float s = sin(float(lid) * 0.1) * 0.5 + 0.5;
    tile[lid] = s;
    barrier();
    uint gid = gl_GlobalInvocationID.x;
    outBuf.data[gid] = tile[lid] / (max(s, 0.01) + 0.001);
}
)";
    GlslCompileOptions compOpts;
    compOpts.promoteRelaxedPrecision = true;
    compOpts.auditComputeShader = true;
    const auto compRes = GlslCompiler::compileAndOptimize(compSrc, ShaderStage::Compute, compOpts);
    check(compRes.ok(), "compileAndOptimize compiles the compute shader");
    check(compRes.relaxedPrecision.promotedCount > 0, "it promotes bounded compute float ops");
    check(compRes.computeAudit.audited && compRes.computeAudit.valid, "the compute audit validates the SPIR-V");
    check(compRes.computeAudit.barriersUniform && compRes.computeAudit.sharedBoundsOk &&
              compRes.computeAudit.divSqrtOk && compRes.computeAudit.provenSafe,
          "the audit proves uniform barriers, shared bounds and safe division");
    check(compRes.computeAudit.localSize[0] == 64, "the audit reads local_size_x = 64");
    check(GlslCompiler::lastRelaxedPrecisionStats().promotedCount > 0,
          "the promotion's stats are recorded");
    check(GlslCompiler::lastComputeAuditStats().provenSafe, "the audit's stats are recorded");
}

} // namespace

int main() {
    testCompileAndDiagnostics();
    testRelaxedPrecisionAndComputeAudit();
    std::printf("bro_glsl_compiler_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
