#include "webgl/webgl2_context.h"
#include "webgl/glsl_translator.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl {

// ===========================================================================
// VAO
// ===========================================================================

WebGLVertexArrayObject WebGL2RenderingContext::createVertexArray() {
    if (vkCtx_) return vkCtx_->createVertexArray();
    GLuint id = 0;
    glGenVertexArrays(1, &id);
    validVAOs_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteVertexArray(WebGLVertexArrayObject vao) {
    if (vkCtx_) { vkCtx_->deleteVertexArray(vao); return; }
    if (vao.id && validVAOs_.erase(vao.id)) {
        if (sVAO_ == vao.id) sVAO_ = 0; // GL reverts to the default VAO
        glDeleteVertexArrays(1, &vao.id);
    }
}

void WebGL2RenderingContext::bindVertexArray(WebGLVertexArrayObject vao) {
    if (vkCtx_) { vkCtx_->bindVertexArray(vao); return; }
    sVAO_ = vao.id;
    glBindVertexArray(vao.id);
}

// ===========================================================================
// Vertex attributes
// ===========================================================================

void WebGL2RenderingContext::vertexAttribPointer(GLuint index, GLint size, GLenum type,
                                                  GLboolean normalized, GLsizei stride, GLintptr offset) {
    if (vkCtx_) { vkCtx_->vertexAttribPointer(index, size, type, normalized, stride, static_cast<uintptr_t>(offset)); return; }
    glVertexAttribPointer(index, size, type, normalized, stride, (const void*)offset);
}

void WebGL2RenderingContext::vertexAttribIPointer(GLuint index, GLint size, GLenum type,
                                                   GLsizei stride, GLintptr offset) {
    glVertexAttribIPointer(index, size, type, stride, (const void*)offset);
}

void WebGL2RenderingContext::enableVertexAttribArray(GLuint index) {
    if (vkCtx_) { vkCtx_->enableVertexAttribArray(index); return; }
    glEnableVertexAttribArray(index);
}
void WebGL2RenderingContext::disableVertexAttribArray(GLuint index) {
    if (vkCtx_) { vkCtx_->disableVertexAttribArray(index); return; }
    glDisableVertexAttribArray(index);
}
void WebGL2RenderingContext::vertexAttribDivisor(GLuint index, GLuint divisor) {
    if (vkCtx_) { vkCtx_->vertexAttribDivisor(index, divisor); return; }
    glVertexAttribDivisor(index, divisor);
}
void WebGL2RenderingContext::vertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w) { glVertexAttribI4i(index, x, y, z, w); }
void WebGL2RenderingContext::vertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) { glVertexAttribI4ui(index, x, y, z, w); }
void WebGL2RenderingContext::vertexAttribI4iv(GLuint index, const GLint* v) { glVertexAttribI4iv(index, v); }
void WebGL2RenderingContext::vertexAttribI4uiv(GLuint index, const GLuint* v) { glVertexAttribI4uiv(index, v); }

// ===========================================================================
// Shaders
// ===========================================================================

WebGLShader WebGL2RenderingContext::createShader(GLenum type) {
    if (vkCtx_) return vkCtx_->createShader(type);
    GLuint id = glCreateShader(type);
    validShaders_.insert(id);
    return {id, type};
}

void WebGL2RenderingContext::deleteShader(WebGLShader shader) {
    if (vkCtx_) { vkCtx_->deleteShader(shader); return; }
    if (shader.id && validShaders_.erase(shader.id)) {
        glDeleteShader(shader.id);
    }
}

void WebGL2RenderingContext::shaderSource(WebGLShader shader, const std::string& source) {
    if (vkCtx_) { vkCtx_->shaderSource(shader, source); return; }
    // Translate GLSL ES 3.00 → GLSL 3.30
    std::string translated = translateGLSL(source, shader.type);
    const char* src = translated.c_str();
    glShaderSource(shader.id, 1, &src, nullptr);
}

void WebGL2RenderingContext::compileShader(WebGLShader shader) {
    if (vkCtx_) { vkCtx_->compileShader(shader); return; }
    glCompileShader(shader.id);
}

GLboolean WebGL2RenderingContext::getShaderParameter_compileStatus(WebGLShader shader) {
    if (vkCtx_) return vkCtx_->getShaderParameter(shader, GL_COMPILE_STATUS);
    GLint ok = 0;
    glGetShaderiv(shader.id, GL_COMPILE_STATUS, &ok);
    return ok ? GL_TRUE : GL_FALSE;
}

std::string WebGL2RenderingContext::getShaderInfoLog(WebGLShader shader) {
    if (vkCtx_) return vkCtx_->getShaderInfoLog(shader);
    GLint len = 0;
    glGetShaderiv(shader.id, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return "";
    std::string log(len, '\0');
    glGetShaderInfoLog(shader.id, len, nullptr, log.data());
    // Trim trailing null
    while (!log.empty() && log.back() == '\0') log.pop_back();
    return log;
}

// ===========================================================================
// Programs
// ===========================================================================

WebGLProgram WebGL2RenderingContext::createProgram() {
    if (vkCtx_) return vkCtx_->createProgram();
    GLuint id = glCreateProgram();
    validPrograms_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteProgram(WebGLProgram program) {
    if (vkCtx_) { vkCtx_->deleteProgram(program); return; }
    if (program.id && validPrograms_.erase(program.id)) {
        glDeleteProgram(program.id);
    }
}

void WebGL2RenderingContext::attachShader(WebGLProgram program, WebGLShader shader) {
    if (vkCtx_) { vkCtx_->attachShader(program, shader); return; }
    glAttachShader(program.id, shader.id);
}

void WebGL2RenderingContext::detachShader(WebGLProgram program, WebGLShader shader) {
    if (vkCtx_) { vkCtx_->detachShader(program, shader); return; }
    glDetachShader(program.id, shader.id);
}

void WebGL2RenderingContext::linkProgram(WebGLProgram program) {
    if (vkCtx_) { vkCtx_->linkProgram(program); return; }
    glLinkProgram(program.id);
}

void WebGL2RenderingContext::useProgram(WebGLProgram program) {
    if (vkCtx_) { vkCtx_->useProgram(program); return; }
    sProgram_ = program.id;
    glUseProgram(program.id);
}

GLboolean WebGL2RenderingContext::getProgramParameter_linkStatus(WebGLProgram program) {
    if (vkCtx_) return vkCtx_->getProgramParameter(program, GL_LINK_STATUS);
    GLint ok = 0;
    glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
    return ok ? GL_TRUE : GL_FALSE;
}

std::string WebGL2RenderingContext::getProgramInfoLog(WebGLProgram program) {
    if (vkCtx_) return vkCtx_->getProgramInfoLog(program);
    GLint len = 0;
    glGetProgramiv(program.id, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return "";
    std::string log(len, '\0');
    glGetProgramInfoLog(program.id, len, nullptr, log.data());
    while (!log.empty() && log.back() == '\0') log.pop_back();
    return log;
}

void WebGL2RenderingContext::bindAttribLocation(WebGLProgram program, GLuint index, const std::string& name) {
    glBindAttribLocation(program.id, index, name.c_str());
}

GLint WebGL2RenderingContext::getAttribLocation(WebGLProgram program, const std::string& name) {
    if (vkCtx_) return vkCtx_->getAttribLocation(program, name);
    return glGetAttribLocation(program.id, name.c_str());
}

GLint WebGL2RenderingContext::getFragDataLocation(WebGLProgram program, const std::string& name) {
    return glGetFragDataLocation(program.id, name.c_str());
}

WebGLUniformLocation WebGL2RenderingContext::getUniformLocation(WebGLProgram program, const std::string& name) {
    if (vkCtx_) return vkCtx_->getUniformLocation(program, name);
    GLint loc = glGetUniformLocation(program.id, name.c_str());
    return {loc, program.id};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveAttrib(WebGLProgram program, GLuint index) {
    char name[256];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveAttrib(program.id, index, sizeof(name), &len, &size, &type, name);
    return {std::string(name, len), type, size};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveUniform(WebGLProgram program, GLuint index) {
    char name[256];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(program.id, index, sizeof(name), &len, &size, &type, name);
    return {std::string(name, len), type, size};
}

GLint WebGL2RenderingContext::getProgramParameter_int(WebGLProgram program, GLenum pname) {
    if (vkCtx_) return vkCtx_->getProgramParameter(program, pname);
    GLint val = 0;
    glGetProgramiv(program.id, pname, &val);
    return val;
}

GLuint WebGL2RenderingContext::getUniformBlockIndex(WebGLProgram program, const std::string& name) {
    return glGetUniformBlockIndex(program.id, name.c_str());
}

void WebGL2RenderingContext::uniformBlockBinding(WebGLProgram program, GLuint blockIndex, GLuint blockBinding) {
    glUniformBlockBinding(program.id, blockIndex, blockBinding);
}

std::vector<GLuint> WebGL2RenderingContext::getUniformIndices(WebGLProgram program,
                                                              const std::vector<std::string>& names) {
    std::vector<const char*> ptrs(names.size());
    for (size_t i = 0; i < names.size(); i++) ptrs[i] = names[i].c_str();
    std::vector<GLuint> indices(names.size(), GL_INVALID_INDEX);
    if (!ptrs.empty()) {
        glGetUniformIndices(program.id, (GLsizei)ptrs.size(), ptrs.data(), indices.data());
    }
    return indices;
}

std::vector<GLint> WebGL2RenderingContext::getActiveUniforms(WebGLProgram program,
                                                             const std::vector<GLuint>& indices,
                                                             GLenum pname) {
    std::vector<GLint> params(indices.size(), 0);
    if (!indices.empty()) {
        glGetActiveUniformsiv(program.id, (GLsizei)indices.size(), indices.data(),
                              pname, params.data());
    }
    return params;
}

GLint WebGL2RenderingContext::getActiveUniformBlockParameteri(WebGLProgram program,
                                                              GLuint blockIndex, GLenum pname) {
    GLint v = 0;
    glGetActiveUniformBlockiv(program.id, blockIndex, pname, &v);
    return v;
}

std::vector<GLint> WebGL2RenderingContext::getActiveUniformBlockIndices(WebGLProgram program,
                                                                        GLuint blockIndex) {
    GLint count = 0;
    glGetActiveUniformBlockiv(program.id, blockIndex,
                              GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
    if (count <= 0) return {};
    std::vector<GLint> indices(count, 0);
    glGetActiveUniformBlockiv(program.id, blockIndex,
                              GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, indices.data());
    return indices;
}

std::string WebGL2RenderingContext::getActiveUniformBlockName(WebGLProgram program,
                                                              GLuint blockIndex) {
    char name[256];
    GLsizei len = 0;
    glGetActiveUniformBlockName(program.id, blockIndex, sizeof(name), &len, name);
    return std::string(name, len);
}

// ===========================================================================
// Uniforms
// ===========================================================================

void WebGL2RenderingContext::uniform1f(WebGLUniformLocation loc, GLfloat x) {
    if (vkCtx_) { vkCtx_->uniform1f(loc, x); return; }
    glUniform1f(loc.location, x);
}
void WebGL2RenderingContext::uniform2f(WebGLUniformLocation loc, GLfloat x, GLfloat y) {
    if (vkCtx_) { vkCtx_->uniform2f(loc, x, y); return; }
    glUniform2f(loc.location, x, y);
}
void WebGL2RenderingContext::uniform3f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z) {
    if (vkCtx_) { vkCtx_->uniform3f(loc, x, y, z); return; }
    glUniform3f(loc.location, x, y, z);
}
void WebGL2RenderingContext::uniform4f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (vkCtx_) { vkCtx_->uniform4f(loc, x, y, z, w); return; }
    glUniform4f(loc.location, x, y, z, w);
}
void WebGL2RenderingContext::uniform1i(WebGLUniformLocation loc, GLint x) {
    if (vkCtx_) { vkCtx_->uniform1i(loc, x); return; }
    glUniform1i(loc.location, x);
}
void WebGL2RenderingContext::uniform2i(WebGLUniformLocation loc, GLint x, GLint y) { glUniform2i(loc.location, x, y); }
void WebGL2RenderingContext::uniform3i(WebGLUniformLocation loc, GLint x, GLint y, GLint z) { glUniform3i(loc.location, x, y, z); }
void WebGL2RenderingContext::uniform4i(WebGLUniformLocation loc, GLint x, GLint y, GLint z, GLint w) { glUniform4i(loc.location, x, y, z, w); }

void WebGL2RenderingContext::uniform1ui(WebGLUniformLocation loc, GLuint x) { glUniform1ui(loc.location, x); }
void WebGL2RenderingContext::uniform2ui(WebGLUniformLocation loc, GLuint x, GLuint y) { glUniform2ui(loc.location, x, y); }
void WebGL2RenderingContext::uniform3ui(WebGLUniformLocation loc, GLuint x, GLuint y, GLuint z) { glUniform3ui(loc.location, x, y, z); }
void WebGL2RenderingContext::uniform4ui(WebGLUniformLocation loc, GLuint x, GLuint y, GLuint z, GLuint w) { glUniform4ui(loc.location, x, y, z, w); }
void WebGL2RenderingContext::uniform1uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform1uiv(loc.location, count, v); }
void WebGL2RenderingContext::uniform2uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform2uiv(loc.location, count, v); }
void WebGL2RenderingContext::uniform3uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform3uiv(loc.location, count, v); }
void WebGL2RenderingContext::uniform4uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform4uiv(loc.location, count, v); }

void WebGL2RenderingContext::uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) { vkCtx_->uniform1fv(loc, count, v); return; }
    glUniform1fv(loc.location, count, v);
}
void WebGL2RenderingContext::uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) { vkCtx_->uniform2fv(loc, count, v); return; }
    glUniform2fv(loc.location, count, v);
}
void WebGL2RenderingContext::uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) { vkCtx_->uniform3fv(loc, count, v); return; }
    glUniform3fv(loc.location, count, v);
}
void WebGL2RenderingContext::uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) { vkCtx_->uniform4fv(loc, count, v); return; }
    glUniform4fv(loc.location, count, v);
}
void WebGL2RenderingContext::uniform1iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform1iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform2iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform2iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform3iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform3iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform4iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform4iv(loc.location, count, v); }

void WebGL2RenderingContext::uniformMatrix2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (vkCtx_) { vkCtx_->uniformMatrix3fv(loc, count, transpose, v); return; }
    glUniformMatrix3fv(loc.location, count, transpose, v);
}
void WebGL2RenderingContext::uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (vkCtx_) { vkCtx_->uniformMatrix4fv(loc, count, transpose, v); return; }
    glUniformMatrix4fv(loc.location, count, transpose, v);
}
void WebGL2RenderingContext::uniformMatrix2x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2x3fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3x2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix2x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2x4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4x2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3x4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4x3fv(loc.location, count, transpose, v); }


} // namespace bro::webgl
