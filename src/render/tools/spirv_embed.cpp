// bro_spirv_embed — the build-time half of render/glsl_compiler.h.
//
//   bro_spirv_embed <vert|frag|comp> <input.glsl> <output.h> [DEFINE...]
//
// Compiles one GLSL file with the same in-process glslang the engine uses at
// run time and writes its SPIR-V as a brace-enclosed list of 32-bit words, so
// a source file can embed it with
//
//   static const uint32_t kSpv[] =
//   #include "name.vert.spv.h"
//   ;
//
// Each DEFINE becomes `#define DEFINE 1` right after the #version line, so
// one source can build several variants.
//
// Exits nonzero, with glslang's diagnostics on stderr, if the shader does not
// compile; the output is written only on success, so a failed build leaves no
// stale header behind for the next one to pick up.

#include "render/glsl_compiler.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    using bro::render::ShaderStage;
    if (argc < 4) {
        std::fprintf(stderr, "usage: bro_spirv_embed <vert|frag|comp> <input.glsl> <output.h> [DEFINE...]\n");
        return 2;
    }
    ShaderStage stage;
    if (std::strcmp(argv[1], "vert") == 0)      stage = ShaderStage::Vertex;
    else if (std::strcmp(argv[1], "frag") == 0) stage = ShaderStage::Fragment;
    else if (std::strcmp(argv[1], "comp") == 0) stage = ShaderStage::Compute;
    else {
        std::fprintf(stderr, "bro_spirv_embed: unknown stage '%s'\n", argv[1]);
        return 2;
    }

    std::ifstream in(argv[2], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "bro_spirv_embed: cannot read %s\n", argv[2]);
        return 1;
    }
    std::stringstream source;
    source << in.rdbuf();

    std::string text = source.str();
    if (argc > 4) {
        std::string defines;
        for (int i = 4; i < argc; ++i) defines += std::string("#define ") + argv[i] + " 1\n";
        const size_t version = text.find("#version");
        const size_t lineEnd = version == std::string::npos ? std::string::npos : text.find('\n', version);
        if (lineEnd == std::string::npos) {
            std::fprintf(stderr, "bro_spirv_embed: %s has no #version line to define after\n", argv[2]);
            return 1;
        }
        text.insert(lineEnd + 1, defines);
    }

    std::string log;
    const auto spirv = bro::render::compileGlslToSpirv(text, stage, &log);
    if (spirv.empty()) {
        std::fprintf(stderr, "%s: shader compilation failed\n%s\n", argv[2], log.c_str());
        return 1;
    }

    std::ostringstream outText;
    outText << "{";
    char word[16];
    for (size_t i = 0; i < spirv.size(); ++i) {
        std::snprintf(word, sizeof(word), "0x%08x", spirv[i]);
        outText << (i ? "," : "") << ((i % 8) == 0 ? "\n" : "") << word;
    }
    outText << "\n}\n";

    std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
    out << outText.str();
    if (!out) {
        std::fprintf(stderr, "bro_spirv_embed: cannot write %s\n", argv[3]);
        return 1;
    }
    return 0;
}
