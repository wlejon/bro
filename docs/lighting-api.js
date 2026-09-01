// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Lighting, PBR Materials & Post-FX API Reference
 * =============================================================================
 *
 * Physical lights, PBR materials, post-processing stack, tonemapping, and environmental effects.
 * @typedef {Object} LightConfig
 * @property {string} [type]
 * @property {Array<number>} [color]
 * @property {number} [intensity]
 * @property {number} [range]
 * @property {number} [innerCone]
 * @property {number} [outerCone]
 * @property {boolean} [castShadow]
 */

/**
 * @typedef {Object} PBRMaterialConfig
 * @property {Array<number>} [albedo]
 * @property {string} [albedoTexture]
 * @property {number} [roughness]
 * @property {string} [roughnessTexture]
 * @property {number} [metallic]
 * @property {string} [metallicTexture]
 * @property {Array<number>} [emissive]
 * @property {string} [emissiveTexture]
 * @property {number} [emissiveIntensity]
 * @property {string} [normalTexture]
 * @property {number} [normalScale]
 * @property {string} [occlusionTexture]
 * @property {number} [occlusionStrength]
 * @property {string} [alphaMode]
 * @property {number} [alphaCutoff]
 * @property {boolean} [doubleSided]
 */

/**
 * @typedef {Object} EnvironmentConfig
 * @property {string} [panorama]
 * @property {string} [cubeMap]
 * @property {Array<number>} [color]
 * @property {number} [intensity]
 * @property {number} [blur]
 * @property {boolean} [background]
 */

/**
 * @typedef {Object} ToneMapConfig
 * @property {string} [mode]
 * @property {number} [exposure]
 * @property {number} [whitePoint]
 */

/**
 * @typedef {Object} AmbientConfig
 * @property {Array<number>} [color]
 * @property {number} [intensity]
 */

/**
 * Shadow atlas settings — `scene.setShadowQuality({atlasSize, pcfTaps})`,
 * or positionally `scene.setShadowQuality(4096, 3)`.
 *
 * All shadow-casting lights share one square depth atlas. Each frame it is
 * cut into a 1x1, 2x2 or 4x4 grid from how many tiles the lights ask for
 * (a directional light wants one tile per cascade, a spot one, a point six):
 * a single sun over an orthographic camera gets the WHOLE atlas, a 4-cascade
 * sun gets a quarter per cascade, and only a busier scene falls back to 16ths.
 *
 * Directional lights are fitted to what the camera can see: the visible
 * volume (a frustum slice per cascade, or the orthographic box) is clipped to
 * the bounds of every shadow-receiving mesh, bounded by a sphere, and snapped
 * to the map's texel grid in a fixed light basis — so a 1400 m far plane over
 * a 60 m-tall scene costs nothing, and panning the camera does not make the
 * edges shimmer. The depth range is then stretched to cover every caster, so
 * things above or behind the view still cast into it.
 *
 * @property {number} [atlasSize=4096] Side of the square depth atlas, in
 *   texels. 2048 halves the memory (16 MB vs 64 MB) at half the resolution.
 * @property {number} [pcfTaps=3] Side of the receiver's PCF grid of
 *   hardware-bilinear compares at one-texel spacing: 1 (a single tap, hard
 *   texel-sized edges), 3 (a 4-texel tent — the default), or 5 (a 6-texel
 *   tent, softer).
 */

/**
 * @typedef {Object} ShadowCacheConfig
 * @property {boolean} [enabled]
 * @property {number} [staticResolution]
 */

/**
 * @typedef {Object} FogConfig
 * @property {string} [mode]
 * @property {Array<number>} [color]
 * @property {number} [density]
 * @property {number} [start]
 * @property {number} [end]
 * @property {number} [heightFalloff]
 * @property {number} [height]
 */

/**
 * @typedef {Object} AtmosphereConfig
 * @property {Array<number>} [rayleigh]
 * @property {Array<number>} [mie]
 * @property {number} [turbidity]
 * @property {Array<number>} [sunPosition]
 * @property {number} [sunIntensity]
 */

/**
 * @typedef {Object} StarfieldConfig
 * @property {number} [starCount]
 * @property {number} [starSize]
 * @property {number} [twinkleSpeed]
 * @property {Array<number>} [tint]
 */

/**
 * @typedef {Object} TiltShiftConfig
 * @property {number} [blur]
 * @property {number} [focus]
 * @property {number} [range]
 */

/**
 * @typedef {Object} BloomConfig
 * @property {number} [threshold]
 * @property {number} [intensity]
 * @property {number} [radius]
 * @property {number} [iterations]
 */

/**
 * @typedef {Object} SSAOConfig
 * @property {number} [radius]
 * @property {number} [bias]
 * @property {number} [intensity]
 * @property {number} [sampleCount]
 */

/**
 * @typedef {Object} SSRConfig
 * @property {number} [maxDistance]
 * @property {number} [thickness]
 * @property {number} [stepCount]
 * @property {number} [roughnessCutoff]
 */

/**
 * @typedef {Object} DepthOfFieldConfig
 * @property {number} [focusDistance]
 * @property {number} [focalLength]
 * @property {number} [fStop]
 * @property {number} [maxBlur]
 */

/**
 * @typedef {Object} ColorLUTConfig
 * @property {string} [texture]
 * @property {number} [intensity]
 */

/**
 * @typedef {Object} FXAAConfig
 * @property {boolean} [enabled]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class LightNode {

  /**
   * @type {Array<number>}
   */
  color;

  /**
   * @type {number}
   */
  intensity;

  /**
   * @type {number}
   */
  range;

  /**
   * @type {number}
   */
  innerCone;

  /**
   * @type {number}
   */
  outerCone;

  /**
   * Whether this light renders a shadow map. Default false. Meshes cast by
   * default (`mesh.castsShadow`) and receive by default (`mesh.receivesShadow`).
   * @type {boolean}
   */
  castsShadow;

  /**
   * Extra constant depth bias, in the light's [0,1] clip depth, added on top
   * of the renderer's automatic bias. Default 0. The automatic bias is sized
   * from the shadow texel the fit produced — a fraction of a texel of slope
   * plus a texel-scaled normal offset — so it holds at any scene scale; set
   * this only for a light that still shows acne. Negative values allowed.
   * @type {number}
   */
  shadowBias;

  /**
   * World-space floor on how far a receiving fragment is pushed along its
   * normal before the shadow lookup, in addition to the automatic
   * texel-proportional offset. Default 0.03. Raise it for thin two-sided
   * geometry (leaves, cloth) that shows acne.
   * @type {number}
   */
  shadowNormalBias;

  /**
   * Cascades for a directional light under a perspective camera, 1–4
   * (default 4); one atlas tile each. Ignored under an orthographic camera,
   * which has one on-screen scale and so gets one map.
   * @type {number}
   */
  cascadeCount;

  /**
   * Practical-split blend for the cascade boundaries: 0 = uniform spacing
   * (indoor), 1 = logarithmic (long outdoor views). Default 0.5.
   * @type {number}
   */
  cascadeSplitLambda;

}

class ShapeNode {

  /**
   * @type {string}
   */
  shapeType;

  /**
   * @type {Array<number>}
   */
  color;

  /**
   * @type {Array<number>}
   */
  size;

}

class SpriteNode {

  /**
   * @type {string}
   */
  texture;

  /**
   * @type {Array<number>}
   */
  size;

  /**
   * @param {string} name
   * @param {Object} animSpec
   */
  addAnimation(name, animSpec) {}

  /**
   * @param {string} name
   */
  play(name) {}

  stop() {}

}

class HtmlNode {

  /**
   * @param {string} html
   */
  setHtml(html) {}

  markHtmlDirty() {}

}

class ParticleNode {

  /**
   * @param {number} count
   */
  burst(count) {}

  clear() {}

}

class Particles3DNode {

  /**
   * @param {number} count
   */
  burst(count) {}

  clear() {}

}

class GaussianSplatNode {

  /**
   * @param {string} path
   */
  savePly(path) {}

}

class DecalNode {

  /**
   * @type {string}
   */
  texture;

  /**
   * @type {Array<number>}
   */
  size;

}

class ReflectionProbeNode {

  probeCapture() {}

}

