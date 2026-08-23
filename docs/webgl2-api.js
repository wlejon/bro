// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * WebGL2RenderingContext — WebGL 2.0 Graphics Rendering Pipeline
 * =============================================================================
 *
 * OpenGL ES 3.0 compatible 3D hardware-accelerated rendering interface for WebGL2.
 * Includes buffer objects, textures, shaders, programs, framebuffers, renderbuffers,
 * vertex array objects, samplers, queries, and sync primitives.
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
   * @type {number}
   */
  canvasWidth;

  /**
   * @readonly
   * @type {number}
   */
  canvasHeight;

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

}

