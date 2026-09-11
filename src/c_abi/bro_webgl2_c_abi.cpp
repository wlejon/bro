// =============================================================================
// bro_webgl2_c_abi.cpp — C++ forwarding implementations for bro.webgl2
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_webgl2_c_abi.h"
#include <cstdint>
#include <string>

struct BroWebGLBufferImpl { int id = 0; };
struct BroWebGLTextureImpl { int id = 0; };
struct BroWebGLProgramImpl { int id = 0; };
struct BroWebGLShaderImpl { int id = 0; };
struct BroWebGLFramebufferImpl { int id = 0; };
struct BroWebGLRenderbufferImpl { int id = 0; };
struct BroWebGLVertexArrayObjectImpl { int id = 0; };
struct BroWebGLUniformLocationImpl { int id = 0; };
struct BroWebGLSamplerImpl { int id = 0; };
struct BroWebGLQueryImpl { int id = 0; };
struct BroWebGLSyncImpl { int id = 0; };
struct BroWebGLTransformFeedbackImpl { int id = 0; };

struct BroWebGL2RenderingContextImpl {
    int32_t canvasWidth = 300;
    int32_t canvasHeight = 150;
};

extern "C" {

// WebGLBuffer
void* bro_WebGLBuffer_create(void) { return new BroWebGLBufferImpl(); }
void bro_WebGLBuffer_destroy(void* self) { delete static_cast<BroWebGLBufferImpl*>(self); }

// WebGLTexture
void* bro_WebGLTexture_create(void) { return new BroWebGLTextureImpl(); }
void bro_WebGLTexture_destroy(void* self) { delete static_cast<BroWebGLTextureImpl*>(self); }

// WebGLProgram
void* bro_WebGLProgram_create(void) { return new BroWebGLProgramImpl(); }
void bro_WebGLProgram_destroy(void* self) { delete static_cast<BroWebGLProgramImpl*>(self); }

// WebGLShader
void* bro_WebGLShader_create(void) { return new BroWebGLShaderImpl(); }
void bro_WebGLShader_destroy(void* self) { delete static_cast<BroWebGLShaderImpl*>(self); }

// WebGLFramebuffer
void* bro_WebGLFramebuffer_create(void) { return new BroWebGLFramebufferImpl(); }
void bro_WebGLFramebuffer_destroy(void* self) { delete static_cast<BroWebGLFramebufferImpl*>(self); }

// WebGLRenderbuffer
void* bro_WebGLRenderbuffer_create(void) { return new BroWebGLRenderbufferImpl(); }
void bro_WebGLRenderbuffer_destroy(void* self) { delete static_cast<BroWebGLRenderbufferImpl*>(self); }

// WebGLVertexArrayObject
void* bro_WebGLVertexArrayObject_create(void) { return new BroWebGLVertexArrayObjectImpl(); }
void bro_WebGLVertexArrayObject_destroy(void* self) { delete static_cast<BroWebGLVertexArrayObjectImpl*>(self); }

// WebGLUniformLocation
void* bro_WebGLUniformLocation_create(void) { return new BroWebGLUniformLocationImpl(); }
void bro_WebGLUniformLocation_destroy(void* self) { delete static_cast<BroWebGLUniformLocationImpl*>(self); }

// WebGLSampler
void* bro_WebGLSampler_create(void) { return new BroWebGLSamplerImpl(); }
void bro_WebGLSampler_destroy(void* self) { delete static_cast<BroWebGLSamplerImpl*>(self); }

// WebGLQuery
void* bro_WebGLQuery_create(void) { return new BroWebGLQueryImpl(); }
void bro_WebGLQuery_destroy(void* self) { delete static_cast<BroWebGLQueryImpl*>(self); }

// WebGLSync
void* bro_WebGLSync_create(void) { return new BroWebGLSyncImpl(); }
void bro_WebGLSync_destroy(void* self) { delete static_cast<BroWebGLSyncImpl*>(self); }

// WebGLTransformFeedback
void* bro_WebGLTransformFeedback_create(void) { return new BroWebGLTransformFeedbackImpl(); }
void bro_WebGLTransformFeedback_destroy(void* self) { delete static_cast<BroWebGLTransformFeedbackImpl*>(self); }

// WebGL2RenderingContext
void* bro_WebGL2RenderingContext_create(void) { return new BroWebGL2RenderingContextImpl(); }
void bro_WebGL2RenderingContext_destroy(void* self) { delete static_cast<BroWebGL2RenderingContextImpl*>(self); }

int32_t bro_WebGL2RenderingContext_get_canvasWidth(void* self) {
    auto* c = static_cast<BroWebGL2RenderingContextImpl*>(self);
    return c ? c->canvasWidth : 300;
}

int32_t bro_WebGL2RenderingContext_get_canvasHeight(void* self) {
    auto* c = static_cast<BroWebGL2RenderingContextImpl*>(self);
    return c ? c->canvasHeight : 150;
}

void bro_WebGL2RenderingContext_clearColor(void* /*self*/, double /*r*/, double /*g*/, double /*b*/, double /*a*/) {}
void bro_WebGL2RenderingContext_clear(void* /*self*/, uint32_t /*mask*/) {}
void bro_WebGL2RenderingContext_viewport(void* /*self*/, int32_t /*x*/, int32_t /*y*/, int32_t /*width*/, int32_t /*height*/) {}
void bro_WebGL2RenderingContext_enable(void* /*self*/, uint32_t /*cap*/) {}
void bro_WebGL2RenderingContext_disable(void* /*self*/, uint32_t /*cap*/) {}
void bro_WebGL2RenderingContext_blendFunc(void* /*self*/, uint32_t /*sfactor*/, uint32_t /*dfactor*/) {}
void bro_WebGL2RenderingContext_depthFunc(void* /*self*/, uint32_t /*func*/) {}

void* bro_WebGL2RenderingContext_createBuffer(void* /*self*/) { return new BroWebGLBufferImpl(); }
void bro_WebGL2RenderingContext_deleteBuffer(void* /*self*/, void* buffer) { delete static_cast<BroWebGLBufferImpl*>(buffer); }
void bro_WebGL2RenderingContext_bindBuffer(void* /*self*/, uint32_t /*target*/, void* /*buffer*/) {}
void bro_WebGL2RenderingContext_bufferData(void* /*self*/, uint32_t /*target*/, void* /*data*/, uint32_t /*usage*/) {}

void* bro_WebGL2RenderingContext_createTexture(void* /*self*/) { return new BroWebGLTextureImpl(); }
void bro_WebGL2RenderingContext_deleteTexture(void* /*self*/, void* texture) { delete static_cast<BroWebGLTextureImpl*>(texture); }
void bro_WebGL2RenderingContext_bindTexture(void* /*self*/, uint32_t /*target*/, void* /*texture*/) {}
void bro_WebGL2RenderingContext_activeTexture(void* /*self*/, uint32_t /*texture*/) {}

void* bro_WebGL2RenderingContext_createProgram(void* /*self*/) { return new BroWebGLProgramImpl(); }
void bro_WebGL2RenderingContext_deleteProgram(void* /*self*/, void* program) { delete static_cast<BroWebGLProgramImpl*>(program); }
void bro_WebGL2RenderingContext_linkProgram(void* /*self*/, void* /*program*/) {}
void bro_WebGL2RenderingContext_useProgram(void* /*self*/, void* /*program*/) {}

void* bro_WebGL2RenderingContext_createShader(void* /*self*/, uint32_t /*type*/) { return new BroWebGLShaderImpl(); }
void bro_WebGL2RenderingContext_deleteShader(void* /*self*/, void* shader) { delete static_cast<BroWebGLShaderImpl*>(shader); }
void bro_WebGL2RenderingContext_shaderSource(void* /*self*/, void* /*shader*/, const char* /*source*/) {}
void bro_WebGL2RenderingContext_compileShader(void* /*self*/, void* /*shader*/) {}
void bro_WebGL2RenderingContext_attachShader(void* /*self*/, void* /*program*/, void* /*shader*/) {}

void* bro_WebGL2RenderingContext_createVertexArray(void* /*self*/) { return new BroWebGLVertexArrayObjectImpl(); }
void bro_WebGL2RenderingContext_deleteVertexArray(void* /*self*/, void* array) { delete static_cast<BroWebGLVertexArrayObjectImpl*>(array); }
void bro_WebGL2RenderingContext_bindVertexArray(void* /*self*/, void* /*array*/) {}

void* bro_WebGL2RenderingContext_getUniformLocation(void* /*self*/, void* /*program*/, const char* /*name*/) {
    return new BroWebGLUniformLocationImpl();
}
void bro_WebGL2RenderingContext_drawArrays(void* /*self*/, uint32_t /*mode*/, int32_t /*first*/, int32_t /*count*/) {}
void bro_WebGL2RenderingContext_drawElements(void* /*self*/, uint32_t /*mode*/, int32_t /*count*/, uint32_t /*type*/, int32_t /*offset*/) {}

} // extern "C"
