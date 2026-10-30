#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/debug_views.h"

#include <algorithm>
#include <cstring>
#include <regex>
#include <sstream>

#include "mesh.vert.src.h"
#include "mesh.frag.src.h"
#include "volume.frag.src.h"
#include "mesh_instanced.vert.src.h"
#include "mesh_skinned.vert.src.h"
#include "shadow.vert.src.h"
#include "shadow_skinned.vert.src.h"
#include "shadow_instanced.vert.src.h"

namespace bro::scene::vk {

namespace {

constexpr uint32_t kCustomSet = 4;
// The engine's varyings take locations 0..6 (mesh.vert / mesh.frag).
constexpr uint32_t kFirstVaryingLocation = 7;

// The GL-era names a chunk may use, onto the Vulkan shaders' own. Both draw
// camera-relative (scene_view.h), so vWorldPos and uModel carry the eye
// subtracted exactly as they did on GL.
constexpr const char* kFragmentAliases =
    "#define uBaseColorTex texAlbedo\n"
    "#define vWorldPos inWorldPos\n"
    "#define vNormal inNormal\n"
    "#define vUV inUV\n"
    "#define vColor inColor\n"
    "#define vCamDist length(inWorldPos)\n"
    "#define vTangentW inTangent\n"
    "#define vBitangentW inBitangent\n";
constexpr const char* kVertexAliases =
    "#define uModel sceneModel()\n"
    "#define aPos inPos\n"
    "#define aNormal inNormal\n"
    "#define aUV inUV\n"
    "#define aColor inColor\n"
    "#define aTangent inTangent\n";

// The marker line the chunk replaces, with `define` set for the hook.
std::string splice(const std::string& base, const std::string& userChunk, const char* define) {
    std::string s(base);
    if (userChunk.empty()) return s;
    const std::string chunk = debug_views::expand(userChunk);
    const char* marker = "//__USER_CHUNK__";
    const size_t pos = s.find(marker);
    if (pos != std::string::npos) {
        std::string defines = std::string("\n#define ") + define + " 1\n";
        // mesh.frag's optional second hook, called where the chunk defines it.
        if (std::strcmp(define, "CUSTOM_FRAGMENT") == 0 && chunk.find("void userLight(") != std::string::npos)
            defines += "#define CUSTOM_LIGHT 1\n";
        s.replace(pos, std::strlen(marker), defines + chunk + "\n");
    }
    return s;
}

// `#define <name> 1` right after the #version line.
std::string defineAfterVersion(std::string src, const char* name) {
    const size_t version = src.find("#version");
    const size_t lineEnd = version == std::string::npos ? std::string::npos : src.find('\n', version);
    if (lineEnd == std::string::npos) return src;
    src.insert(lineEnd + 1, std::string("#define ") + name + " 1\n");
    return src;
}

bool isIntegerType(const std::string& type) {
    return type == "int" || type == "uint" || type == "bool" || type.rfind("ivec", 0) == 0 ||
           type.rfind("uvec", 0) == 0 || type.rfind("bvec", 0) == 0;
}

// std140 size and alignment of one non-array member (arrays round both to 16).
void std140(const std::string& type, uint32_t& size, uint32_t& align) {
    const char last = type.empty() ? '1' : type.back();
    const bool vector = type.find("vec") != std::string::npos;
    if (type == "mat2") { size = 32; align = 16; return; }
    if (type == "mat3") { size = 48; align = 16; return; }
    if (type == "mat4") { size = 64; align = 16; return; }
    if (!vector) { size = 4; align = 4; return; }
    const uint32_t n = static_cast<uint32_t>(last - '0');
    size = 4 * n;
    align = n == 2 ? 8 : 16;
}

// std430 array stride of a buffer element type, 0 for one a buffer cannot hold.
uint32_t std430Stride(const std::string& type) {
    if (type == "float" || type == "int" || type == "uint") return 4;
    if (type == "vec2" || type == "ivec2" || type == "uvec2") return 8;
    if (type == "vec3" || type == "ivec3" || type == "uvec3" || type == "vec4" || type == "ivec4" ||
        type == "uvec4")
        return 16;
    if (type == "mat4") return 64;
    return 0;
}

// The engine's own shader a chunk is spliced into (from the source tree under
// BRO_SCENE_SHADER_SOURCE).
std::string base(const char* file, const char* embedded) {
    return SceneVkShaderCompiler::shaderSource(file, embedded);
}

std::string vertexBase(SceneRenderer::CustomShaderTarget target) {
    switch (target) {
    case SceneRenderer::CustomShaderTarget::Instanced: return base("draw/mesh_instanced.vert", kVkMeshInstancedVertSrc);
    case SceneRenderer::CustomShaderTarget::Skinned: return base("draw/mesh_skinned.vert", kVkMeshSkinnedVertSrc);
    default: return base("draw/mesh.vert", kVkMeshVertSrc);
    }
}

}  // namespace

bool CustomShaderInterface::declareUniform(const std::string& type, const std::string& declarator,
                                           uint32_t& offset, std::string& err) {
    static const std::regex declRe(R"(^\s*(\w+)\s*(\[\s*(\d+)\s*\])?\s*(=[\s\S]*)?$)");
    std::smatch m;
    if (!std::regex_match(declarator, m, declRe)) {
        err = "custom shader: cannot parse the uniform declaration '" + declarator + "'";
        return false;
    }
    const std::string name = m[1].str();
    const bool array = m[2].matched;
    if (type.rfind("sampler", 0) == 0) {
        if ((type != "sampler2D" && type != "sampler2DArray") || array) {
            err = "custom shader: only single sampler2D / sampler2DArray uniforms are supported ('" + name + "')";
            return false;
        }
        if (std::none_of(samplers_.begin(), samplers_.end(), [&](const Sampler& x) { return x.name == name; })) {
            if (samplers_.size() == kMaxSamplers) {
                err = "custom shader: more than " + std::to_string(kMaxSamplers) + " samplers";
                return false;
            }
            samplers_.push_back({name, type == "sampler2DArray"});
        }
        return true;
    }
    if (std::any_of(uniforms_.begin(), uniforms_.end(), [&](const Uniform& u) { return u.name == name; })) {
        return true;   // declared by the other chunk too: one member
    }
    uint32_t size = 0, align = 0;
    std140(type, size, align);
    if (array) {
        const uint32_t count = static_cast<uint32_t>(std::max(1, std::stoi(m[3].str())));
        align = 16;
        size = ((size + 15) & ~15u) * count;
    }
    offset = (offset + align - 1) & ~(align - 1);
    uniforms_.push_back({type, name, offset, isIntegerType(type)});
    uniformDecls_.push_back("    " + type + " " + name + (array ? m[2].str() : "") + ";\n");
    offset += size;
    return true;
}

bool CustomShaderInterface::declareBuffer(const std::string& type, const std::string& name, uint32_t& offset,
                                          std::string& err) {
    if (std::any_of(buffers_.begin(), buffers_.end(), [&](const Buffer& b) { return b.name == name; })) {
        return true;   // declared by the other chunk too
    }
    if (name.rfind("u_", 0) != 0) {
        err = "custom shader: a buffer's name must start with u_ ('" + name + "')";
        return false;
    }
    const uint32_t stride = std430Stride(type);
    if (stride == 0) {
        err = "custom shader: a buffer holds float, int, uint, their vec2-4 forms or mat4 ('" + type + " " + name +
              "')";
        return false;
    }
    offset = (offset + 7) & ~7u;
    Buffer b{type, name, stride, offset, offset + 8};
    uniformDecls_.push_back("    " + name + "_Buffer " + name + "_ref;\n");
    uniformDecls_.push_back("    uint " + name + "_length;\n");
    offset += 12;
    buffers_.push_back(std::move(b));
    return true;
}

bool CustomShaderInterface::scan(const std::string& chunk, bool vertex, std::string& body, uint32_t& offset,
                                 std::string& err) {
    static const std::regex uniformRe(R"(\buniform\s+(?:(?:highp|mediump|lowp)\s+)?(\w+)\s+([^;]+);)");
    static const std::regex bufferRe(R"(\b(?:readonly\s+)?buffer\s+(\w+)\s+(\w+)\s*\[\s*\]\s*;)");
    static const std::regex varyingRe(
        R"(^\s*(?:(flat|smooth|noperspective)\s+)?(in|out)\s+(\w+)\s+(\w+)\s*(\[\s*\d+\s*\])?\s*;)");

    std::istringstream lines(chunk);
    std::string line;
    while (std::getline(lines, line)) {
        // Declarations are matched in the code, not in a trailing comment;
        // each is cut from the line (the line stays, so error line numbers
        // still name the user's lines).
        const size_t comment = line.find("//");
        std::string code = line.substr(0, comment);
        const std::string tail = comment == std::string::npos ? std::string() : line.substr(comment);

        std::string rest;
        auto last = code.cbegin();
        for (std::sregex_iterator it(code.begin(), code.end(), uniformRe), end; it != end; ++it) {
            const std::smatch& m = *it;
            rest.append(last, m[0].first);
            last = m[0].second;
            // A comma inside an initialiser (vec3(1, 2, 3)) is not a separator.
            std::string pending;
            int depth = 0;
            for (char c : m[2].str()) {
                if (c == '(') ++depth;
                if (c == ')') --depth;
                if (c == ',' && depth == 0) {
                    if (!declareUniform(m[1].str(), pending, offset, err)) return false;
                    pending.clear();
                } else {
                    pending += c;
                }
            }
            if (!declareUniform(m[1].str(), pending, offset, err)) return false;
        }
        rest.append(last, code.cend());
        code = rest;

        rest.clear();
        last = code.cbegin();
        for (std::sregex_iterator it(code.begin(), code.end(), bufferRe), end; it != end; ++it) {
            rest.append(last, (*it)[0].first);
            last = (*it)[0].second;
            if (!declareBuffer((*it)[1].str(), (*it)[2].str(), offset, err)) return false;
        }
        rest.append(last, code.cend());
        code = rest;

        std::smatch m;
        if (std::regex_search(code, m, varyingRe)) {
            const bool out = m[2].str() == "out";
            // Only a vertex `out` / fragment `in` is a varying.
            if (out == vertex) {
                const std::string name = m[4].str();
                auto it = std::find_if(varyings_.begin(), varyings_.end(),
                                       [&](const Varying& v) { return v.name == name; });
                if (vertex && it == varyings_.end()) {
                    varyings_.push_back({m[1].str(), m[3].str(), name, m[5].str(),
                                         kFirstVaryingLocation + static_cast<uint32_t>(varyings_.size())});
                } else if (!vertex && it == varyings_.end()) {
                    err = "custom shader: fragment input '" + name + "' has no matching vertex output";
                    return false;
                }
                code = m.prefix().str() + m.suffix().str();
            }
        }
        body += code;
        body += tail;
        body += '\n';
    }
    return true;
}

bool CustomShaderInterface::parse(const std::string& vertexChunk, const std::string& fragmentChunk,
                                  std::string& err) {
    *this = CustomShaderInterface{};
    hasVertex_ = !vertexChunk.empty();
    hasFragment_ = !fragmentChunk.empty();
    uint32_t offset = 0;
    if (!scan(vertexChunk, true, vertexBody_, offset, err) || !scan(fragmentChunk, false, fragmentBody_, offset, err))
        return false;
    if (!samplers_.empty()) {
        // Past the chunk's own: each sampler's CS_* (samplerModesOffset).
        offset = (offset + 3) & ~3u;
        samplerModesOffset_ = offset;
        uniformDecls_.push_back("    uint customSamplerModes;\n");
        offset += 4;
    }
    uboSize_ = std::max<uint32_t>(16, (offset + 15) & ~15u);
    return true;
}

std::string CustomShaderInterface::declarations(bool vertex) const {
    std::string s = vertex ? kVertexAliases : kFragmentAliases;
    // Each buffer's block type, before the uniform block that holds its
    // address (ComputeBuffer addresses are 256-byte aligned).
    for (const Buffer& b : buffers_) {
        s += "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer " + b.name +
             "_Buffer { " + b.type + " data[]; };\n";
        s += "#define " + b.name + " " + b.name + "_ref.data\n";
    }
    if (!uniformDecls_.empty()) {
        s += "layout(set = " + std::to_string(kCustomSet) + ", binding = 0, std140) uniform UserUniforms {\n";
        for (const std::string& d : uniformDecls_) s += d;
        s += "};\n";
    }
    // Each sampler a bare image, read through the custom set's sampler its
    // texture asks for (SamplerMode: two bits a slot of customSamplerModes).
    const std::string set = "layout(set = " + std::to_string(kCustomSet) + ", binding = ";
    if (!samplers_.empty()) {
        s += "#ifndef CUSTOM_SAMPLERS_DECLARED\n#define CUSTOM_SAMPLERS_DECLARED\n" + set +
             std::to_string(kSamplersBinding) + ") uniform sampler customSamplers[" + std::to_string(kSamplerModes) +
             "];\n#endif\n";
    }
    for (size_t i = 0; i < samplers_.size(); ++i) {
        const std::string& name = samplers_[i].name;
        const char* image = samplers_[i].array ? "texture2DArray " : "texture2D ";
        const char* sampler = samplers_[i].array ? "sampler2DArray" : "sampler2D";
        s += set + std::to_string(i + 1) + ") uniform " + image + name + "_image;\n";
        s += "#define " + name + " " + sampler + "(" + name + "_image, customSamplers[(customSamplerModes >> " +
             std::to_string(2 * i) + "u) & 3u])\n";
    }
    for (const Varying& v : varyings_) {
        s += "layout(location = " + std::to_string(v.location) + ") ";
        if (!v.qualifier.empty()) s += v.qualifier + " ";
        s += std::string(vertex ? "out " : "in ") + v.type + " " + v.name + v.array + ";\n";
    }
    // The chunk's own line numbers in compile errors.
    s += "#line 1\n";
    return s;
}

std::string CustomShaderInterface::withExtensions(std::string src) const {
    if (buffers_.empty()) return src;
    const size_t version = src.find("#version");
    const size_t lineEnd = version == std::string::npos ? std::string::npos : src.find('\n', version);
    if (lineEnd == std::string::npos) return src;
    src.insert(lineEnd + 1, "#extension GL_EXT_buffer_reference : require\n");
    return src;
}

std::string CustomShaderInterface::vertexSource() const {
    return hasVertex_ ? declarations(true) + vertexBody_ : std::string();
}

std::string CustomShaderInterface::fragmentSource() const {
    return hasFragment_ ? declarations(false) + fragmentBody_ : std::string();
}

bool SceneVkCustomShader::validateCustomShader(SceneRenderer::CustomShaderTarget target,
                                               const std::string& vertexChunk,
                                               const std::string& fragmentChunk,
                                               std::string& errOut) {
    CustomShaderInterface iface;
    if (!iface.parse(vertexChunk, fragmentChunk, errOut)) return false;
    if (target == SceneRenderer::CustomShaderTarget::Volume) {
        if (!vertexChunk.empty() || fragmentChunk.find("void userVolume(") == std::string::npos) {
            errOut = "a volume takes a fragment chunk defining userVolume, and no vertex chunk";
            return false;
        }
        const std::string fs = iface.withExtensions(
            splice(base("draw/volume.frag", kVkVolumeFragSrc), iface.fragmentSource(), "CUSTOM_VOLUME"));
        return !SceneVkShaderCompiler::compileGlsl(fs, VK_SHADER_STAGE_FRAGMENT_BIT, &errOut).empty();
    }
    if (!vertexChunk.empty()) {
        const std::string vs = iface.withExtensions(splice(vertexBase(target), iface.vertexSource(), "CUSTOM_VERTEX"));
        if (SceneVkShaderCompiler::compileGlsl(vs, VK_SHADER_STAGE_VERTEX_BIT, &errOut).empty()) return false;
    }
    if (!fragmentChunk.empty()) {
        const std::string fs = iface.withExtensions(
            splice(base("draw/mesh.frag", kVkMeshFragSrc), iface.fragmentSource(), "CUSTOM_FRAGMENT"));
        if (SceneVkShaderCompiler::compileGlsl(fs, VK_SHADER_STAGE_FRAGMENT_BIT, &errOut).empty()) return false;
    }
    return true;
}

bool SceneVkCustomShader::compileCustomShaderModules(VkDevice device,
                                                     SceneRenderer::CustomShaderTarget target,
                                                     const CustomShaderInterface& iface,
                                                     VkShaderModule& outVs,
                                                     VkShaderModule& outFs,
                                                     bool indirectOutput,
                                                     std::string& errOut) {
    // A custom varying needs the vertex stage to write it even when only the
    // fragment chunk reads it, so both stages always splice their chunk.
    const std::string vsSrc = iface.withExtensions(splice(vertexBase(target), iface.vertexSource(), "CUSTOM_VERTEX"));
    std::string fsSrc = iface.withExtensions(
        splice(base("draw/mesh.frag", kVkMeshFragSrc), iface.fragmentSource(), "CUSTOM_FRAGMENT"));
    if (indirectOutput) fsSrc = defineAfterVersion(std::move(fsSrc), "SCENE_INDIRECT_OUTPUT");

    const auto vsSpirv = SceneVkShaderCompiler::compileGlsl(vsSrc, VK_SHADER_STAGE_VERTEX_BIT, &errOut);
    if (vsSpirv.empty()) return false;
    const auto fsSpirv = SceneVkShaderCompiler::compileGlsl(fsSrc, VK_SHADER_STAGE_FRAGMENT_BIT, &errOut);
    if (fsSpirv.empty()) return false;

    outVs = SceneVkShaderModule::create(device, vsSpirv);
    outFs = SceneVkShaderModule::create(device, fsSpirv);
    if (outVs == VK_NULL_HANDLE || outFs == VK_NULL_HANDLE) {
        if (outVs != VK_NULL_HANDLE) SceneVkShaderModule::destroy(device, outVs);
        if (outFs != VK_NULL_HANDLE) SceneVkShaderModule::destroy(device, outFs);
        outVs = outFs = VK_NULL_HANDLE;
        errOut = "Failed to create VkShaderModule";
        return false;
    }
    return true;
}

bool SceneVkCustomShader::compileVolumeModule(VkDevice device, const CustomShaderInterface& iface,
                                              VkShaderModule& outFs, std::string& errOut) {
    const std::string fsSrc = iface.withExtensions(
        splice(base("draw/volume.frag", kVkVolumeFragSrc), iface.fragmentSource(), "CUSTOM_VOLUME"));
    const auto spirv = SceneVkShaderCompiler::compileGlsl(fsSrc, VK_SHADER_STAGE_FRAGMENT_BIT, &errOut);
    if (spirv.empty()) return false;
    outFs = SceneVkShaderModule::create(device, spirv);
    if (outFs == VK_NULL_HANDLE) errOut = "Failed to create VkShaderModule";
    return outFs != VK_NULL_HANDLE;
}

bool SceneVkCustomShader::compileCustomShadowShaderModule(VkDevice device,
                                                          ShadowCaster caster,
                                                          bool cutout,
                                                          const CustomShaderInterface& iface,
                                                          VkShaderModule& outVs,
                                                          std::string& errOut) {
    const std::string chunk = iface.vertexSource();
    if (chunk.empty()) return false;
    std::string src;
    switch (caster) {
    case ShadowCaster::Skinned: src = base("shadow/shadow_skinned.vert", kVkShadowSkinnedVertSrc); break;
    case ShadowCaster::Instanced: src = base("shadow/shadow_instanced.vert", kVkShadowInstancedVertSrc); break;
    default: src = base("shadow/shadow.vert", kVkShadowVertSrc); break;
    }
    // CUSTOM_CASTER declares the inputs only a custom caster's pipeline feeds.
    std::string vsSrc = iface.withExtensions(defineAfterVersion(splice(src, chunk, "CUSTOM_VERTEX"), "CUSTOM_CASTER"));
    if (cutout) vsSrc = defineAfterVersion(std::move(vsSrc), "SHADOW_CUTOUT");
    const auto spirv = SceneVkShaderCompiler::compileGlsl(vsSrc, VK_SHADER_STAGE_VERTEX_BIT, &errOut);
    if (spirv.empty()) return false;
    outVs = SceneVkShaderModule::create(device, spirv);
    return outVs != VK_NULL_HANDLE;
}

} // namespace bro::scene::vk
