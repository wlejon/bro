#include "webgl/vulkan/webgl_vk_shaders_parser.h"
#include "webgl/webgl_types.h"
#include <sstream>
#include <regex>
#include <cstring>
#include <algorithm>
#include <set>

namespace bro::webgl::vk {

namespace {

// A fragment stage sees GL's window space through the FragmentPush mapping
// (webgl_vk_types.h): gl_FragCoord.y, dFdy and gl_PointCoord.y rebuilt from
// Vulkan's. A macro is not re-expanded inside its own replacement, so each one
// reads the built-in it shadows.
std::string fragmentWindowSpace(const ParsedShader& fs) {
    std::string out = "layout(push_constant) uniform BroFragmentPush { float yOffset; float yScale; } bro_push;\n";
    if (fs.hasFragCoord) {
        out += "vec4 bro_fragCoord() {\n"
               "    return vec4(gl_FragCoord.x, bro_push.yOffset + bro_push.yScale * gl_FragCoord.y, gl_FragCoord.zw);\n"
               "}\n"
               "#define gl_FragCoord bro_fragCoord()\n";
    }
    if (fs.hasPointCoord) {
        out += "vec2 bro_pointCoord() {\n"
               "    return vec2(gl_PointCoord.x, 0.5 - bro_push.yScale * (gl_PointCoord.y - 0.5));\n"
               "}\n"
               "#define gl_PointCoord bro_pointCoord()\n";
    }
    out += "#define dFdy(p) (bro_push.yScale * dFdy(p))\n";
    return out;
}

} // namespace

uint32_t WebGLVkShaderParser::alignTo(uint32_t offset, uint32_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

bool WebGLVkShaderParser::isSamplerType(const std::string& type) {
    return type.find("sampler") != std::string::npos ||
           type.find("Sampler") != std::string::npos;
}

GLenum WebGLVkShaderParser::typeStringToGLenum(const std::string& type) {
    if (type == "float") return GL_FLOAT;
    if (type == "vec2") return GL_FLOAT_VEC2;
    if (type == "vec3") return GL_FLOAT_VEC3;
    if (type == "vec4") return GL_FLOAT_VEC4;
    if (type == "int") return GL_INT;
    if (type == "ivec2") return GL_INT_VEC2;
    if (type == "ivec3") return GL_INT_VEC3;
    if (type == "ivec4") return GL_INT_VEC4;
    if (type == "uint") return GL_UNSIGNED_INT;
    if (type == "uvec2") return GL_UNSIGNED_INT_VEC2;
    if (type == "uvec3") return GL_UNSIGNED_INT_VEC3;
    if (type == "uvec4") return GL_UNSIGNED_INT_VEC4;
    if (type == "bool") return GL_BOOL;
    if (type == "bvec2") return GL_BOOL_VEC2;
    if (type == "bvec3") return GL_BOOL_VEC3;
    if (type == "bvec4") return GL_BOOL_VEC4;
    if (type == "mat2" || type == "mat2x2") return GL_FLOAT_MAT2;
    if (type == "mat3" || type == "mat3x3") return GL_FLOAT_MAT3;
    if (type == "mat4" || type == "mat4x4") return GL_FLOAT_MAT4;
    if (type == "mat2x3") return 0x8B65; // GL_FLOAT_MAT2x3
    if (type == "mat3x2") return 0x8B67; // GL_FLOAT_MAT3x2
    if (type == "mat2x4") return 0x8B66; // GL_FLOAT_MAT2x4
    if (type == "mat4x2") return 0x8B68; // GL_FLOAT_MAT4x2
    if (type == "mat3x4") return 0x8B69; // GL_FLOAT_MAT3x4
    if (type == "mat4x3") return 0x8B6A; // GL_FLOAT_MAT4x3
    if (type == "sampler2D") return GL_SAMPLER_2D;
    if (type == "samplerCube") return GL_SAMPLER_CUBE;
    if (type == "sampler3D") return 0x8B5F; // GL_SAMPLER_3D
    if (type == "sampler2DArray") return 0x8DC1; // GL_SAMPLER_2D_ARRAY
    if (type == "sampler2DShadow") return 0x8B62; // GL_SAMPLER_2D_SHADOW
    if (type == "samplerCubeShadow") return 0x8DC5; // GL_SAMPLER_CUBE_SHADOW
    if (type == "isampler2D") return 0x8D6E;
    if (type == "isampler3D") return 0x8D6F;
    if (type == "isamplerCube") return 0x8D70;
    if (type == "usampler2D") return 0x8D74;
    if (type == "usampler3D") return 0x8D75;
    if (type == "usamplerCube") return 0x8D76;
    return GL_FLOAT;
}

std::pair<uint32_t, uint32_t> WebGLVkShaderParser::getUniformSizeAndAlign(const std::string& type) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool") return {4, 4};
    if (type == "vec2" || type == "ivec2" || type == "uvec2" || type == "bvec2") return {8, 8};
    if (type == "vec3" || type == "ivec3" || type == "uvec3" || type == "bvec3") return {12, 16};
    if (type == "vec4" || type == "ivec4" || type == "uvec4" || type == "bvec4") return {16, 16};
    if (type == "mat2" || type == "mat2x2") return {32, 16}; // 2 vec4s
    if (type == "mat3" || type == "mat3x3") return {48, 16}; // 3 vec4s
    if (type == "mat4" || type == "mat4x4") return {64, 16}; // 4 vec4s
    if (type == "mat2x3") return {32, 16}; // 2 vec4s
    if (type == "mat3x2") return {48, 16}; // 3 vec4s
    if (type == "mat2x4") return {32, 16};
    if (type == "mat4x2") return {64, 16};
    if (type == "mat3x4") return {48, 16};
    if (type == "mat4x3") return {64, 16};
    return {16, 16};
}

namespace {

std::string stripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool inBlock = false;
    for (size_t i = 0; i < src.size(); ++i) {
        if (!inBlock && i + 1 < src.size() && src[i] == '/' && src[i+1] == '*') {
            inBlock = true;
            i++;
        } else if (inBlock && i + 1 < src.size() && src[i] == '*' && src[i+1] == '/') {
            inBlock = false;
            i++;
        } else if (!inBlock && i + 1 < src.size() && src[i] == '/' && src[i+1] == '/') {
            while (i < src.size() && src[i] != '\n') i++;
            if (i < src.size()) out += '\n';
        } else if (!inBlock) {
            out += src[i];
        }
    }
    return out;
}

std::string stripPrecisions(const std::string& line) {
    // Strip standalone precision statements
    std::string trimmed = line;
    auto pos = trimmed.find_first_not_of(" \t");
    if (pos != std::string::npos) trimmed = trimmed.substr(pos);
    if (trimmed.rfind("precision ", 0) == 0 && trimmed.find(';') != std::string::npos) {
        return "";
    }

    // Strip inline precision qualifiers
    std::string stripped;
    stripped.reserve(line.size());
    size_t i = 0;
    while (i < line.size()) {
        bool replaced = false;
        for (const char* q : {"highp ", "mediump ", "lowp ", "highp\t", "mediump\t", "lowp\t"}) {
            size_t len = strlen(q);
            if (line.compare(i, len, q) == 0) {
                if (i == 0 || line[i-1] == ' ' || line[i-1] == '\t' || line[i-1] == '(' || line[i-1] == ',') {
                    i += len;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            stripped += line[i++];
        }
    }
    return stripped;
}

int safeStoi(const std::string& str, int defVal = 1) {
    if (str.empty()) return defVal;
    try {
        return std::stoi(str);
    } catch (...) {
        return defVal;
    }
}

static std::vector<ParsedVar> parseBlockMembers(const std::string& body) {
    std::vector<ParsedVar> members;
    std::regex memRegex(R"(\b([A-Za-z_]\w*)\s+([A-Za-z_]\w*)(?:\[\s*(\d+)\s*\])?\s*;)");
    auto begin = std::sregex_iterator(body.begin(), body.end(), memRegex);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::smatch m = *it;
        ParsedVar pv;
        pv.type = m[1].str();
        pv.name = m[2].str();
        pv.arraySize = m[3].matched ? safeStoi(m[3].str(), 1) : 1;
        pv.isArray = m[3].matched;
        members.push_back(pv);
    }
    return members;
}

} // namespace

ParsedShader WebGLVkShaderParser::parse(const std::string& glslSource, GLenum shaderType) {
    ParsedShader result;
    result.type = shaderType;
    bool isVertex = (shaderType == GL_VERTEX_SHADER);

    std::string clean = stripComments(glslSource);
    std::istringstream stream(clean);
    std::string line;
    std::ostringstream remainingLines;

    bool inUniformBlock = false;
    std::string currentBlockName;
    std::string currentBlockBody;
    int currentBlockBinding = -1;

    // Regex patterns
    // 1. Uniform block header: [layout(...)] uniform <Name> {
    std::regex uboHeaderRegex(R"(^\s*(?:layout\s*\(([^)]*)\)\s*)?uniform\s+([A-Za-z_]\w*)\s*\{)");
    // 2. Uniform variable: uniform <type> <name>[<count>];
    std::regex uniformRegex(R"(^\s*uniform\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)(?:\[\s*(\d+)\s*\])?\s*;)");
    // 3. User attribute / input: [layout(location = N)] [qualifiers] (attribute|in) <type> <name>;
    std::regex attrRegex(R"(^\s*(?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)?(?:(flat|smooth|noperspective|centroid)\s+)?(?:attribute|in)\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)(?:\[\s*(\d+)\s*\])?\s*;)");
    // 4. Varying (classic): varying <type> <name>;
    std::regex varyingClassicRegex(R"(^\s*(?:(flat|smooth|noperspective|centroid)\s+)?varying\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)(?:\[\s*(\d+)\s*\])?\s*;)");
    // 5. VS user output / varying: [layout(location = N)] [qualifiers] out <type> <name>;
    std::regex vsOutRegex(R"(^\s*(?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)?(?:(flat|smooth|noperspective|centroid)\s+)?out\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)(?:\[\s*(\d+)\s*\])?\s*;)");
    // 6. FS user input / varying: [layout(location = N)] [qualifiers] in <type> <name>;
    std::regex fsInRegex(R"(^\s*(?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)?(?:(flat|smooth|noperspective|centroid)\s+)?in\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)(?:\[\s*(\d+)\s*\])?\s*;)");
    // 7. FS user output: [layout(location = N)] out <type> <name>;
    std::regex fsOutRegex(R"(^\s*(?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)?out\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)(?:\[\s*(\d+)\s*\])?\s*;)");

    while (std::getline(stream, line)) {
        if (line.find("#version") != std::string::npos) continue;

        line = stripPrecisions(line);
        if (line.empty()) continue;

        if (line.find("gl_FragColor") != std::string::npos) result.hasFragColor = true;
        if (line.find("gl_FragCoord") != std::string::npos) result.hasFragCoord = true;
        if (line.find("gl_PointCoord") != std::string::npos) result.hasPointCoord = true;

        if (inUniformBlock) {
            currentBlockBody += line + "\n";
            if (line.find("};") != std::string::npos) {
                inUniformBlock = false;
                ParsedUniformBlock ub;
                ub.name = currentBlockName;
                ub.body = currentBlockBody;
                ub.binding = currentBlockBinding;
                ub.members = parseBlockMembers(currentBlockBody);
                result.uniformBlocks.push_back(ub);
            }
            continue;
        }

        std::smatch match;
        if (std::regex_search(line, match, uboHeaderRegex)) {
            inUniformBlock = true;
            std::string layoutParams = match[1].str();
            currentBlockName = match[2].str();
            currentBlockBody = "";
            currentBlockBinding = -1;
            if (!layoutParams.empty()) {
                std::regex bindRegex(R"(binding\s*=\s*(\d+))");
                std::smatch bMatch;
                if (std::regex_search(layoutParams, bMatch, bindRegex)) {
                    currentBlockBinding = safeStoi(bMatch[1].str(), -1);
                }
            }
            if (line.find("};") != std::string::npos) {
                inUniformBlock = false;
                ParsedUniformBlock ub;
                ub.name = currentBlockName;
                ub.body = line.substr(line.find('{'));
                ub.binding = currentBlockBinding;
                ub.members = parseBlockMembers(ub.body);
                result.uniformBlocks.push_back(ub);
            }
            continue;
        }

        if (std::regex_search(line, match, uniformRegex)) {
            std::string type = match[1].str();
            std::string name = match[2].str();
            int arrCount = match[3].matched ? safeStoi(match[3].str(), 1) : 1;
            ParsedVar v;
            v.type = type;
            v.name = name;
            v.arraySize = arrCount;
            v.isArray = match[3].matched;

            if (isSamplerType(type)) {
                result.samplers.push_back(v);
            } else {
                result.uniforms.push_back(v);
            }
            continue;
        }

        if (isVertex) {
            if (std::regex_search(line, match, attrRegex)) {
                ParsedVar v;
                v.location = match[1].matched ? safeStoi(match[1].str(), -1) : -1;
                v.qualifiers = match[2].matched ? match[2].str() : "";
                v.type = match[3].str();
                v.name = match[4].str();
                v.arraySize = match[5].matched ? safeStoi(match[5].str(), 1) : 1;
                v.isArray = match[5].matched;
                result.attributes.push_back(v);
                continue;
            }
            if (std::regex_search(line, match, varyingClassicRegex)) {
                ParsedVar v;
                v.location = -1;
                v.qualifiers = match[1].matched ? match[1].str() : "";
                v.type = match[2].str();
                v.name = match[3].str();
                v.arraySize = match[4].matched ? safeStoi(match[4].str(), 1) : 1;
                v.isArray = match[4].matched;
                result.varyings.push_back(v);
                continue;
            }
            if (std::regex_search(line, match, vsOutRegex)) {
                ParsedVar v;
                v.location = match[1].matched ? safeStoi(match[1].str(), -1) : -1;
                v.qualifiers = match[2].matched ? match[2].str() : "";
                v.type = match[3].str();
                v.name = match[4].str();
                v.arraySize = match[5].matched ? safeStoi(match[5].str(), 1) : 1;
                v.isArray = match[5].matched;
                result.varyings.push_back(v);
                continue;
            }
        } else {
            // Fragment stage
            if (std::regex_search(line, match, varyingClassicRegex)) {
                ParsedVar v;
                v.location = -1;
                v.qualifiers = match[1].matched ? match[1].str() : "";
                v.type = match[2].str();
                v.name = match[3].str();
                v.arraySize = match[4].matched ? safeStoi(match[4].str(), 1) : 1;
                v.isArray = match[4].matched;
                result.varyings.push_back(v);
                continue;
            }
            if (std::regex_search(line, match, fsInRegex)) {
                ParsedVar v;
                v.location = match[1].matched ? safeStoi(match[1].str(), -1) : -1;
                v.qualifiers = match[2].matched ? match[2].str() : "";
                v.type = match[3].str();
                v.name = match[4].str();
                v.arraySize = match[5].matched ? safeStoi(match[5].str(), 1) : 1;
                v.isArray = match[5].matched;
                result.varyings.push_back(v);
                continue;
            }
            if (std::regex_search(line, match, fsOutRegex)) {
                ParsedVar v;
                v.location = match[1].matched ? safeStoi(match[1].str(), -1) : -1;
                v.type = match[2].str();
                v.name = match[3].str();
                v.arraySize = match[4].matched ? safeStoi(match[4].str(), 1) : 1;
                v.isArray = match[4].matched;
                result.fragmentOutputs.push_back(v);
                continue;
            }
        }

        remainingLines << line << "\n";
    }

    result.cleanedSource = remainingLines.str();
    return result;
}

std::string WebGLVkShaderParser::generateStandaloneVulkanGLSL(const ParsedShader& parsed) {
    std::ostringstream out;
    out << "#version 450\n";
    out << "#define texture2D texture\n";
    out << "#define textureCube texture\n";

    bool isVertex = (parsed.type == GL_VERTEX_SHADER);

    if (isVertex) {
        int nextLoc = 0;
        for (const auto& attr : parsed.attributes) {
            int loc = (attr.location >= 0) ? attr.location : nextLoc;
            nextLoc = std::max(nextLoc, loc + 1);
            out << "layout(location = " << loc << ") in ";
            if (!attr.qualifiers.empty()) out << attr.qualifiers << " ";
            out << attr.type << " " << attr.name;
            if (attr.isArray) out << "[" << attr.arraySize << "]";
            out << ";\n";
        }
        int nextVaryingLoc = 0;
        for (const auto& v : parsed.varyings) {
            int loc = (v.location >= 0) ? v.location : nextVaryingLoc;
            nextVaryingLoc = std::max(nextVaryingLoc, loc + (v.isArray ? v.arraySize : 1));
            out << "layout(location = " << loc << ") ";
            if (!v.qualifiers.empty()) out << v.qualifiers << " ";
            out << "out " << v.type << " " << v.name;
            if (v.isArray) out << "[" << v.arraySize << "]";
            out << ";\n";
        }
    } else {
        int nextVaryingLoc = 0;
        for (const auto& v : parsed.varyings) {
            int loc = (v.location >= 0) ? v.location : nextVaryingLoc;
            nextVaryingLoc = std::max(nextVaryingLoc, loc + (v.isArray ? v.arraySize : 1));
            out << "layout(location = " << loc << ") ";
            if (!v.qualifiers.empty()) out << v.qualifiers << " ";
            out << "in " << v.type << " " << v.name;
            if (v.isArray) out << "[" << v.arraySize << "]";
            out << ";\n";
        }
        if (!parsed.fragmentOutputs.empty()) {
            int nextOutLoc = 0;
            for (const auto& fo : parsed.fragmentOutputs) {
                int loc = (fo.location >= 0) ? fo.location : nextOutLoc;
                nextOutLoc = std::max(nextOutLoc, loc + (fo.isArray ? fo.arraySize : 1));
                out << "layout(location = " << loc << ") out " << fo.type << " " << fo.name;
                if (fo.isArray) out << "[" << fo.arraySize << "]";
                out << ";\n";
            }
        } else {
            out << "layout(location = 0) out vec4 bro_FragColor;\n";
            out << "#define gl_FragColor bro_FragColor\n";
        }
    }

    int nextBinding = 0;
    for (const auto& s : parsed.samplers) {
        out << "layout(binding = " << nextBinding++ << ") uniform " << s.type << " " << s.name;
        if (s.isArray) out << "[" << s.arraySize << "]";
        out << ";\n";
    }

    int nextUboBinding = kFirstUniformBlockBinding;
    for (const auto& ub : parsed.uniformBlocks) {
        int b = nextUboBinding++;
        out << "layout(binding = " << b << ", std140) uniform " << ub.name << " " << ub.body << "\n";
    }

    if (!parsed.uniforms.empty()) {
        out << "layout(binding = " << kDefaultUniformBinding << ", std140) uniform WebGLUniforms {\n";
        for (const auto& u : parsed.uniforms) {
            out << "    " << u.type << " " << u.name;
            if (u.isArray) out << "[" << u.arraySize << "]";
            out << ";\n";
        }
        out << "};\n";
    }

    if (isVertex) {
        std::string vSource = parsed.cleanedSource;
        std::regex mainRegex(R"(\bvoid\s+main\s*\(\s*(?:void)?\s*\))");
        if (std::regex_search(vSource, mainRegex)) {
            vSource = std::regex_replace(vSource, mainRegex, "void _bro_user_main()", std::regex_constants::format_first_only);
            out << vSource << "\n";
            out << "void main() {\n";
            out << "    _bro_user_main();\n";
            out << "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
            out << "}\n";
        } else {
            out << vSource;
        }
    } else {
        out << fragmentWindowSpace(parsed);
        out << parsed.cleanedSource;
    }
    return out.str();
}

ProgramLinkResult WebGLVkShaderParser::linkAndGenerateVulkanGLSL(
    const ParsedShader& vs,
    const ParsedShader& fs,
    const std::unordered_map<std::string, GLuint>& boundAttribs)
{
    ProgramLinkResult res;

    // 1. Verify varying type matching between VS and FS
    std::unordered_map<std::string, const ParsedVar*> vsVaryings;
    for (const auto& v : vs.varyings) vsVaryings[v.name] = &v;

    for (const auto& fv : fs.varyings) {
        auto it = vsVaryings.find(fv.name);
        if (it != vsVaryings.end()) {
            if (it->second->type != fv.type || it->second->arraySize != fv.arraySize) {
                res.success = false;
                res.errorLog = "Link error: varying type mismatch for '" + fv.name +
                               "': vs (" + it->second->type + ") != fs (" + fv.type + ")";
                return res;
            }
        }
    }

    // 2. Map varyings to locations deterministically
    std::set<std::string> allVaryingNames;
    for (const auto& v : vs.varyings) allVaryingNames.insert(v.name);
    for (const auto& v : fs.varyings) allVaryingNames.insert(v.name);

    std::unordered_map<std::string, int> varyingLocations;
    int nextVLoc = 0;
    for (const auto& name : allVaryingNames) {
        varyingLocations[name] = nextVLoc++;
        auto it = vsVaryings.find(name);
        if (it != vsVaryings.end() && it->second->isArray) {
            nextVLoc += (it->second->arraySize - 1);
        }
    }

    // 3. Map attributes to locations
    std::set<int> usedAttrLocs;
    for (const auto& attr : vs.attributes) {
        auto bIt = boundAttribs.find(attr.name);
        if (bIt != boundAttribs.end()) {
            usedAttrLocs.insert(static_cast<int>(bIt->second));
        } else if (attr.location >= 0) {
            usedAttrLocs.insert(attr.location);
        }
    }

    int nextAttrLoc = 0;
    for (const auto& attr : vs.attributes) {
        int loc = -1;
        auto bIt = boundAttribs.find(attr.name);
        if (bIt != boundAttribs.end()) {
            loc = static_cast<int>(bIt->second);
        } else if (attr.location >= 0) {
            loc = attr.location;
        } else {
            while (usedAttrLocs.count(nextAttrLoc)) nextAttrLoc++;
            loc = nextAttrLoc++;
            usedAttrLocs.insert(loc);
        }
        res.attribLocations[attr.name] = loc;

        VkAttribInfo info;
        info.name = attr.name;
        info.type = typeStringToGLenum(attr.type);
        info.size = attr.arraySize;
        info.location = loc;
        res.activeAttribs.push_back(info);
    }

    // 4. Map fragment outputs
    std::set<int> usedFragLocs;
    for (const auto& fo : fs.fragmentOutputs) {
        if (fo.location >= 0) usedFragLocs.insert(fo.location);
    }
    int nextFragLoc = 0;
    for (const auto& fo : fs.fragmentOutputs) {
        int loc = fo.location;
        if (loc < 0) {
            while (usedFragLocs.count(nextFragLoc)) nextFragLoc++;
            loc = nextFragLoc++;
            usedFragLocs.insert(loc);
        }
        res.fragDataLocations[fo.name] = loc;
    }

    // 5. Unify non-sampler uniforms across VS and FS
    std::vector<ParsedVar> unifiedUniforms;
    std::unordered_map<std::string, size_t> uniformIndexByName;

    auto addUniform = [&](const ParsedVar& u) {
        auto it = uniformIndexByName.find(u.name);
        if (it == uniformIndexByName.end()) {
            uniformIndexByName[u.name] = unifiedUniforms.size();
            unifiedUniforms.push_back(u);
        }
    };

    for (const auto& u : vs.uniforms) addUniform(u);
    for (const auto& u : fs.uniforms) addUniform(u);

    // Compute the std140 layout of the default uniform block
    uint32_t currentOffset = 0;
    int nextUniLoc = 0;
    std::string defaultBlockMembers;

    for (const auto& u : unifiedUniforms) {
        auto [baseSize, align] = getUniformSizeAndAlign(u.type);
        uint32_t totalSize = u.isArray ? (u.arraySize * alignTo(baseSize, 16)) : baseSize;
        currentOffset = alignTo(currentOffset, align);

        VkUniformInfo info;
        info.name = u.name + (u.isArray ? "[0]" : "");
        info.location = nextUniLoc++;
        info.type = typeStringToGLenum(u.type);
        info.offset = currentOffset;
        info.size = totalSize;
        info.count = u.arraySize;
        info.blockIndex = -1;
        info.matrixStride = (u.type.rfind("mat", 0) == 0) ? 16 : 0;
        info.isRowMajor = 0;
        info.arrayStride = u.isArray ? alignTo(baseSize, 16) : 0;

        res.uniformLocations[u.name] = info.location;
        if (u.isArray) res.uniformLocations[info.name] = info.location;
        res.uniforms.push_back(info);

        defaultBlockMembers += "    " + u.type + " " + u.name;
        if (u.isArray) defaultBlockMembers += "[" + std::to_string(u.arraySize) + "]";
        defaultBlockMembers += ";\n";

        currentOffset += totalSize;
    }
    res.defaultUniformSize = alignTo(currentOffset, 16);

    // 6. Map Samplers
    std::vector<ParsedVar> unifiedSamplers;
    std::set<std::string> samplerNames;
    auto addSampler = [&](const ParsedVar& s) {
        if (!samplerNames.count(s.name)) {
            samplerNames.insert(s.name);
            unifiedSamplers.push_back(s);
        }
    };
    for (const auto& s : vs.samplers) addSampler(s);
    for (const auto& s : fs.samplers) addSampler(s);

    int nextSampBinding = 0;
    std::string samplerDecls;
    for (const auto& s : unifiedSamplers) {
        int b = nextSampBinding++;
        res.samplerBindings[s.name] = b;

        VkUniformInfo info;
        info.name = s.name;
        info.location = nextUniLoc++;
        info.type = typeStringToGLenum(s.type);
        info.offset = 0;
        info.size = 0;
        info.count = 1;
        info.blockIndex = -1;
        info.matrixStride = 0;
        info.isRowMajor = 0;
        info.arrayStride = 0;
        res.uniformLocations[s.name] = info.location;
        res.uniforms.push_back(info);

        samplerDecls += "layout(binding = " + std::to_string(b) + ") uniform " + s.type + " " + s.name + ";\n";
    }

    // 7. Map Uniform Blocks
    std::vector<ParsedUniformBlock> unifiedUBOs;
    std::set<std::string> uboNames;
    auto addUbo = [&](const ParsedUniformBlock& ub) {
        if (!uboNames.count(ub.name)) {
            uboNames.insert(ub.name);
            unifiedUBOs.push_back(ub);
        }
    };
    for (const auto& ub : vs.uniformBlocks) addUbo(ub);
    for (const auto& ub : fs.uniformBlocks) addUbo(ub);

    std::string uboDecls;
    int nextUboBinding = kFirstUniformBlockBinding;
    GLuint blockIdx = 0;
    for (const auto& ub : unifiedUBOs) {
        int b = nextUboBinding++;
        res.uniformBlockIndices[ub.name] = blockIdx;

        VkUniformBlockInfo bInfo;
        bInfo.name = ub.name;
        bInfo.index = blockIdx;
        bInfo.binding = (ub.binding >= 0) ? static_cast<GLuint>(ub.binding) : 0;
        bInfo.descriptorBinding = static_cast<GLuint>(b);
        bInfo.referencedByVertex = std::any_of(vs.uniformBlocks.begin(), vs.uniformBlocks.end(),
                                               [&](const ParsedUniformBlock& x){ return x.name == ub.name; });
        bInfo.referencedByFragment = std::any_of(fs.uniformBlocks.begin(), fs.uniformBlocks.end(),
                                                 [&](const ParsedUniformBlock& x){ return x.name == ub.name; });

        uint32_t currentUboOffset = 0;
        for (const auto& member : ub.members) {
            auto [baseSize, align] = getUniformSizeAndAlign(member.type);
            bool isMat = (member.type.rfind("mat", 0) == 0);
            uint32_t memberAlign = align;
            if (member.isArray || isMat) {
                memberAlign = alignTo(memberAlign, 16);
            }
            currentUboOffset = alignTo(currentUboOffset, memberAlign);

            uint32_t totalSize = member.isArray ? (member.arraySize * alignTo(baseSize, 16)) : baseSize;

            VkUniformInfo uInfo;
            uInfo.name = member.name + (member.isArray ? "[0]" : "");
            uInfo.location = -1;
            uInfo.type = typeStringToGLenum(member.type);
            uInfo.offset = currentUboOffset;
            uInfo.size = totalSize;
            uInfo.count = member.arraySize;
            uInfo.blockIndex = static_cast<GLint>(blockIdx);
            uInfo.matrixStride = isMat ? 16 : 0;
            uInfo.isRowMajor = 0;
            uInfo.arrayStride = member.isArray ? alignTo(baseSize, 16) : 0;

            GLuint uIdx = static_cast<GLuint>(res.uniforms.size());
            bInfo.activeUniformIndices.push_back(uIdx);
            res.uniformLocations[member.name] = -1;
            if (member.isArray) res.uniformLocations[uInfo.name] = -1;
            res.uniforms.push_back(uInfo);

            currentUboOffset += totalSize;
        }

        bInfo.dataSize = alignTo(currentUboOffset, 16);
        res.uniformBlocks.push_back(bInfo);

        uboDecls += "layout(binding = " + std::to_string(b) + ", std140) uniform " + ub.name + " " + ub.body + "\n";
        blockIdx++;
    }

    // Common preamble
    std::string defaultBlock;
    if (!defaultBlockMembers.empty()) {
        defaultBlock = "layout(binding = " + std::to_string(kDefaultUniformBinding) +
                    ", std140) uniform WebGLUniforms {\n" + defaultBlockMembers + "};\n";
    }

    // Build Vertex Vulkan GLSL
    {
        std::ostringstream vout;
        vout << "#version 450\n";
        vout << "#define texture2D texture\n#define textureCube texture\n";
        for (const auto& attr : vs.attributes) {
            int loc = res.attribLocations[attr.name];
            vout << "layout(location = " << loc << ") in ";
            if (!attr.qualifiers.empty()) vout << attr.qualifiers << " ";
            vout << attr.type << " " << attr.name;
            if (attr.isArray) vout << "[" << attr.arraySize << "]";
            vout << ";\n";
        }
        for (const auto& v : vs.varyings) {
            int loc = varyingLocations[v.name];
            vout << "layout(location = " << loc << ") ";
            if (!v.qualifiers.empty()) vout << v.qualifiers << " ";
            vout << "out " << v.type << " " << v.name;
            if (v.isArray) vout << "[" << v.arraySize << "]";
            vout << ";\n";
        }
        vout << samplerDecls;
        vout << uboDecls;
        vout << defaultBlock;

        // Wrap main() to adjust gl_Position.z from OpenGL [-1, 1] to Vulkan [0, 1]
        std::string vSource = vs.cleanedSource;
        std::regex mainRegex(R"(\bvoid\s+main\s*\(\s*(?:void)?\s*\))");
        if (std::regex_search(vSource, mainRegex)) {
            vSource = std::regex_replace(vSource, mainRegex, "void _bro_user_main()", std::regex_constants::format_first_only);
            vout << vSource << "\n";
            vout << "void main() {\n";
            vout << "    _bro_user_main();\n";
            vout << "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
            vout << "}\n";
        } else {
            vout << vSource;
        }
        res.vsVulkanSource = vout.str();
    }

    // Build Fragment Vulkan GLSL
    {
        std::ostringstream fout;
        fout << "#version 450\n";
        fout << "#define texture2D texture\n#define textureCube texture\n";
        for (const auto& v : fs.varyings) {
            int loc = varyingLocations[v.name];
            fout << "layout(location = " << loc << ") ";
            if (!v.qualifiers.empty()) fout << v.qualifiers << " ";
            fout << "in " << v.type << " " << v.name;
            if (v.isArray) fout << "[" << v.arraySize << "]";
            fout << ";\n";
        }
        if (!fs.fragmentOutputs.empty()) {
            for (const auto& fo : fs.fragmentOutputs) {
                int loc = res.fragDataLocations[fo.name];
                fout << "layout(location = " << loc << ") out " << fo.type << " " << fo.name;
                if (fo.isArray) fout << "[" << fo.arraySize << "]";
                fout << ";\n";
            }
        } else {
            fout << "layout(location = 0) out vec4 bro_FragColor;\n";
            fout << "#define gl_FragColor bro_FragColor\n";
            res.fragDataLocations["gl_FragColor"] = 0;
        }
        fout << samplerDecls;
        fout << uboDecls;
        fout << defaultBlock;
        fout << fragmentWindowSpace(fs);
        fout << fs.cleanedSource;
        res.fsVulkanSource = fout.str();
    }

    res.success = true;
    return res;
}

} // namespace bro::webgl::vk
