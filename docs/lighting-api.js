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
 * Image-based lighting from an HDR panorama, which also draws as the sky.
 * setEnvironment(null) clears it.
 * @typedef {Object} EnvironmentConfig
 * @property {string} [panorama]  Path to an equirectangular .hdr (`hdr` is an alias); "" clears. On a load failure the previous environment stays.
 * @property {number} [intensity]  IBL strength multiplier.
 * @property {number} [rotation]  Y-axis rotation in radians, to line the panorama's sun up with a directional light.
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
 * Global shadow atlas quality (scene.setShadowQuality). Per-light shadow
 * settings (cascades, bias, normalBias, maxDistance) live on the LightNode.
 * @typedef {Object} ShadowQualityConfig
 * @property {number} [atlasSize] -  Side length in texels of the square shadow depth atlas (default 4096).
 * @property {number} [pcfTaps] -  PCF filter grid side: 1 (single sample), 3 or 5 (default 3).
 */

/**
 * The static shadow-tile cache (on by default): a shadow-atlas tile is
 * re-rendered only when its light moved or a caster over it changed. Pixels
 * are identical either way; turning it off is for debugging.
 * @typedef {Object} ShadowCacheConfig
 * @property {boolean} [enabled]
 */

/**
 * Every setFog call replaces the whole fog state; setFog({}) (or setFog(null)) turns it off.
 * With density > 0 the fog is exponential-squared, 1 - exp(-(density*d)^2)
 * for d the camera distance past `start`; otherwise a linear start..end ramp
 * when end > 0.
 * @typedef {Object} FogConfig
 * @property {string} [mode]  "linear" ignores density; "none" (or "off") turns fog off. Any other value, or none, picks by density as above.
 * @property {Array<number>} [color]  Linear RGB.
 * @property {number} [density]  Exponential mode density per world unit.
 * @property {number} [start]  Distance where fog begins (both modes). `startDistance` is an alias.
 * @property {number} [end]  Linear mode: distance of full fog.
 * @property {number} [heightFalloff]  Exponential mode: density thins by exp(-heightFalloff * (y - height)) with world height y.
 * @property {number} [height]  World y of the fog layer's base, where the density is `density` (default 0).
 */

/**
 * Physical sky scattering. Omitted fields keep the Earth defaults.
 * @typedef {Object} AtmosphereConfig
 * @property {Array<number>} [rayleigh]  Rayleigh scattering per metre, [r, g, b] (default [5.802e-6, 13.558e-6, 33.1e-6]).
 * @property {Array<number>} [mie]  [coefficient per metre (default 3.996e-6), anisotropy g in (-1, 1) (default 0.76)].
 * @property {number} [turbidity]  Haze: multiplies the Mie coefficient (default 1).
 * @property {Array<number>} [sunPosition]  Direction towards the sun, [x, y, z].
 * @property {number} [sunIntensity]  Pins the sun's radiance (20 * sunIntensity). Without it the sky follows the brightest directional light.
 */

/**
 * @typedef {Object} StarfieldConfig
 * @property {number} [starCount]  Relative density: 1000 is the default field, about 0..2000.
 * @property {number} [starSize]  Brightness multiplier (default 1); star sprites have a fixed size.
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
 */

/**
 * @typedef {Object} SSAOConfig
 * @property {number} [radius]
 * @property {number} [bias]
 * @property {number} [intensity]
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
 * @property {number} [focalLength]  The depth range around focusDistance that stays sharp (default 5).
 * @property {number} [maxBlur]
 */

/**
 *  Options of `SceneGraph.setColorLUT`.
 * @typedef {Object} ColorLUTConfig
 * @property {string} [path=""] -  Path of the LUT strip image (N*N by N pixels); empty clears the LUT.
 * @property {number} [size=0] -  Cube size N; 0 infers it from the strip's aspect ratio.
 * @property {number} [amount=1] -  Blend between the ungraded (0) and fully graded (1) image.
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

