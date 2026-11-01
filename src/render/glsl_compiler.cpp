#include "render/glsl_compiler.h"

#include <brass/target/spirv/spirv_binary_reader.hpp>
#include <brass/target/spirv/spirv_relaxed_precision.hpp>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace bro::render {

namespace {

// glslang keeps process-wide state (its symbol tables), initialised once and
// never finalised: a shader can be compiled at any point in the process's
// life, teardown included. Past that, a compile's state is its own thread's
// (its pool allocator is thread-local), so compiles run in parallel; the
// mutex guards the initialisation and the memo, and is what acquireGlslang
// hands WebGL's linker.
std::mutex s_mutex;
bool s_initialized = false;
std::unordered_map<std::string, std::vector<uint32_t>> s_memo;

thread_local GlslRelaxedPrecisionStats tl_lastRelaxedStats{};
thread_local GlslComputeAuditStats tl_lastAuditStats{};

bool envFlagEnabled(const char* name) {
    const char* v = std::getenv(name);
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "ON") == 0);
}

glslang_stage_t toGlslangStage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:   return GLSLANG_STAGE_VERTEX;
        case ShaderStage::Fragment: return GLSLANG_STAGE_FRAGMENT;
        case ShaderStage::Compute:  return GLSLANG_STAGE_COMPUTE;
    }
    return GLSLANG_STAGE_VERTEX;
}

void appendLog(std::string* log, const char* what, const char* text) {
    if (!log) return;
    log->append(what);
    if (text && *text) log->append(text);
}

// With s_mutex held.
bool initializeOnce() {
    if (!s_initialized) s_initialized = glslang_initialize_process() != 0;
    return s_initialized;
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}

// Replace every `#include "name"` line of `text` with the file `name` from
// `dir`, the include root (an included file's own includes are from it too).
bool expandIncludes(std::string& text, const std::string& dir, int depth,
                    std::vector<std::string>* deps, std::string* err) {
    if (depth > 8) {
        if (err) *err = "#include nested too deeply";
        return false;
    }
    std::string out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t hash = line.find_first_not_of(" \t");
        if (hash != std::string::npos && line.compare(hash, 8, "#include") == 0) {
            const size_t open = line.find('"', hash);
            const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
            if (close == std::string::npos) {
                if (err) *err = "malformed include: " + line;
                return false;
            }
            const std::string path = dir + line.substr(open + 1, close - open - 1);
            std::string included;
            if (!readTextFile(path, included)) {
                if (err) *err = "cannot read included " + path;
                return false;
            }
            if (deps) deps->push_back(path);
            if (!expandIncludes(included, dir, depth + 1, deps, err)) return false;
            out += included;
            out += '\n';
            continue;
        }
        out += line;
        out += '\n';
    }
    text = std::move(out);
    return true;
}

std::vector<uint32_t> compileRawGlslToSpirv(std::string_view source, ShaderStage stage,
                                            std::string* log) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!initializeOnce()) {
            appendLog(log, "glslang: process initialisation failed", nullptr);
            return {};
        }
    }

    const std::string code(source);  // glslang wants a NUL-terminated string
    const glslang_stage_t glStage = toGlslangStage(stage);
    const auto messages = static_cast<glslang_messages_t>(
        GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);

    glslang_input_t input{};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = glStage;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_0;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_0;
    input.code = code.c_str();
    input.default_version = 100;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = messages;
    input.resource = glslang_default_resource();

    glslang_shader_t* shader = glslang_shader_create(&input);
    if (!shader) {
        appendLog(log, "glslang: could not create a shader object", nullptr);
        return {};
    }

    std::vector<uint32_t> spirv;
    if (!glslang_shader_preprocess(shader, &input)) {
        appendLog(log, "", glslang_shader_get_info_log(shader));
    } else if (!glslang_shader_parse(shader, &input)) {
        appendLog(log, "", glslang_shader_get_info_log(shader));
    } else {
        glslang_program_t* program = glslang_program_create();
        glslang_program_add_shader(program, shader);
        if (!glslang_program_link(program, messages)) {
            appendLog(log, "", glslang_program_get_info_log(program));
        } else {
            glslang_program_SPIRV_generate(program, glStage);
            spirv.resize(glslang_program_SPIRV_get_size(program));
            glslang_program_SPIRV_get(program, spirv.data());
            if (const char* msg = glslang_program_SPIRV_get_messages(program); msg && *msg)
                appendLog(log, "", msg);
        }
        glslang_program_delete(program);
    }
    glslang_shader_delete(shader);

    if (spirv.empty() || spirv[0] != 0x07230203u) return {};
    return spirv;
}

} // namespace

bool preprocessGlslFile(const std::string& path, const std::vector<std::string>& defines,
                        std::string& out, std::vector<std::string>* deps, std::string* err,
                        const std::string& includeDir) {
    std::string text;
    if (!readTextFile(path, text)) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    if (deps) deps->push_back(path);
    std::string dir = includeDir;
    if (dir.empty()) {
        const size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) dir = path.substr(0, slash + 1);
    } else if (dir.back() != '/' && dir.back() != '\\') {
        dir += '/';
    }
    if (!expandIncludes(text, dir, 0, deps, err)) return false;
    if (!defines.empty()) {
        std::string lines;
        for (const auto& d : defines) lines += "#define " + d + " 1\n";
        const size_t version = text.find("#version");
        const size_t lineEnd = version == std::string::npos ? std::string::npos : text.find('\n', version);
        if (lineEnd == std::string::npos) {
            if (err) *err = path + " has no #version line to define after";
            return false;
        }
        text.insert(lineEnd + 1, lines);
    }
    out = std::move(text);
    return true;
}

std::unique_lock<std::mutex> acquireGlslang() {
    std::unique_lock<std::mutex> lock(s_mutex);
    if (!initializeOnce()) return {};
    return lock;
}

GlslRelaxedPrecisionStats promoteSpirvRelaxedPrecision(
    std::vector<uint32_t>& spirv,
    float maxAbsFp16,
    bool assumeStageInputsUnitRange) {
    GlslRelaxedPrecisionStats stats;
    stats.enabled = true;
    brass::spirv::SpirvRelaxedPrecisionOptions opts;
    opts.max_abs_fp16 = maxAbsFp16;
    opts.assume_stage_inputs_unit_range = assumeStageInputsUnitRange;
    const auto rep = brass::spirv::promote_relaxed_precision(spirv, opts);
    stats.parseOk = rep.parse_ok;
    stats.totalFloatDefs = rep.total_float_defs;
    stats.provenBoundedDefs = rep.proven_bounded_defs;
    stats.promotedCount = rep.promoted_count;
    stats.alreadyRelaxedCount = rep.already_relaxed_count;
    stats.keptFp32Count = rep.kept_fp32_count;
    stats.error = rep.error;
    tl_lastRelaxedStats = stats;
    return stats;
}

GlslComputeAuditStats auditComputeSpirv(const std::vector<uint32_t>& spirv) {
    GlslComputeAuditStats stats;
    stats.audited = true;
    const auto rep = brass::spirv::audit_compute_shader_binary(spirv);
    stats.parseOk = rep.parse_ok;
    stats.structuralOk = rep.structural_ok;
    stats.dominanceOk = rep.dominance_ok;
    stats.barriersUniform = rep.barriers_uniform;
    stats.sharedBoundsOk = rep.shared_bounds_ok;
    stats.divSqrtOk = rep.div_sqrt_ok;
    stats.valid = rep.parse_ok && rep.structural_ok && rep.dominance_ok;
    stats.provenSafe = rep.ok();
    stats.numFunctions = rep.num_functions;
    stats.numBlocks = rep.num_blocks;
    stats.numInstructions = rep.num_instructions;
    stats.hazardCount = static_cast<uint32_t>(rep.hazards.size());
    stats.divisionCount = static_cast<uint32_t>(rep.divisions.size());
    uint32_t safeDivs = 0;
    for (const auto& d : rep.divisions) {
        if (d.proven_safe) ++safeDivs;
    }
    stats.provenSafeDivisions = safeDivs;
    stats.barrierCount = static_cast<uint32_t>(rep.barriers.size());
    stats.localSize = rep.local_size;
    stats.findings.reserve(rep.hazards.size());
    for (const auto& h : rep.hazards) {
        stats.findings.push_back(h.message);
    }
    tl_lastAuditStats = stats;
    return stats;
}

GlslCompileResult compileGlslToSpirvWithOptions(
    std::string_view source, ShaderStage stage,
    const GlslCompileOptions& options) {
    GlslCompileResult result;
    result.spirv = compileRawGlslToSpirv(source, stage, &result.errorLog);
    if (result.spirv.empty()) return result;

    if (options.promoteRelaxedPrecision &&
        (stage == ShaderStage::Fragment || stage == ShaderStage::Compute)) {
        result.relaxedPrecision = promoteSpirvRelaxedPrecision(
            result.spirv,
            options.maxAbsFp16,
            options.assumeStageInputsUnitRange);
    }

    if (options.auditComputeShader && stage == ShaderStage::Compute) {
        result.computeAudit = auditComputeSpirv(result.spirv);
        if (options.failOnUnprovenCompute && !result.computeAudit.provenSafe) {
            appendLog(&result.errorLog,
                      "compute audit failed: ",
                      result.computeAudit.findings.empty()
                          ? "unproven hazard"
                          : result.computeAudit.findings.front().c_str());
            result.spirv.clear();
        }
    }

    return result;
}

std::vector<uint32_t> compileGlslToSpirv(std::string_view source, ShaderStage stage,
                                         std::string* log) {
    const bool envRelaxed = envFlagEnabled("BRO_SHADER_RELAXED_PRECISION") &&
                            (stage == ShaderStage::Fragment || stage == ShaderStage::Compute);
    const bool envAudit = envFlagEnabled("BRO_AUDIT_COMPUTE_SHADERS") &&
                          stage == ShaderStage::Compute;

    std::string key;
    key.reserve(source.size() + 6);
    key.push_back(static_cast<char>('0' + static_cast<int>(stage)));
    key.push_back(envRelaxed ? 'R' : 'r');
    key.push_back(envAudit ? 'A' : 'a');
    key.push_back('|');
    key.append(source);

    if (!envRelaxed && !envAudit) {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (auto it = s_memo.find(key); it != s_memo.end()) return it->second;
    }

    std::vector<uint32_t> spirv = compileRawGlslToSpirv(source, stage, log);
    if (spirv.empty()) return {};

    if (envRelaxed) {
        promoteSpirvRelaxedPrecision(spirv);
    }
    if (envAudit) {
        const auto audit = auditComputeSpirv(spirv);
        if (!audit.provenSafe && log) {
            for (const auto& f : audit.findings) {
                appendLog(log, "[audit] ", (f + "\n").c_str());
            }
        }
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    s_memo[key] = spirv;
    return spirv;
}

GlslCompileResult GlslCompiler::compileAndOptimize(
    std::string_view source, ShaderStage stage,
    const GlslCompileOptions& options) {
    return compileGlslToSpirvWithOptions(source, stage, options);
}

GlslRelaxedPrecisionStats GlslCompiler::promoteRelaxedPrecision(
    std::vector<uint32_t>& spirv,
    float maxAbsFp16,
    bool assumeStageInputsUnitRange) {
    return promoteSpirvRelaxedPrecision(spirv, maxAbsFp16, assumeStageInputsUnitRange);
}

GlslComputeAuditStats GlslCompiler::auditCompute(const std::vector<uint32_t>& spirv) {
    return auditComputeSpirv(spirv);
}

GlslRelaxedPrecisionStats GlslCompiler::lastRelaxedPrecisionStats() {
    return tl_lastRelaxedStats;
}

GlslComputeAuditStats GlslCompiler::lastComputeAuditStats() {
    return tl_lastAuditStats;
}

} // namespace bro::render
