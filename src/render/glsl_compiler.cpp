#include "render/glsl_compiler.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace bro::render {

namespace {

// glslang keeps process-wide state (its symbol tables and pool allocators),
// initialised once and never finalised: a shader can be compiled at any point
// in the process's life, teardown included. The same mutex serialises the
// compiles themselves, which keeps glslang's per-thread pools out of the
// picture and guards the memo.
std::mutex s_mutex;
bool s_initialized = false;
std::unordered_map<std::string, std::vector<uint32_t>> s_memo;

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

} // namespace

std::vector<uint32_t> compileGlslToSpirv(std::string_view source, ShaderStage stage,
                                         std::string* log) {
    std::lock_guard<std::mutex> lock(s_mutex);

    std::string key;
    key.reserve(source.size() + 2);
    key.push_back(static_cast<char>('0' + static_cast<int>(stage)));
    key.push_back('|');
    key.append(source);
    if (auto it = s_memo.find(key); it != s_memo.end()) return it->second;

    if (!s_initialized) {
        if (!glslang_initialize_process()) {
            appendLog(log, "glslang: process initialisation failed", nullptr);
            return {};
        }
        s_initialized = true;
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
    s_memo.emplace(std::move(key), spirv);
    return spirv;
}

} // namespace bro::render
