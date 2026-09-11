// =============================================================================
// bro_webgl2_c_abi.h — Pure C-ABI declarations for bro.webgl2
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_WEBGL2_C_ABI_H
#define BRO_WEBGL2_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.webgl2.WebGLBuffer ---
void* bro_WebGLBuffer_create(void);
void  bro_WebGLBuffer_destroy(void* self);

// --- Interface bro.webgl2.WebGLTexture ---
void* bro_WebGLTexture_create(void);
void  bro_WebGLTexture_destroy(void* self);

// --- Interface bro.webgl2.WebGLProgram ---
void* bro_WebGLProgram_create(void);
void  bro_WebGLProgram_destroy(void* self);

// --- Interface bro.webgl2.WebGLShader ---
void* bro_WebGLShader_create(void);
void  bro_WebGLShader_destroy(void* self);

// --- Interface bro.webgl2.WebGLFramebuffer ---
void* bro_WebGLFramebuffer_create(void);
void  bro_WebGLFramebuffer_destroy(void* self);

// --- Interface bro.webgl2.WebGLRenderbuffer ---
void* bro_WebGLRenderbuffer_create(void);
void  bro_WebGLRenderbuffer_destroy(void* self);

// --- Interface bro.webgl2.WebGLVertexArrayObject ---
void* bro_WebGLVertexArrayObject_create(void);
void  bro_WebGLVertexArrayObject_destroy(void* self);

// --- Interface bro.webgl2.WebGLUniformLocation ---
void* bro_WebGLUniformLocation_create(void);
void  bro_WebGLUniformLocation_destroy(void* self);

// --- Interface bro.webgl2.WebGLSampler ---
void* bro_WebGLSampler_create(void);
void  bro_WebGLSampler_destroy(void* self);

// --- Interface bro.webgl2.WebGLQuery ---
void* bro_WebGLQuery_create(void);
void  bro_WebGLQuery_destroy(void* self);

// --- Interface bro.webgl2.WebGLSync ---
void* bro_WebGLSync_create(void);
void  bro_WebGLSync_destroy(void* self);

// --- Interface bro.webgl2.WebGLTransformFeedback ---
void* bro_WebGLTransformFeedback_create(void);
void  bro_WebGLTransformFeedback_destroy(void* self);

// --- Interface bro.webgl2.WebGL2RenderingContext ---
void* bro_WebGL2RenderingContext_create(void);
void  bro_WebGL2RenderingContext_destroy(void* self);
int32_t bro_WebGL2RenderingContext_get_canvasWidth(void* self);
int32_t bro_WebGL2RenderingContext_get_canvasHeight(void* self);
void bro_WebGL2RenderingContext_clearColor(void* self, double r, double g, double b, double a);
void bro_WebGL2RenderingContext_clear(void* self, uint32_t mask);
void bro_WebGL2RenderingContext_viewport(void* self, int32_t x, int32_t y, int32_t width, int32_t height);
void bro_WebGL2RenderingContext_enable(void* self, uint32_t cap);
void bro_WebGL2RenderingContext_disable(void* self, uint32_t cap);
void bro_WebGL2RenderingContext_blendFunc(void* self, uint32_t sfactor, uint32_t dfactor);
void bro_WebGL2RenderingContext_depthFunc(void* self, uint32_t func);
void* bro_WebGL2RenderingContext_createBuffer(void* self);
void bro_WebGL2RenderingContext_deleteBuffer(void* self, void* buffer);
void bro_WebGL2RenderingContext_bindBuffer(void* self, uint32_t target, void* buffer);
void bro_WebGL2RenderingContext_bufferData(void* self, uint32_t target, void* data, uint32_t usage);
void* bro_WebGL2RenderingContext_createTexture(void* self);
void bro_WebGL2RenderingContext_deleteTexture(void* self, void* texture);
void bro_WebGL2RenderingContext_bindTexture(void* self, uint32_t target, void* texture);
void bro_WebGL2RenderingContext_activeTexture(void* self, uint32_t texture);
void* bro_WebGL2RenderingContext_createProgram(void* self);
void bro_WebGL2RenderingContext_deleteProgram(void* self, void* program);
void bro_WebGL2RenderingContext_linkProgram(void* self, void* program);
void bro_WebGL2RenderingContext_useProgram(void* self, void* program);
void* bro_WebGL2RenderingContext_createShader(void* self, uint32_t type);
void bro_WebGL2RenderingContext_deleteShader(void* self, void* shader);
void bro_WebGL2RenderingContext_shaderSource(void* self, void* shader, const char* source);
void bro_WebGL2RenderingContext_compileShader(void* self, void* shader);
void bro_WebGL2RenderingContext_attachShader(void* self, void* program, void* shader);
void* bro_WebGL2RenderingContext_createVertexArray(void* self);
void bro_WebGL2RenderingContext_deleteVertexArray(void* self, void* array);
void bro_WebGL2RenderingContext_bindVertexArray(void* self, void* array);
void* bro_WebGL2RenderingContext_getUniformLocation(void* self, void* program, const char* name);
void bro_WebGL2RenderingContext_drawArrays(void* self, uint32_t mode, int32_t first, int32_t count);
void bro_WebGL2RenderingContext_drawElements(void* self, uint32_t mode, int32_t count, uint32_t type, int32_t offset);

#ifdef __cplusplus
}
#endif

#endif // BRO_WEBGL2_C_ABI_H
