#include "webgl/vulkan/webgl_vk_glsl.h"
#include "webgl/vulkan/webgl_vk_spirv.h"
#include "render/glsl_compiler.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Include/Types.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace bro::webgl::vk::glsl {

namespace {

constexpr const char* kDefaultBlockName = "BroDefaultUniforms";
// glslang maps samplers from binding 0, uniform blocks from kBlockShift and
// the default block (a block of its own on Vulkan) to kDefaultBlockBinding
// plus that shift. Every binding the program ends up with is read back from
// the reflection; these only keep the three ranges apart.
constexpr int kBlockShift = 32;
constexpr int kDefaultBlockBinding = 64;

const auto kMessages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

enum class Dialect { Es100, Es300 };

bool startsWith(const std::string& s, size_t at, const char* prefix) {
    return s.compare(at, std::strlen(prefix), prefix) == 0;
}

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Whether `word` appears in `src` as a whole identifier (comments included:
// a false positive only costs an unused helper in the preamble).
bool mentions(const std::string& src, const char* word) {
    const size_t n = std::strlen(word);
    for (size_t at = src.find(word); at != std::string::npos; at = src.find(word, at + 1)) {
        const bool startOk = at == 0 || !isIdentChar(src[at - 1]);
        const bool endOk = at + n >= src.size() || !isIdentChar(src[at + n]);
        if (startOk && endOk) return true;
    }
    return false;
}

// The language a shader is written in: `#version 300 es` on its first
// directive line, else GLSL ES 1.00.
Dialect dialectOf(const std::string& src) {
    size_t at = 0;
    while (at < src.size()) {
        size_t end = src.find('\n', at);
        if (end == std::string::npos) end = src.size();
        size_t p = src.find_first_not_of(" \t\r", at);
        if (p != std::string::npos && p < end) {
            if (startsWith(src, p, "//")) {
                at = end + 1;
                continue;
            }
            if (src[p] != '#') return Dialect::Es100;
            p = src.find_first_not_of(" \t", p + 1);
            if (p == std::string::npos || !startsWith(src, p, "version")) return Dialect::Es100;
            p = src.find_first_not_of(" \t", p + 7);
            return (p != std::string::npos && startsWith(src, p, "300")) ? Dialect::Es300 : Dialect::Es100;
        }
        at = end + 1;
    }
    return Dialect::Es100;
}

// The source with the #extension lines of WebGL 1 extensions that are core
// here (and unknown to desktop GLSL) blanked, keeping line numbers.
std::string withoutEsExtensions(const std::string& src) {
    static const char* const kCoreExtensions[] = {
        "GL_OES_standard_derivatives", "GL_EXT_shader_texture_lod",
        "GL_EXT_frag_depth", "GL_EXT_draw_buffers",
    };
    std::string out = src;
    size_t at = 0;
    while (at < out.size()) {
        size_t end = out.find('\n', at);
        if (end == std::string::npos) end = out.size();
        size_t p = out.find_first_not_of(" \t", at);
        if (p != std::string::npos && p < end && out[p] == '#') {
            p = out.find_first_not_of(" \t", p + 1);
            if (p != std::string::npos && p < end && startsWith(out, p, "extension")) {
                const std::string line = out.substr(p, end - p);
                for (const char* ext : kCoreExtensions) {
                    if (line.find(ext) != std::string::npos) {
                        std::fill(out.begin() + static_cast<std::ptrdiff_t>(at),
                                  out.begin() + static_cast<std::ptrdiff_t>(end), ' ');
                        break;
                    }
                }
            }
        }
        at = end + 1;
    }
    return out;
}

// What both stages of both dialects get. GL_EXT_spirv_intrinsics is enabled
// because with it glslang accepts a macro whose name starts with GL_.
constexpr const char* kCommonPreamble =
    "#extension GL_EXT_spirv_intrinsics : enable\n"
    "#define GL_ES 1\n";

// GLSL ES 1.00 in GLSL 450 terms.
constexpr const char* kEs100Functions =
    "#define texture2D texture\n"
    "#define texture2DProj textureProj\n"
    "#define texture2DLod textureLod\n"
    "#define texture2DProjLod textureProjLod\n"
    "#define textureCube texture\n"
    "#define textureCubeLod textureLod\n"
    "#define texture2DLodEXT textureLod\n"
    "#define texture2DProjLodEXT textureProjLod\n"
    "#define textureCubeLodEXT textureLod\n"
    "#define texture2DGradEXT textureGrad\n"
    "#define texture2DProjGradEXT textureProjGrad\n"
    "#define textureCubeGradEXT textureGrad\n";

std::string preamble(EShLanguage stage, Dialect dialect, const std::string& src, const Limits& limits) {
    std::string out = kCommonPreamble;
    if (dialect == Dialect::Es100) out += kEs100Functions;
    if (stage == EShLangVertex) {
        if (dialect == Dialect::Es100) out += "#define attribute in\n#define varying out\n";
        out += "#define main bro_user_main\n";
        return out;
    }

    out += "#define GL_FRAGMENT_PRECISION_HIGH 1\n";
    if (dialect == Dialect::Es100) {
        out += "#define varying in\n#define gl_FragDepthEXT gl_FragDepth\n";
        if (mentions(src, "gl_FragData")) {
            out += "layout(location = 0) out vec4 bro_FragData[" + std::to_string(limits.maxDrawBuffers) + "];\n"
                   "#define gl_FragData bro_FragData\n";
        } else if (mentions(src, "gl_FragColor")) {
            out += "layout(location = 0) out vec4 bro_FragColor;\n#define gl_FragColor bro_FragColor\n";
        }
    }
    // GL's window space, through the FragmentPush mapping (webgl_vk_types.h).
    // A macro is not re-expanded inside its own replacement, so each one reads
    // the built-in it shadows.
    out += "layout(push_constant) uniform BroFragmentPush { float yOffset; float yScale; } bro_push;\n";
    if (mentions(src, "gl_FragCoord")) {
        out += "vec4 bro_fragCoord() {\n"
               "    return vec4(gl_FragCoord.x, bro_push.yOffset + bro_push.yScale * gl_FragCoord.y, gl_FragCoord.zw);\n"
               "}\n"
               "#define gl_FragCoord bro_fragCoord()\n";
    }
    if (mentions(src, "gl_PointCoord")) {
        out += "vec2 bro_pointCoord() {\n"
               "    return vec2(gl_PointCoord.x, 0.5 - bro_push.yScale * (gl_PointCoord.y - 0.5));\n"
               "}\n"
               "#define gl_PointCoord bro_pointCoord()\n";
    }
    out += "#define dFdy(p) (bro_push.yScale * dFdy(p))\n";
    return out;
}

// The vertex stage's real main, a second source string after the user's: a
// point is one pixel unless the shader says otherwise (Vulkan requires the
// size to be written), and GL's clip-space depth [-w, w] maps onto [0, w].
constexpr const char* kVertexEpilogue =
    "\n#undef main\n"
    "void main() {\n"
    "    gl_PointSize = 1.0;\n"
    "    bro_user_main();\n"
    "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n"
    "}\n";

// glslang's info log without the line every shader gets for compiling ES
// source as GLSL 450.
std::string cleanLog(const char* log) {
    std::string out;
    std::string text = log ? log : "";
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        if (line.find("forced to be (450") == std::string::npos && !line.empty()) out += line + "\n";
        at = end + 1;
    }
    return out;
}

// One stage's source strings and preamble, kept alive for its TShader.
struct StageSource {
    std::string preamble;
    std::string source;
    const char* strings[2]{};
    int count = 0;
};

StageSource stageSource(EShLanguage stage, const std::string& src, const Limits& limits) {
    StageSource s;
    s.source = withoutEsExtensions(src);
    s.preamble = preamble(stage, dialectOf(src), s.source, limits);
    s.strings[s.count++] = s.source.c_str();
    if (stage == EShLangVertex) s.strings[s.count++] = kVertexEpilogue;
    return s;
}

bool parse(glslang::TShader& shader, const StageSource& s, std::string& log) {
    shader.setStrings(s.strings, s.count);
    shader.setPreamble(s.preamble.c_str());
    shader.setEnvInput(glslang::EShSourceGlsl, shader.getStage(), glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setEnvInputVulkanRulesRelaxed();
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setGlobalUniformBlockName(kDefaultBlockName);
    shader.setGlobalUniformSet(0);
    shader.setGlobalUniformBinding(kDefaultBlockBinding);
    shader.setShiftBinding(glslang::EResUbo, kBlockShift);
    // Version 450 forced over the source's own #version: ES 1.00 / 3.00 are
    // not languages glslang targets Vulkan from.
    const bool ok = shader.parse(GetDefaultResources(), 450, ENoProfile, true, false, kMessages);
    log += cleanLog(shader.getInfoLog());
    return ok;
}

// The vertex source's inputs in declaration order, with the location the
// source gives one itself (layout(location)) or -1, read from the parse tree
// before the linker assigns the rest. Inputs without a location are placed in
// declaration order, which is what code that never asks (attribute 0 is the
// first one declared) relies on.
struct DeclaredInput {
    std::string name;
    int location = -1;
};
std::vector<DeclaredInput> declaredInputs(glslang::TShader& vs) {
    std::vector<DeclaredInput> out;
    const glslang::TIntermediate* im = vs.getIntermediate();
    TIntermNode* rootNode = im ? im->getTreeRoot() : nullptr;
    glslang::TIntermAggregate* root = rootNode ? rootNode->getAsAggregate() : nullptr;
    if (!root) return out;
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* agg = node->getAsAggregate();
        if (!agg || agg->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* obj : agg->getSequence()) {
            glslang::TIntermSymbol* sym = obj->getAsSymbolNode();
            if (!sym || sym->getQualifier().storage != glslang::EvqVaryingIn) continue;
            const glslang::TQualifier& q = sym->getQualifier();
            out.push_back({sym->getName().c_str(), q.hasLocation() ? static_cast<int>(q.layoutLocation) : -1});
        }
    }
    return out;
}

// std140 stride of an array element of a default-block value (glslang's
// reflection reports the tightly packed one for the default block).
uint32_t std140ArrayStride(GLenum type) {
    const TypeInfo t = typeInfo(type);
    return t.isMatrix() ? 16u * t.columns : 16u;
}

// Place the active attributes: source locations first, then
// bindAttribLocation, then, in declaration order, the lowest free run of
// locations.
bool placeAttributes(glslang::TProgram& program, const std::vector<DeclaredInput>& declared,
                     const std::unordered_map<std::string, GLuint>& bound, const Limits& limits,
                     ProgramInterface& iface, std::string& log) {
    struct Pending {
        VkAttribInfo info;
        uint32_t slots = 1;
    };
    std::vector<Pending> attribs;
    for (int i = 0; i < program.getNumPipeInputs(); ++i) {
        const glslang::TObjectReflection& in = program.getPipeInput(i);
        if (!(in.stages & EShLangVertexMask) || in.name.rfind("gl_", 0) == 0) continue;
        Pending p;
        p.info.name = in.name;
        p.info.type = static_cast<GLenum>(in.glDefineType);
        const glslang::TType* type = in.getType();
        p.info.size = (type && type->isArray()) ? std::max(1, type->getOuterArraySize()) : 1;
        p.slots = static_cast<uint32_t>(p.info.size) * typeInfo(p.info.type).columns;
        attribs.push_back(p);
    }
    auto declIndex = [&](const std::string& name) {
        for (size_t i = 0; i < declared.size(); ++i)
            if (declared[i].name == name) return i;
        return declared.size();
    };
    std::stable_sort(attribs.begin(), attribs.end(), [&](const Pending& a, const Pending& b) {
        return declIndex(a.info.name) < declIndex(b.info.name);
    });

    std::vector<int> owner(limits.maxVertexAttribs, -1);
    auto claim = [&](size_t index, uint32_t loc) -> bool {
        const Pending& p = attribs[index];
        if (loc + p.slots > limits.maxVertexAttribs) {
            log += "ERROR: attribute '" + p.info.name + "' does not fit below MAX_VERTEX_ATTRIBS\n";
            return false;
        }
        for (uint32_t s = 0; s < p.slots; ++s) {
            if (owner[loc + s] >= 0) {
                log += "ERROR: attributes '" + attribs[static_cast<size_t>(owner[loc + s])].info.name +
                       "' and '" + p.info.name + "' alias location " + std::to_string(loc + s) + "\n";
                return false;
            }
            owner[loc + s] = static_cast<int>(index);
        }
        attribs[index].info.location = static_cast<GLint>(loc);
        return true;
    };
    for (size_t i = 0; i < attribs.size(); ++i) {
        const size_t d = declIndex(attribs[i].info.name);
        if (d < declared.size() && declared[d].location >= 0)
            if (!claim(i, static_cast<uint32_t>(declared[d].location))) return false;
    }
    for (size_t i = 0; i < attribs.size(); ++i) {
        if (attribs[i].info.location >= 0) continue;
        if (auto it = bound.find(attribs[i].info.name); it != bound.end())
            if (!claim(i, it->second)) return false;
    }
    for (size_t i = 0; i < attribs.size(); ++i) {
        if (attribs[i].info.location >= 0) continue;
        uint32_t loc = 0;
        for (; loc + attribs[i].slots <= limits.maxVertexAttribs; ++loc) {
            bool free = true;
            for (uint32_t s = 0; s < attribs[i].slots && free; ++s) free = owner[loc + s] < 0;
            if (free) break;
        }
        if (!claim(i, loc)) return false;
    }

    for (const Pending& p : attribs) {
        const TypeInfo t = typeInfo(p.info.type);
        VkVertexInput::Kind kind = VkVertexInput::Kind::Float;
        if (t.kind == TypeInfo::Kind::Int) kind = VkVertexInput::Kind::Int;
        if (t.kind == TypeInfo::Kind::Uint) kind = VkVertexInput::Kind::Uint;
        for (uint32_t s = 0; s < p.slots; ++s)
            iface.vertexInputs.push_back({static_cast<uint32_t>(p.info.location) + s, kind});
        iface.attribs.push_back(p.info);
    }
    std::sort(iface.vertexInputs.begin(), iface.vertexInputs.end(),
              [](const VkVertexInput& a, const VkVertexInput& b) { return a.location < b.location; });
    return true;
}

void reflectUniforms(glslang::TProgram& program, ProgramInterface& iface) {
    // GL's uniform blocks are glslang's, less the default block.
    int defaultBlock = -1;
    std::vector<GLint> glBlockOf(static_cast<size_t>(program.getNumUniformBlocks()), -1);
    for (int i = 0; i < program.getNumUniformBlocks(); ++i) {
        const glslang::TObjectReflection& b = program.getUniformBlock(i);
        // The fragment stage's FragmentPush is not a GL block.
        if (b.getType() && b.getType()->getQualifier().isPushConstant()) continue;
        if (b.name == kDefaultBlockName) {
            defaultBlock = i;
            iface.defaultBlockBinding = b.getBinding();
            iface.defaultBlockSize = (static_cast<uint32_t>(std::max(b.size, 0)) + 15u) & ~15u;
            continue;
        }
        VkUniformBlockInfo info;
        info.name = b.name;
        info.dataSize = static_cast<uint32_t>(std::max(b.size, 0));
        info.descriptorBinding = static_cast<uint32_t>(b.getBinding());
        for (const VkUniformBlockInfo& prior : iface.uniformBlocks)
            if (prior.descriptorBinding == info.descriptorBinding) ++info.arrayElement;
        info.referencedByVertex = (b.stages & EShLangVertexMask) != 0;
        info.referencedByFragment = (b.stages & EShLangFragmentMask) != 0;
        glBlockOf[static_cast<size_t>(i)] = static_cast<GLint>(iface.uniformBlocks.size());
        iface.uniformBlocks.push_back(std::move(info));
    }

    GLint nextLocation = 0;
    auto giveLocations = [&](VkUniformInfo& u) {
        u.location = nextLocation;
        for (GLint e = 0; e < u.size; ++e)
            iface.locations.push_back({static_cast<uint32_t>(iface.uniforms.size()), static_cast<uint32_t>(e)});
        nextLocation += u.size;
    };

    for (int i = 0; i < program.getNumUniformVariables(); ++i) {
        const glslang::TObjectReflection& r = program.getUniform(i);
        VkUniformInfo u;
        u.name = r.name;
        u.type = static_cast<GLenum>(r.glDefineType);
        u.size = std::max(1, r.size);
        const TypeInfo t = typeInfo(u.type);

        if (r.index >= 0 && r.index == defaultBlock) {
            u.offset = static_cast<uint32_t>(r.offset);
            u.arrayStride = u.size > 1 ? std140ArrayStride(u.type) : 0;
            u.matrixStride = t.isMatrix() ? 16 : 0;
            giveLocations(u);
        } else if (r.index >= 0 && static_cast<size_t>(r.index) < glBlockOf.size() &&
                   glBlockOf[static_cast<size_t>(r.index)] >= 0) {
            const GLint block = glBlockOf[static_cast<size_t>(r.index)];
            u.blockIndex = block;
            u.offset = static_cast<uint32_t>(std::max(r.offset, 0));
            u.arrayStride = u.size > 1 ? static_cast<uint32_t>(std::max(r.arrayStride, 0)) : 0;
            u.matrixStride = t.isMatrix() ? 16 : 0;
            const glslang::TType* type = r.getType();
            u.rowMajor = t.isMatrix() && type && type->getQualifier().layoutMatrix == glslang::ElmRowMajor;
            // A member of an array of blocks belongs to every element.
            const uint32_t binding = iface.uniformBlocks[static_cast<size_t>(block)].descriptorBinding;
            for (VkUniformBlockInfo& b : iface.uniformBlocks)
                if (b.descriptorBinding == binding)
                    b.activeUniformIndices.push_back(static_cast<GLuint>(iface.uniforms.size()));
        } else if (t.kind == TypeInfo::Kind::Sampler && r.getBinding() >= 0) {
            VkSamplerBinding s;
            s.binding = static_cast<uint32_t>(r.getBinding());
            s.count = static_cast<uint32_t>(u.size);
            s.type = u.type;
            s.firstUnit = iface.samplerUnitCount;
            iface.samplerUnitCount += s.count;
            // Visible only where it is sampled, so each stage's samplers
            // count against that stage's descriptor limit alone.
            if (r.stages & EShLangVertexMask) {
                s.stages |= VK_SHADER_STAGE_VERTEX_BIT;
                iface.vertexSamplerUnits += s.count;
            }
            if (r.stages & EShLangFragmentMask) {
                s.stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
                iface.fragmentSamplerUnits += s.count;
            }
            u.sampler = static_cast<int32_t>(iface.samplers.size());
            iface.samplers.push_back(s);
            giveLocations(u);
        } else {
            continue;
        }
        iface.uniforms.push_back(std::move(u));
    }
}

void reflectOutputs(glslang::TProgram& program, ProgramInterface& iface) {
    for (int i = 0; i < program.getNumPipeOutputs(); ++i) {
        const glslang::TObjectReflection& out = program.getPipeOutput(i);
        if (!(out.stages & EShLangFragmentMask)) continue;
        if (out.name.rfind("gl_", 0) == 0 || out.name.rfind("bro_", 0) == 0) continue;
        iface.fragDataLocations[out.name] = static_cast<GLint>(out.layoutLocation());
    }
}

} // namespace

TypeInfo typeInfo(GLenum type) {
    using K = TypeInfo::Kind;
    auto v = [](K k, uint8_t rows) { return TypeInfo{k, 1, rows}; };
    auto m = [](uint8_t cols, uint8_t rows) { return TypeInfo{K::Float, cols, rows}; };
    switch (type) {
        case GL_FLOAT: return v(K::Float, 1);
        case GL_FLOAT_VEC2: return v(K::Float, 2);
        case GL_FLOAT_VEC3: return v(K::Float, 3);
        case GL_FLOAT_VEC4: return v(K::Float, 4);
        case GL_INT: return v(K::Int, 1);
        case GL_INT_VEC2: return v(K::Int, 2);
        case GL_INT_VEC3: return v(K::Int, 3);
        case GL_INT_VEC4: return v(K::Int, 4);
        case GL_UNSIGNED_INT: return v(K::Uint, 1);
        case GL_UNSIGNED_INT_VEC2: return v(K::Uint, 2);
        case GL_UNSIGNED_INT_VEC3: return v(K::Uint, 3);
        case GL_UNSIGNED_INT_VEC4: return v(K::Uint, 4);
        case GL_BOOL: return v(K::Bool, 1);
        case GL_BOOL_VEC2: return v(K::Bool, 2);
        case GL_BOOL_VEC3: return v(K::Bool, 3);
        case GL_BOOL_VEC4: return v(K::Bool, 4);
        case GL_FLOAT_MAT2: return m(2, 2);
        case GL_FLOAT_MAT3: return m(3, 3);
        case GL_FLOAT_MAT4: return m(4, 4);
        case GL_FLOAT_MAT2x3: return m(2, 3);
        case GL_FLOAT_MAT2x4: return m(2, 4);
        case GL_FLOAT_MAT3x2: return m(3, 2);
        case GL_FLOAT_MAT3x4: return m(3, 4);
        case GL_FLOAT_MAT4x2: return m(4, 2);
        case GL_FLOAT_MAT4x3: return m(4, 3);
        case GL_SAMPLER_2D: case GL_SAMPLER_3D: case GL_SAMPLER_CUBE: case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_SAMPLER_CUBE_SHADOW:
        case GL_INT_SAMPLER_2D: case GL_INT_SAMPLER_3D: case GL_INT_SAMPLER_CUBE: case GL_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            return v(K::Sampler, 1);
        default: return TypeInfo{};
    }
}

bool compile(const std::string& source, GLenum type, std::string& log) {
    log.clear();
    auto lock = render::acquireGlslang();
    if (!lock.owns_lock()) {
        log = "ERROR: the shader compiler failed to initialise\n";
        return false;
    }
    const EShLanguage stage = type == GL_VERTEX_SHADER ? EShLangVertex : EShLangFragment;
    const StageSource src = stageSource(stage, source, Limits{});
    glslang::TShader shader(stage);
    return parse(shader, src, log);
}

LinkResult link(const std::string& vertexSource, const std::string& fragmentSource,
                const std::unordered_map<std::string, GLuint>& boundAttribs, const Limits& limits) {
    LinkResult result;
    auto lock = render::acquireGlslang();
    if (!lock.owns_lock()) {
        result.log = "ERROR: the shader compiler failed to initialise\n";
        return result;
    }

    const StageSource vsSrc = stageSource(EShLangVertex, vertexSource, limits);
    const StageSource fsSrc = stageSource(EShLangFragment, fragmentSource, limits);
    // The program refers to its shaders, so they outlive it.
    glslang::TShader vs(EShLangVertex);
    glslang::TShader fs(EShLangFragment);
    if (!parse(vs, vsSrc, result.log) || !parse(fs, fsSrc, result.log)) return result;
    const std::vector<DeclaredInput> declared = declaredInputs(vs);

    glslang::TProgram program;
    program.addShader(&vs);
    program.addShader(&fs);
    if (!program.link(kMessages)) {
        result.log += cleanLog(program.getInfoLog());
        return result;
    }
    std::unique_ptr<glslang::TIoMapResolver> resolver(program.getGlslIoResolver(EShLangVertex));
    std::unique_ptr<glslang::TIoMapper> mapper(glslang::GetGlslIoMapper());
    if (!program.mapIO(resolver.get(), mapper.get())) {
        result.log += cleanLog(program.getInfoLog());
        return result;
    }
    program.buildReflection(EShReflectionStrictArraySuffix | EShReflectionBasicArraySuffix |
                            EShReflectionAllBlockVariables | EShReflectionSeparateBuffers);

    if (!placeAttributes(program, declared, boundAttribs, limits, result.iface, result.log)) return result;
    reflectUniforms(program, result.iface);
    reflectOutputs(program, result.iface);

    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), result.vertSpirv, &options);
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), result.fragSpirv, &options);
    if (result.vertSpirv.empty() || result.fragSpirv.empty()) {
        result.log += "ERROR: SPIR-V generation failed\n";
        return result;
    }

    std::unordered_map<std::string, uint32_t> locations;
    for (const VkAttribInfo& a : result.iface.attribs) locations[a.name] = static_cast<uint32_t>(a.location);
    if (!spirv::setInputLocations(result.vertSpirv, locations)) {
        result.log += "ERROR: could not place the vertex attributes\n";
        return result;
    }
    spirv::stripRelaxedPrecision(result.vertSpirv);
    spirv::stripRelaxedPrecision(result.fragSpirv);
    result.ok = true;
    return result;
}

} // namespace bro::webgl::vk::glsl
