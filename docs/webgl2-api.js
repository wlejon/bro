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
 * Note: `WebGL2RenderingContext` and `WebGLRenderingContext` are global constructors,
 * while handle types (`WebGLBuffer`, `WebGLTexture`, `WebGLProgram`, etc.) are
 * interface types returned by context methods (`createBuffer()`, `createTexture()`, etc.).
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

