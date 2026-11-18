// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * WebGL2RenderingContext — WebGL 2.0 Graphics Rendering Pipeline
 * =============================================================================
 *
 * OpenGL ES 3.0 compatible 3D hardware-accelerated rendering interface for WebGL2.
 * Includes buffer objects, textures, shaders, programs, framebuffers, renderbuffers,
 * vertex array objects, samplers, queries, and sync primitives.
 *
 * Every interface is a global: `WebGL2RenderingContext`, `WebGLRenderingContext`
 * and the handle types (`WebGLBuffer`, `WebGLTexture`, `WebGLProgram`, `WebGLShader`,
 * `WebGLFramebuffer`, `WebGLRenderbuffer`, `WebGLVertexArrayObject`, `WebGLQuery`,
 * `WebGLSampler`, `WebGLSync`, `WebGLTransformFeedback`, `WebGLUniformLocation`).
 * None can be constructed; each object a context hands out is an instance of its
 * type (`gl.createBuffer() instanceof WebGLBuffer`). The context's methods,
 * constants and the `drawingBufferWidth` / `drawingBufferHeight` accessors
 * live on `WebGL2RenderingContext.prototype` (a context's one own property is
 * `canvas`), and calling one on anything but a context throws a TypeError
 * ("Illegal invocation"). `getContext`, `toDataURL`, `toBlob`, `width` and
 * `height` live on `HTMLCanvasElement.prototype`.
 *
 * WebGL 1: `getContext('webgl')` (or the legacy `'experimental-webgl'`) is a
 * `WebGLRenderingContext`, an interface unrelated to `WebGL2RenderingContext`
 * as in a browser: its prototype has WebGL 1's methods and constants only (no
 * `bindVertexArray`, `texImage3D`, `RGBA32F`, ...), `VERSION` is "WebGL 1.0" and
 * `SHADING_LANGUAGE_VERSION` "WebGL GLSL ES 1.0", and a `#version 300 es`
 * shader fails to compile. It runs on the same backend, so its extensions are
 * the WebGL 2 functionality under WebGL 1's names: `ANGLE_instanced_arrays`
 * (`drawArraysInstancedANGLE`, `drawElementsInstancedANGLE`,
 * `vertexAttribDivisorANGLE`), `OES_vertex_array_object` (`createVertexArrayOES`,
 * `bindVertexArrayOES`, `isVertexArrayOES`, `deleteVertexArrayOES`),
 * `WEBGL_draw_buffers` (`drawBuffersWEBGL`, `COLOR_ATTACHMENTi_WEBGL`),
 * `OES_texture_float` / `OES_texture_half_float` (unsized RGBA / RGB /
 * LUMINANCE / ALPHA textures of FLOAT or `HALF_FLOAT_OES`), their `_linear`
 * forms, `WEBGL_depth_texture` (DEPTH_COMPONENT of UNSIGNED_SHORT / UNSIGNED_INT,
 * DEPTH_STENCIL of `UNSIGNED_INT_24_8_WEBGL`), `EXT_sRGB` (`SRGB_EXT`,
 * `SRGB_ALPHA_EXT`), `OES_element_index_uint`, `OES_standard_derivatives`,
 * `EXT_frag_depth`, `EXT_shader_texture_lod`, `EXT_blend_minmax`,
 * `OES_fbo_render_mipmap`, `WEBGL_lose_context`, `WEBGL_debug_renderer_info`,
 * and, as the device allows, `WEBGL_color_buffer_float`,
 * `EXT_color_buffer_half_float`, `EXT_float_blend`, `OES_texture_float_linear`,
 * `EXT_texture_filter_anisotropic` and the compressed-texture extensions.
 * Its vertex array objects are `WebGLVertexArrayObjectOES` (no global of that
 * name, as in a browser). WebGL 2's enums are refused as Chromium refuses them:
 * a WebGL 2 `getParameter` / `enable` / `pixelStorei` / `texParameter` name,
 * buffer target, texture target or framebuffer target is INVALID_ENUM (the
 * call does nothing, `getParameter` answers null), as are `MIN` / `MAX` before
 * `EXT_blend_minmax`, a texture type before its extension (FLOAT,
 * `HALF_FLOAT_OES`, the depth types) and a renderbuffer format outside ES 2.0's
 * and its extensions'; a sized `texImage2D` internal format is INVALID_VALUE.
 *
 * Context attributes (`canvas.getContext('webgl2', attributes)`), as granted and
 * as `getContextAttributes()` reports them:
 * - `alpha` (default true), `depth` (true), `stencil` (false): the drawing buffer
 *   has that buffer exactly when asked. Without `alpha` its alpha is one — clears
 *   and draws keep it there, `ALPHA_BITS` is 0, `DST_ALPHA` reads one, readPixels
 *   and copies read one — and it composites opaque.
 * - `antialias`: always granted false (the drawing buffer is never multisampled;
 *   render to a multisampled renderbuffer and blit for MSAA).
 * - `premultipliedAlpha`: always granted true (the compositor blends premultiplied).
 * - `preserveDrawingBuffer` (false): once the compositor has shown the drawing
 *   buffer, it reads as cleared (transparent black, opaque black without alpha;
 *   depth 1, stencil 0) to the next draw, read or copy — a `clear()` of all of it
 *   being the clear it was owed. With it, the drawing buffer keeps its contents.
 * - `powerPreference` (`'default'`, `'low-power'`, `'high-performance'`): kept
 *   and reported (one GPU serves every context); another string is a TypeError.
 * - `failIfMajorPerformanceCaveat`: reported as asked; `desynchronized` and
 *   `xrCompatible`: granted false.
 *
 * A canvas that is not in a document starts at 300x150, as on the web (one in
 * the document without width/height attributes is sized to the viewport).
 * Shaders: a GLSL variable declared without an initializer (global or local)
 * reads as zero, as ANGLE makes it. Uniform data (`uniform*v`,
 * `uniformMatrix*fv`): a typed array of the list's own type (Float32Array,
 * Int32Array, Uint32Array) is read in place; any other typed array or a plain
 * array is converted element by element (a Float64Array of matrices works).
 * Identifiers that are Vulkan GLSL keywords but not GLSL ES ones (`sampler`,
 * `buffer`, `shared`, `texture2D` in ES 3.00, ...) compile and keep their
 * names, as do GLSL 450 built-ins ES 1.00 lacks used as names in ES 1.00
 * source (`vec4 texture = texture2D(...)`, a uniform named `sample`). `getUniformLocation('a')` finds element 0 of any uniform array,
 * one-element arrays included. `renderbufferStorage(RENDERBUFFER, DEPTH_STENCIL,
 * ...)` (WebGL 1's unsized form) allocates DEPTH24_STENCIL8.
 *
 * Limits are the Vulkan device's, capped where a WebGL implementation is:
 * `MAX_TEXTURE_IMAGE_UNITS` / `MAX_VERTEX_TEXTURE_IMAGE_UNITS` are 32 and
 * `MAX_COMBINED_TEXTURE_IMAGE_UNITS` 64 wherever the device's per-stage and
 * per-set descriptor limits allow.
 *
 * Programs report what the linked shaders use: `ACTIVE_UNIFORMS` and
 * `getActiveUniform` list only the uniforms live code reads (plus every member
 * of a uniform block), and `getUniformLocation` of a declared but unread uniform
 * is null. A compile or link with nothing to say has an empty info log.
 *
 * Extensions beyond the core: `EXT_color_buffer_float`, `EXT_color_buffer_half_float`,
 * `EXT_float_blend`, `OES_texture_float_linear`, `EXT_texture_filter_anisotropic`,
 * the compressed formats the device samples, `EXT_disjoint_timer_query_webgl2`,
 * `KHR_parallel_shader_compile` (`COMPLETION_STATUS_KHR` turns true once the
 * WebGL thread has finished the compile or link, and never waits for it),
 * `WEBGL_debug_renderer_info` (`UNMASKED_VENDOR_WEBGL` / `UNMASKED_RENDERER_WEBGL`
 * name the Vulkan device), `WEBGL_lose_context` and `BRO_buffer_map`; on both
 * versions, as the device has the feature, `EXT_depth_clamp` (`DEPTH_CLAMP_EXT`),
 * `EXT_polygon_offset_clamp` (`polygonOffsetClampEXT`), `WEBGL_polygon_mode`
 * (`polygonModeWEBGL(FRONT_AND_BACK, LINE_WEBGL)`, `POLYGON_OFFSET_LINE_WEBGL`),
 * `EXT_texture_mirror_clamp_to_edge` (`MIRROR_CLAMP_TO_EDGE_EXT`), and
 * `WEBGL_debug_shaders` (`getTranslatedShaderSource` answers a compiled
 * shader's source). Not offered: `EXT_clip_control` (the vertex stage's
 * depth remap is compiled into every program) and `WEBGL_multi_draw` (its
 * `gl_DrawID` needs a per-draw constant in every program). An extension's
 * enums are enums only after its `getExtension`; its object's toString names
 * its interface (`[object OESVertexArrayObject]`).
 *
 * Threading: a call returns as soon as it is recorded. Each context encodes its
 * calls into a command ring on the JS thread, copying their data (typed arrays,
 * strings, canvas / video / ImageData pixels; a decoded image is held, not
 * copied), so an array may change right after the call. The engine's WebGL
 * thread replays the ring in call order and owns the Vulkan work, overlapping
 * the page's script; a frame's commands are submitted when the frame is
 * composited. At a frame's end the drawing buffer is copied, on the WebGL
 * thread, into the image the compositor samples, and the live frame loop
 * (a window, `runFrames`) does not wait for that: the script's next frame
 * runs while the WebGL thread finishes replaying this one, at most one frame
 * ahead, so a window may show the frame before the one just drawn when the
 * replay is the longer. Reads of the canvas (`readPixels`, `toDataURL`,
 * `createImageBitmap`) and captures (`screenshot`, `getPixel`,
 * `frameImage`) always see the frame just drawn. Errors keep their order: `getError` reports what the calls before
 * it raised. Answered without the WebGL thread: constant `getParameter`
 * values, extensions, uniform and attribute locations and a program's or
 * shader's status and info log (once its last link or compile has replayed:
 * the first read after one waits for that), query availability, sync status.
 * Round trips, which wait until the WebGL thread has replayed every call
 * before them, so are worth keeping out of a frame loop: `getError`,
 * `isEnabled` and the `is*` object checks, `getParameter` of live state (the
 * current bindings, viewport, clear color, ...), the other `get*Parameter`
 * calls, `getUniform`, `getVertexAttrib`, `checkFramebufferStatus`,
 * `getBufferSubData`, `readPixels` into client memory (which also waits for
 * the GPU), `finish`, `BRO_buffer_map`'s mapping, and reading the canvas
 * (`toDataURL`, `drawImage` of it, `createImageBitmap`). A blocking
 * `clientWaitSync` (a timeout above zero) waits for the GPU itself.
 * @example
 * const gl = canvas.getContext('webgl2');
 *   gl.clearColor(0.0, 0.0, 0.0, 1.0);
 *   gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
 */
class WebGLBuffer {

}

class WebGLTexture {

}

class WebGLProgram {

}

class WebGLShader {

}

class WebGLFramebuffer {

}

class WebGLRenderbuffer {

}

class WebGLVertexArrayObject {

}

class WebGLUniformLocation {

}

class WebGLSampler {

}

class WebGLQuery {

}

class WebGLSync {

}

class WebGLTransformFeedback {

}

class WebGL2RenderingContext {

  /**
   * @readonly
   * @type {HTMLCanvasElement}
   */
  canvas;

  /**
   * @readonly
   * @type {number}
   */
  drawingBufferWidth;

  /**
   * @readonly
   * @type {number}
   */
  drawingBufferHeight;

  /**
   * @param {number} r
   * @param {number} g
   * @param {number} b
   * @param {number} a
   */
  clearColor(r, g, b, a) {}

  /**
   * @param {number} mask
   */
  clear(mask) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} width
   * @param {number} height
   */
  viewport(x, y, width, height) {}

  /**
   * @param {number} cap
   */
  enable(cap) {}

  /**
   * @param {number} cap
   */
  disable(cap) {}

  /**
   * @param {number} sfactor
   * @param {number} dfactor
   */
  blendFunc(sfactor, dfactor) {}

  /**
   * @param {number} func
   */
  depthFunc(func) {}

  /**
   * @returns {WebGLBuffer|null}
   */
  createBuffer() {}

  /**
   * @param {WebGLBuffer|null} buffer
   */
  deleteBuffer(buffer) {}

  /**
   * @param {number} target
   * @param {WebGLBuffer|null} buffer
   */
  bindBuffer(target, buffer) {}

  /**
   * @param {number} target
   * @param {(ArrayBuffer|ArrayBufferView|number)} data
   * @param {number} usage
   */
  bufferData(target, data, usage) {}

  /**
   * @returns {WebGLTexture|null}
   */
  createTexture() {}

  /**
   * @param {WebGLTexture|null} texture
   */
  deleteTexture(texture) {}

  /**
   * @param {number} target
   * @param {WebGLTexture|null} texture
   */
  bindTexture(target, texture) {}

  /**
   * @param {number} texture
   */
  activeTexture(texture) {}

  /**
   * @returns {WebGLProgram|null}
   */
  createProgram() {}

  /**
   * @param {WebGLProgram|null} program
   */
  deleteProgram(program) {}

  /**
   * @param {WebGLProgram|null} program
   */
  linkProgram(program) {}

  /**
   * @param {WebGLProgram|null} program
   */
  useProgram(program) {}

  /**
   * @param {number} type
   * @returns {WebGLShader|null}
   */
  createShader(type) {}

  /**
   * @param {WebGLShader|null} shader
   */
  deleteShader(shader) {}

  /**
   * @param {WebGLShader|null} shader
   * @param {string} source
   */
  shaderSource(shader, source) {}

  /**
   * @param {WebGLShader|null} shader
   */
  compileShader(shader) {}

  /**
   * @param {WebGLProgram|null} program
   * @param {WebGLShader|null} shader
   */
  attachShader(program, shader) {}

  /**
   * @returns {WebGLVertexArrayObject|null}
   */
  createVertexArray() {}

  /**
   * @param {WebGLVertexArrayObject|null} array
   */
  deleteVertexArray(array) {}

  /**
   * @param {WebGLVertexArrayObject|null} array
   */
  bindVertexArray(array) {}

  /**
   * @param {WebGLProgram|null} program
   * @param {string} name
   * @returns {WebGLUniformLocation|null}
   */
  getUniformLocation(program, name) {}

  /**
   * @param {number} mode
   * @param {number} first
   * @param {number} count
   */
  drawArrays(mode, first, count) {}

  /**
   * @param {number} mode
   * @param {number} count
   * @param {number} type
   * @param {number} offset
   */
  drawElements(mode, count, type, offset) {}

  /**
   * Read `dstBuffer.length` (or `length`) elements of the buffer bound to
   * `target`, from byte `srcByteOffset`. Only the requested bytes that the
   * GPU wrote (a `readPixels` into a pack buffer, transform feedback) are
   * read back. After a `fenceSync` that followed the write has signalled,
   * the read waits for nothing: `fenceSync` already copied a `*_READ`-hinted
   * buffer's GPU writes out to host memory (see the example).
   * @param {number} target
   * @param {number} srcByteOffset
   * @param {ArrayBufferView} dstBuffer
   * @param {number} [dstOffset]
   * @param {number} [length]
   * @example
   * // Asynchronous readback: no stall on any frame.
   * gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
   * gl.bufferData(gl.PIXEL_PACK_BUFFER, w * h * 16, gl.STREAM_READ);
   * gl.readPixels(0, 0, w, h, gl.RGBA, gl.FLOAT, 0);   // a GPU copy
   * let sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
   * // ... on later frames:
   * if (sync && gl.clientWaitSync(sync, 0, 0) !== gl.TIMEOUT_EXPIRED) {
   *     gl.deleteSync(sync); sync = null;
   *     gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, out);  // a memcpy
   * }
   */
  getBufferSubData(target, srcByteOffset, dstBuffer, dstOffset, length) {}

  /**
   * Into client memory (an `ArrayBufferView`) or, with a `PIXEL_PACK_BUFFER`
   * bound, a byte offset into it. Reading a buffer as its own format (RGBA8
   * as `RGBA`/`UNSIGNED_BYTE`, RGBA32F as `RGBA`/`FLOAT`, ...) is a plain
   * copy: into a pack buffer a GPU copy nothing waits for, into client
   * memory a row copy of the readback.
   * @param {number} x
   * @param {number} y
   * @param {number} width
   * @param {number} height
   * @param {number} format
   * @param {number} type
   * @param {ArrayBufferView|number} dstData
   */
  readPixels(x, y, width, height, format, type, dstData) {}

  /**
   * `ANY_SAMPLES_PASSED(_CONSERVATIVE)`, `TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN`,
   * and with `EXT_disjoint_timer_query_webgl2` enabled `TIME_ELAPSED_EXT`.
   * @param {number} target
   * @param {WebGLQuery} query
   */
  beginQuery(target, query) {}

  /**
   * @param {number} target
   */
  endQuery(target) {}

  /**
   * `QUERY_RESULT_AVAILABLE` (a boolean; never waits) or `QUERY_RESULT`
   * (waits for the query's own submission if it has not finished). A timer
   * query's result is nanoseconds.
   *
   * Availability follows the event loop, as the WebGL 2.0 spec requires: a
   * query is unavailable for the rest of the task that ended it (a `finish()`
   * included), and a value read in one task holds until control returns to the
   * event loop, so poll on a later frame or task, never in a loop.
   * @param {WebGLQuery} query
   * @param {number} pname
   * @returns {number|boolean|null}
   */
  getQueryParameter(query, pname) {}

  /**
   * `CURRENT_QUERY` (the active query of `target`, or null), or with
   * `EXT_disjoint_timer_query_webgl2` `QUERY_COUNTER_BITS_EXT` for
   * `TIME_ELAPSED_EXT` / `TIMESTAMP_EXT`.
   * @param {number} target
   * @param {number} pname
   * @returns {WebGLQuery|number|null}
   */
  getQuery(target, pname) {}

}

/**
 * =============================================================================
 * EXT_disjoint_timer_query_webgl2 — GPU timer queries
 * =============================================================================
 *
 * `gl.getExtension('EXT_disjoint_timer_query_webgl2')`, offered where the
 * graphics queue writes timestamps (every desktop GPU, and MoltenVK). Built on
 * Vulkan timestamp queries:
 *
 * - `TIME_ELAPSED_EXT` with `gl.beginQuery` / `gl.endQuery`: two timestamps,
 *   each written once the GPU work recorded before it has finished, so a run
 *   of queries around consecutive passes splits the GPU's time between them.
 *   Begin ends an open render pass (like an occlusion query); end does not.
 * - `queryCounterEXT(query, TIMESTAMP_EXT)`: one timestamp, the GPU clock when
 *   the work before it finished.
 * - Results are nanoseconds, from `gl.getQueryParameter(q, gl.QUERY_RESULT)`.
 *   `QUERY_RESULT_AVAILABLE` is false in the turn that recorded the query and
 *   turns true a frame or so later, without anything waiting on the GPU.
 * - `gl.getQuery(TIME_ELAPSED_EXT | TIMESTAMP_EXT, QUERY_COUNTER_BITS_EXT)`:
 *   the queue's valid timestamp bits (64 on desktop GPUs).
 * - `gl.getParameter(GPU_DISJOINT_EXT)`: always false; Vulkan reports no
 *   clock disruptions to bro.
 * - `gl.getParameter(TIMESTAMP_EXT)`: the GPU clock now, in nanoseconds (a
 *   lone timestamp submitted and waited for; the context's open work is not).
 *
 * Before `getExtension`, the four enums are `INVALID_ENUM` everywhere.
 *
 * @example
 * const ext = gl.getExtension('EXT_disjoint_timer_query_webgl2');
 * const pending = [];
 * function frame() {
 *     const q = gl.createQuery();
 *     gl.beginQuery(ext.TIME_ELAPSED_EXT, q);
 *     drawWater();
 *     gl.endQuery(ext.TIME_ELAPSED_EXT);
 *     pending.push(q);
 *     // results from earlier frames
 *     while (pending.length && gl.getQueryParameter(pending[0], gl.QUERY_RESULT_AVAILABLE)) {
 *         const p = pending.shift();
 *         if (!gl.getParameter(ext.GPU_DISJOINT_EXT))
 *             console.log('water pass', gl.getQueryParameter(p, gl.QUERY_RESULT) / 1e6, 'ms');
 *         gl.deleteQuery(p);
 *     }
 *     requestAnimationFrame(frame);
 * }
 */
class EXT_disjoint_timer_query_webgl2 {
  /** @type {number} 0x8864 */
  QUERY_COUNTER_BITS_EXT;
  /** @type {number} 0x88BF */
  TIME_ELAPSED_EXT;
  /** @type {number} 0x8E28 */
  TIMESTAMP_EXT;
  /** @type {number} 0x8FBB */
  GPU_DISJOINT_EXT;

  /**
   * Record a timestamp into `query` (target must be `TIMESTAMP_EXT`).
   * @param {WebGLQuery} query
   * @param {number} target
   */
  queryCounterEXT(query, target) {}
}

