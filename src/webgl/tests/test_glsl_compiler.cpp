// WebGL's GLSL -> SPIR-V compiler (webgl/glsl_compiler.h): diagnostics and
// the memo.

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

} // namespace

int main() {
    testCompileAndDiagnostics();
    std::printf("bro_glsl_compiler_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
