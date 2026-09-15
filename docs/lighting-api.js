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
 * @typedef {Object} ShadowQualityConfig
 * @property {number} [resolution]
 * @property {number} [cascades]
 * @property {number} [maxDistance]
 * @property {number} [bias]
 * @property {number} [normalBias]
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
   * @type {boolean}
   */
  castShadow;

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

