// The client's shaders and programs: the source each shader was given, and
// what each link and compile left, published by the replay right after it
// ran (webgl_client_async.h). What a program reports about itself after a
// link (locations, active uniforms and attributes, blocks, its log) is a
// function of that publication (webgl_program_lookup.h), so the binding
// answers it here; the first read after a link waits for the link once.
//
// Only objects the client holds are answered: one deleted (the backend may
// still keep it, flagged, while it is in use or attached) or never made goes
// to the backend, which answers as it always did.

#include "webgl/client/webgl_client.h"

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_types.h"

namespace bro::webgl {

std::shared_ptr<const ProgramLink> emptyProgramLink() {
    static const std::shared_ptr<const ProgramLink> empty = [] {
        auto link = std::make_shared<ProgramLink>();
        link->iface = std::make_shared<const vk::ProgramInterface>();
        return std::shared_ptr<const ProgramLink>(std::move(link));
    }();
    return empty;
}

void replayLinkProgram(vk::WebGLVkContext& gl, WebGLProgram p, ProgramSlot* slot) {
    gl.linkProgram(p);
    if (!slot) return;
    auto link = std::make_shared<ProgramLink>();
    if (const vk::VkProgramResource* prog = gl.programObject(p.id)) {
        link->linked = prog->linkStatus;
        link->infoLog = prog->infoLog;
        link->iface = std::make_shared<const vk::ProgramInterface>(prog->iface);
    } else {
        link->iface = emptyProgramLink()->iface;
    }
    slot->link = std::move(link);
    slot->links.fetch_add(1, std::memory_order_release);
}

void replayCompileShader(vk::WebGLVkContext& gl, WebGLShader s, ShaderSlot* slot) {
    gl.compileShader(s);
    if (!slot) return;
    auto compile = std::make_shared<ShaderCompile>();
    compile->compiled = gl.getShaderParameter(s, GL_COMPILE_STATUS) == GL_TRUE;
    compile->infoLog = gl.getShaderInfoLog(s);
    slot->compile = std::move(compile);
    slot->compiles.fetch_add(1, std::memory_order_release);
}

// --- Shaders ----------------------------------------------------------------

bool WebGLClient::newShader(GLuint id, GLenum type) {
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        syntheticError(GL_INVALID_ENUM);
        return false;
    }
    HeldShader& s = shaders_[id];
    s = HeldShader{};
    s.slot = std::make_shared<ShaderSlot>();
    s.slot->compile = std::make_shared<const ShaderCompile>();
    createShader(id, type);
    return true;
}

void WebGLClient::dropShader(WebGLShader s) {
    shaders_.erase(s.id);
    deleteShader(s);
}

void WebGLClient::setShaderSource(WebGLShader s, std::string_view source) {
    if (auto it = shaders_.find(s.id); it != shaders_.end()) it->second.source.assign(source);
    shaderSource(s, source);
}

void WebGLClient::compileShader(WebGLShader s) {
    std::shared_ptr<ShaderSlot> slot;
    if (auto it = shaders_.find(s.id); it != shaders_.end()) {
        ++it->second.compiles;
        slot = it->second.slot;
    }
    encodeSimple(ring_, Op::CompileShader, rec::kCompileShader, s, holdRef(std::move(slot)));
}

const ShaderCompile* WebGLClient::shaderCompile(GLuint id) {
    auto it = id != 0 ? shaders_.find(id) : shaders_.end();
    if (it == shaders_.end()) return nullptr;
    const HeldShader& s = it->second;
    if (s.slot->compiles.load(std::memory_order_acquire) != s.compiles) drain();
    return s.slot->compile.get();
}

const std::string* WebGLClient::heldShaderSource(GLuint id) const {
    auto it = id != 0 ? shaders_.find(id) : shaders_.end();
    return it == shaders_.end() ? nullptr : &it->second.source;
}

bool WebGLClient::shaderSettled(GLuint id) const {
    auto it = id != 0 ? shaders_.find(id) : shaders_.end();
    return it != shaders_.end() && it->second.slot->compiles.load(std::memory_order_acquire) == it->second.compiles;
}

// --- Programs ---------------------------------------------------------------

void WebGLClient::newProgram(GLuint id) {
    HeldProgram& p = programs_[id];
    p = HeldProgram{};
    p.slot = std::make_shared<ProgramSlot>();
    p.slot->link = emptyProgramLink();
    createProgram(id);
}

void WebGLClient::dropProgram(WebGLProgram p) {
    programs_.erase(p.id);
    deleteProgram(p);
}

void WebGLClient::linkProgram(WebGLProgram p) {
    std::shared_ptr<ProgramSlot> slot;
    if (auto it = programs_.find(p.id); it != programs_.end()) {
        ++it->second.links;
        slot = it->second.slot;
    }
    encodeSimple(ring_, Op::LinkProgram, rec::kLinkProgram, p, holdRef(std::move(slot)));
}

const ProgramLink* WebGLClient::programLink(GLuint id) {
    auto it = id != 0 ? programs_.find(id) : programs_.end();
    if (it == programs_.end()) return nullptr;
    const HeldProgram& p = it->second;
    if (p.slot->links.load(std::memory_order_acquire) != p.links) drain();
    return p.slot->link.get();
}

bool WebGLClient::programSettled(GLuint id) const {
    auto it = id != 0 ? programs_.find(id) : programs_.end();
    return it != programs_.end() && it->second.slot->links.load(std::memory_order_acquire) == it->second.links;
}

}  // namespace bro::webgl
