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
    return {0};
}

void WebGL2RenderingContext::deleteVertexArray(WebGLVertexArrayObject vao) {
    if (vkCtx_) vkCtx_->deleteVertexArray(vao);
}

void WebGL2RenderingContext::bindVertexArray(WebGLVertexArrayObject vao) {
    if (vkCtx_) vkCtx_->bindVertexArray(vao);
}

// ===========================================================================
// Vertex attributes
// ===========================================================================

void WebGL2RenderingContext::vertexAttribPointer(GLuint index, GLint size, GLenum type,
                                                  GLboolean normalized, GLsizei stride, GLintptr offset) {
    if (vkCtx_) vkCtx_->vertexAttribPointer(index, size, type, normalized, stride, static_cast<uintptr_t>(offset));
}

void WebGL2RenderingContext::vertexAttribIPointer(GLuint /*index*/, GLint /*size*/, GLenum /*type*/,
                                                   GLsizei /*stride*/, GLintptr /*offset*/) {}

void WebGL2RenderingContext::enableVertexAttribArray(GLuint index) {
    if (vkCtx_) vkCtx_->enableVertexAttribArray(index);
}
void WebGL2RenderingContext::disableVertexAttribArray(GLuint index) {
    if (vkCtx_) vkCtx_->disableVertexAttribArray(index);
}
void WebGL2RenderingContext::vertexAttribDivisor(GLuint index, GLuint divisor) {
    if (vkCtx_) vkCtx_->vertexAttribDivisor(index, divisor);
}

// ===========================================================================
// Shaders & Programs
// ===========================================================================

WebGLShader WebGL2RenderingContext::createShader(GLenum type) {
    if (vkCtx_) return vkCtx_->createShader(type);
    return {0};
}

void WebGL2RenderingContext::deleteShader(WebGLShader s) {
    if (vkCtx_) vkCtx_->deleteShader(s);
}

void WebGL2RenderingContext::shaderSource(WebGLShader s, const std::string& source) {
    if (vkCtx_) vkCtx_->shaderSource(s, source);
}

void WebGL2RenderingContext::compileShader(WebGLShader s) {
    if (vkCtx_) vkCtx_->compileShader(s);
}

GLboolean WebGL2RenderingContext::getShaderParameter_compileStatus(WebGLShader shader) {
    if (vkCtx_) return vkCtx_->getShaderParameter(shader, 0x8B81 /* GL_COMPILE_STATUS */) ? GL_TRUE : GL_FALSE;
    return GL_FALSE;
}

std::string WebGL2RenderingContext::getShaderInfoLog(WebGLShader s) {
    if (vkCtx_) return vkCtx_->getShaderInfoLog(s);
    return "";
}

WebGLProgram WebGL2RenderingContext::createProgram() {
    if (vkCtx_) return vkCtx_->createProgram();
    return {0};
}

void WebGL2RenderingContext::deleteProgram(WebGLProgram p) {
    if (vkCtx_) vkCtx_->deleteProgram(p);
}

void WebGL2RenderingContext::attachShader(WebGLProgram p, WebGLShader s) {
    if (vkCtx_) vkCtx_->attachShader(p, s);
}

void WebGL2RenderingContext::detachShader(WebGLProgram p, WebGLShader s) {
    if (vkCtx_) vkCtx_->detachShader(p, s);
}

void WebGL2RenderingContext::linkProgram(WebGLProgram p) {
    if (vkCtx_) vkCtx_->linkProgram(p);
}

void WebGL2RenderingContext::useProgram(WebGLProgram p) {
    if (vkCtx_) vkCtx_->useProgram(p);
}

GLboolean WebGL2RenderingContext::getProgramParameter_linkStatus(WebGLProgram program) {
    if (vkCtx_) return vkCtx_->getProgramParameter(program, 0x8B82 /* GL_LINK_STATUS */) ? GL_TRUE : GL_FALSE;
    return GL_FALSE;
}

std::string WebGL2RenderingContext::getProgramInfoLog(WebGLProgram p) {
    if (vkCtx_) return vkCtx_->getProgramInfoLog(p);
    return "";
}

GLint WebGL2RenderingContext::getAttribLocation(WebGLProgram p, const std::string& name) {
    if (vkCtx_) return vkCtx_->getAttribLocation(p, name);
    return -1;
}

GLint WebGL2RenderingContext::getFragDataLocation(WebGLProgram /*program*/, const std::string& /*name*/) {
    return 0;
}

void WebGL2RenderingContext::bindAttribLocation(WebGLProgram /*p*/, GLuint /*index*/, const std::string& /*name*/) {}

WebGLUniformLocation WebGL2RenderingContext::getUniformLocation(WebGLProgram p, const std::string& name) {
    if (vkCtx_) return vkCtx_->getUniformLocation(p, name);
    return {-1};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveAttrib(WebGLProgram /*program*/, GLuint /*index*/) {
    return {};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveUniform(WebGLProgram /*program*/, GLuint /*index*/) {
    return {};
}

GLint WebGL2RenderingContext::getProgramParameter_int(WebGLProgram program, GLenum pname) {
    if (vkCtx_) return vkCtx_->getProgramParameter(program, pname);
    return 0;
}

GLuint WebGL2RenderingContext::getUniformBlockIndex(WebGLProgram /*program*/, const std::string& /*name*/) {
    return GL_INVALID_INDEX;
}

void WebGL2RenderingContext::uniformBlockBinding(WebGLProgram /*program*/, GLuint /*blockIndex*/, GLuint /*blockBinding*/) {}

std::vector<GLuint> WebGL2RenderingContext::getUniformIndices(WebGLProgram /*program*/,
                                                              const std::vector<std::string>& names) {
    return std::vector<GLuint>(names.size(), GL_INVALID_INDEX);
}

std::vector<GLint> WebGL2RenderingContext::getActiveUniforms(WebGLProgram /*program*/,
                                                             const std::vector<GLuint>& indices,
                                                             GLenum /*pname*/) {
    return std::vector<GLint>(indices.size(), 0);
}

GLint WebGL2RenderingContext::getActiveUniformBlockParameteri(WebGLProgram /*program*/,
                                                              GLuint /*blockIndex*/, GLenum /*pname*/) {
    return 0;
}

std::vector<GLint> WebGL2RenderingContext::getActiveUniformBlockIndices(WebGLProgram /*program*/,
                                                                        GLuint /*blockIndex*/) {
    return {};
}

std::string WebGL2RenderingContext::getActiveUniformBlockName(WebGLProgram /*program*/,
                                                              GLuint /*blockIndex*/) {
    return "";
}

// ===========================================================================
// Uniforms
// ===========================================================================

void WebGL2RenderingContext::uniform1f(WebGLUniformLocation loc, GLfloat x) {
    if (vkCtx_) vkCtx_->uniform1f(loc, x);
}
void WebGL2RenderingContext::uniform2f(WebGLUniformLocation loc, GLfloat x, GLfloat y) {
    if (vkCtx_) vkCtx_->uniform2f(loc, x, y);
}
void WebGL2RenderingContext::uniform3f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z) {
    if (vkCtx_) vkCtx_->uniform3f(loc, x, y, z);
}
void WebGL2RenderingContext::uniform4f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (vkCtx_) vkCtx_->uniform4f(loc, x, y, z, w);
}
void WebGL2RenderingContext::uniform1i(WebGLUniformLocation loc, GLint x) {
    if (vkCtx_) vkCtx_->uniform1i(loc, x);
}
void WebGL2RenderingContext::uniform2i(WebGLUniformLocation loc, GLint x, GLint y) {
    if (vkCtx_) vkCtx_->uniform2i(loc, x, y);
}
void WebGL2RenderingContext::uniform3i(WebGLUniformLocation /*loc*/, GLint /*x*/, GLint /*y*/, GLint /*z*/) {}
void WebGL2RenderingContext::uniform4i(WebGLUniformLocation /*loc*/, GLint /*x*/, GLint /*y*/, GLint /*z*/, GLint /*w*/) {}

void WebGL2RenderingContext::uniform1ui(WebGLUniformLocation /*loc*/, GLuint /*x*/) {}
void WebGL2RenderingContext::uniform2ui(WebGLUniformLocation /*loc*/, GLuint /*x*/, GLuint /*y*/) {}
void WebGL2RenderingContext::uniform3ui(WebGLUniformLocation /*loc*/, GLuint /*x*/, GLuint /*y*/, GLuint /*z*/) {}
void WebGL2RenderingContext::uniform4ui(WebGLUniformLocation /*loc*/, GLuint /*x*/, GLuint /*y*/, GLuint /*z*/, GLuint /*w*/) {}
void WebGL2RenderingContext::uniform1uiv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLuint* /*v*/) {}
void WebGL2RenderingContext::uniform2uiv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLuint* /*v*/) {}
void WebGL2RenderingContext::uniform3uiv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLuint* /*v*/) {}
void WebGL2RenderingContext::uniform4uiv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLuint* /*v*/) {}

void WebGL2RenderingContext::uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) vkCtx_->uniform1fv(loc, count, v);
}
void WebGL2RenderingContext::uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) vkCtx_->uniform2fv(loc, count, v);
}
void WebGL2RenderingContext::uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) vkCtx_->uniform3fv(loc, count, v);
}
void WebGL2RenderingContext::uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (vkCtx_) vkCtx_->uniform4fv(loc, count, v);
}
void WebGL2RenderingContext::uniform1iv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLint* /*v*/) {}
void WebGL2RenderingContext::uniform2iv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLint* /*v*/) {}
void WebGL2RenderingContext::uniform3iv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLint* /*v*/) {}
void WebGL2RenderingContext::uniform4iv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, const GLint* /*v*/) {}

void WebGL2RenderingContext::uniformMatrix2fv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, GLboolean /*transpose*/, const GLfloat* /*v*/) {}
void WebGL2RenderingContext::uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (vkCtx_) vkCtx_->uniformMatrix3fv(loc, count, transpose, v);
}
void WebGL2RenderingContext::uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (vkCtx_) vkCtx_->uniformMatrix4fv(loc, count, transpose, v);
}
void WebGL2RenderingContext::uniformMatrix2x3fv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, GLboolean /*transpose*/, const GLfloat* /*v*/) {}
void WebGL2RenderingContext::uniformMatrix3x2fv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, GLboolean /*transpose*/, const GLfloat* /*v*/) {}
void WebGL2RenderingContext::uniformMatrix2x4fv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, GLboolean /*transpose*/, const GLfloat* /*v*/) {}
void WebGL2RenderingContext::uniformMatrix4x2fv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, GLboolean /*transpose*/, const GLfloat* /*v*/) {}
void WebGL2RenderingContext::uniformMatrix3x4fv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, GLboolean /*transpose*/, const GLfloat* /*v*/) {}
void WebGL2RenderingContext::uniformMatrix4x3fv(WebGLUniformLocation /*loc*/, GLsizei /*count*/, GLboolean /*transpose*/, const GLfloat* /*v*/) {}

} // namespace bro::webgl
