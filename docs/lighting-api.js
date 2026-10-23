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
 * The tone curve the exposed HDR frame goes through (scene.setToneMap).
 * Exposure is scene.setExposure's, not the curve's.
 * @typedef {Object} ToneMapConfig
 * @property {string} [mode]  "aces" (default), "reinhard" or "linear".
 * @property {number} [gamma]  Display gamma (default 2.2; `whitePoint` is an alias).
 */

/**
 * Exposure, in EV (scene.setExposure). Every call replaces the whole
 * exposure; a field left out is its default. The HDR frame is multiplied by
 *
 *     1 / (1.2 * 2^(EV - compensation))
 *
 * before the tone curve: EV is a camera's EV100, so scene units only need to
 * agree with each other. Two modes, one mechanism:
 *  - "fixed" (default): EV is the setting. Its default, -log2(1.2) (about
 *    -0.263), is a scale of exactly 1, for scenes lit in display units;
 *    compensation brightens (+1 is twice as bright) or darkens. Tests and
 *    stills want this: the frame does not depend on the frames before it.
 *  - "auto": EV is metered from the frame on the GPU (a 128-bin histogram of
 *    log2 luminance; L the log-average of the pixels between the lowPercent
 *    and highPercent shares, and EV the one that puts L at middle grey,
 *    log2(L / (1.2 * 0.18)), so a sunlit field lands where a camera would
 *    put it) and the applied EV
 *    follows it exponentially, at speedUp per second toward a brighter scene
 *    and speedDown toward a darker one: no overshoot, no oscillation. The
 *    tone pass reads the result on the GPU, so no frame waits on the meter;
 *    the first frame in auto takes the metered EV outright, and time is the
 *    scene's (headless advanceTime included). scene.cullStats() reports
 *    meteredEv, exposureEv and exposureScale a few frames late.
 *    `meter: 'incident'` meters the light instead of the frame: the sun
 *    through the air at the eye (the atmosphere's transmittance, the
 *    planet's shadow included), as a hand-held incident meter reads it,
 *    at the EV the reflected meter gives a grey card in that light,
 *    log2(E / (1.2 pi)). A dark scene stays dark and a bright one bright
 *    (a forest, a planet's ocean seen from orbit, a snowfield), where the
 *    reflected meter takes each to middle grey. It is kept within 1 stop
 *    brighter and 3 darker than the reflected reading, so shade and night
 *    are still seen; with the atmosphere off the reflected meter stands.
 *
 * Physical light values, for a scene lit in real units with auto exposure
 * (lux for directional lights, which are illuminance; candela-scale numbers
 * for point and spot lights, whose intensity is luminous intensity):
 *  - the sun above the air: 128,000 lux (the solar constant at ~94 lm/W);
 *    with the atmosphere on, give the sun light that and the air dims and
 *    reddens it (about 100,000 lux at the ground at noon, a few thousand at
 *    sunset). Without the atmosphere use the ground value directly.
 *  - an overcast day: 1,000 to 10,000 lux; an office: 300 to 500 lux; a
 *    living room: 50 to 150 lux; the full moon: 0.1 to 0.3 lux; a starlit
 *    night: 0.001 lux.
 *  - a 60 W incandescent bulb (800 lm): about 65 cd; a candle: 1 cd.
 *  - metered EVs to expect: a sunlit scene 13 to 15, an overcast one 11 to
 *    12, an interior 4 to 6, moonlight about -3. The meter is clamped to
 *    [minEv, maxEv].
 * @typedef {Object} ExposureConfig
 * @property {string} [mode]  "fixed" (default) or "auto".
 * @property {number} [ev]  Fixed: the exposure, EV100 (default -log2(1.2): a scale of 1).
 * @property {number} [compensation]  Stops added on top, both modes (default 0; +1 doubles the brightness).
 * @property {number} [speedUp]  Auto: adaptation rate per second toward a brighter scene (default 3).
 * @property {number} [speedDown]  Auto: adaptation rate per second toward a darker scene (default 1).
 * @property {number} [minEv]  Auto: the metered EV's lower clamp (default -6).
 * @property {number} [maxEv]  Auto: its upper clamp (default 20).
 * @property {number} [lowPercent]  Auto: the share of darkest pixels the meter ignores, 0..1 (default 0.1).
 * @property {number} [highPercent]  Auto: the share above which the brightest are ignored, 0..1 (default 0.9).
 * @property {string} [meter]  Auto: "reflected" (default, the frame's luminance) or "incident" (the sun's light at the eye).
 */

/**
 * @typedef {Object} AmbientConfig
 * @property {Array<number>} [color]
 * @property {number} [intensity]
 */

/**
 * The receivers' shadow filter (scene.setShadowQuality). Per-light shadow
 * settings (bias, normal bias, shadowDistance) live on the LightNode.
 *
 * Shadows are cached virtual pages (docs/scene-world-architecture.md §8):
 * the sun's is a clipmap fixed in light space, its finest level about a
 * texel a screen pixel near the eye, each level twice the extent of the one
 * before; spots and points have their own maps on the same page pool. A
 * page is drawn when a view first needs it and kept until its light moves
 * or a caster over it changes, so a still scene draws no shadow casters.
 * A sun that turns is followed in steps of about 0.1 degree: the next
 * clipmap is drawn behind the shown one over the following frames and
 * shown when it is complete, every shadow moving by the step at once.
 * What moves every frame (skinned or moving nodes, swaying plants and
 * grass) is drawn each frame over the cached depth. There is nothing to
 * size: resolution follows the screen. A custom vertex chunk that moves
 * geometry past a node's bounds needs its `cullMargin` set, or the pages
 * its shadow falls in are not found.
 * @typedef {Object} ShadowQualityConfig
 * @property {number} [pcfTaps] -  PCF filter grid side: 1 (single sample), 3 or 5 (default 3).
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
 * The atmosphere (scene.setAtmosphere): one model of the air from the ground
 * to orbit (Hillaire 2020), a shell of Rayleigh and Mie scattering and an
 * ozone layer over a spherical planet. Every call replaces the whole
 * atmosphere; omitted fields keep Earth's values; null or {enabled: false}
 * turns it off. While it is on it is the sky (the sun's disc and limb
 * included), the haze between the eye and every surface (aerial
 * perspective, from a camera-aligned 32^3 volume), the sun's light at every
 * surface (the sun light dimmed and reddened by the air it crosses, and
 * nothing once the sun is below a point's horizon), and the sky's light on
 * surfaces (directional: l <= 2 spherical harmonics of the sky and the lit
 * ground below, by sun angle and altitude), which replaces the flat ambient.
 * The eye can be anywhere: on the ground, in the air, above the shell.
 *
 * The sun is the brightest directional light: its direction and its colour
 * times intensity (the illuminance above the air, lux in physical units:
 * 128,000 for Earth's sun, ExposureConfig) are tracked unless pinned here.
 * Pair it with auto exposure (scene.setExposure({ mode: 'auto' })).
 *
 * The air's haze. Earth's default Mie (Hillaire's and Bruneton's reference
 * air) is near-pristine: an aerosol optical depth of 0.005 and over 200 km
 * of visibility, so ground tens of kilometres off is barely hazed. Most of a
 * real day's haze is the boundary layer's aerosol; `turbidity` scales it.
 * Visibility V (metres) wants Mie extinction 3.912 / V less the Rayleigh's
 * 13.6e-6 at sea level; the Mie extinction is 4.44e-6 per unit of turbidity:
 *   turbidity  1  pristine, 200+ km      turbidity 19  a fine day, 40 km (AOD 0.1)
 *   turbidity 10  clear, 65 km (AOD 0.05) turbidity 40  hazy, 20 km (AOD 0.2)
 * @typedef {Object} AtmosphereConfig
 * @property {boolean} [enabled]  false turns it off (default true).
 * @property {Array<number>} [rayleigh]  Rayleigh scattering per metre at the surface, [r, g, b] (default [5.802e-6, 13.558e-6, 33.1e-6]).
 * @property {number} [rayleighScaleHeight]  Metres (default 8000).
 * @property {number|Array<number>} [mie]  Mie scattering per metre (default 3.996e-6), or [that, g].
 * @property {number} [mieG]  Mie anisotropy in (-1, 1) (default 0.8).
 * @property {number} [mieAbsorption]  Per metre (default 0.444e-6).
 * @property {number} [mieScaleHeight]  Metres (default 1200).
 * @property {number} [turbidity]  Haze: multiplies the Mie coefficients (default 1).
 * @property {Array<number>} [ozone]  Ozone absorption per metre at its peak, [r, g, b] (default [0.650e-6, 1.881e-6, 0.085e-6]).
 * @property {number} [ozoneAltitude]  The ozone peak's altitude, metres (default 25000).
 * @property {number} [ozoneWidth]  Its half-width, metres (default 15000).
 * @property {Array<number>} [groundAlbedo]  The planet's surface, Lambertian [r, g, b] (default [0.3, 0.3, 0.3]): it lights the air and the lower half of every surface's sky light.
 * @property {number} [planetRadius]  Metres (default 6360000), on a flat body: on a round one (`scene.setBody`) the air wraps the body, its radius the body's.
 * @property {number} [thickness]  The air's depth above the surface, metres (default 100000).
 * @property {number} [seaLevel]  On a flat body, the world y that sits on the planet's surface, which is placed under the eye (default 0).
 * @property {Array<number>} [sunDirection]  Pins the direction towards the sun, [x, y, z] (`sunPosition` is an alias).
 * @property {Array<number>} [sunColor]  With sunIntensity, pins the sun's illuminance above the air (sunColor * sunIntensity).
 * @property {number} [sunIntensity]
 * @property {number} [sunAngularRadius]  Radians (default 0.004675, the real sun's).
 */

/**
 * @typedef {Object} StarfieldConfig
 * @property {number} [starCount]  Relative density: 1000 is the default field, about 0..2000.
 * @property {number} [starSize]  Brightness multiplier (default 1); star sprites have a fixed size.
 */

/**
 * The miniature look: outside a horizontal band of the frame a blurred copy
 * is mixed in (over a 0.1 feather). Any field turns it on.
 * @typedef {Object} TiltShiftConfig
 * @property {number} [blur]  Blur strength (default 1).
 * @property {number} [focus]  Band centre, 0 = bottom of the frame, 1 = top (default 0.5).
 * @property {number} [range]  Band half-height in the same units (default 0.2).
 */

/**
 * Glow around HDR highlights, added before tonemapping. Any field turns it on.
 * @typedef {Object} BloomConfig
 * @property {number} [threshold]  Luminance where the glow starts (default 1), with a soft knee one threshold wide: full strength at twice it.
 * @property {number} [intensity]  How much of the glow is added (default 1).
 * @property {number} [radius]  The blur's reach (default 1).
 */

/**
 * @typedef {Object} SSAOConfig
 * @property {number} [radius]
 * @property {number} [bias]
 * @property {number} [intensity]
 */

/**
 * @typedef {Object} SSRConfig
 * @property {boolean} [enabled]      false (or no options at all) turns SSR off.
 * @property {number} [maxDistance]   Longest reflected ray, world units (default 30).
 * @property {number} [steps]         Linear march steps over maxDistance, 4..256 (default 48; `stepCount` is accepted too).
 * @property {number} [thickness]     How far behind a surface a ray still counts as hitting it, view units (default 0.3).
 * @property {number} [intensity]     Reflection weight, times each surface's reflectance; 0 is SSR off (default 1).
 * @property {number} [edgeFade]      Fade toward the screen border, fraction of the viewport 0..0.5 (default 0.1).
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
   * No-op stub retained for compatibility; returns `this`.
   * @param {string} name
   * @param {Object} animSpec
   * @returns {SpriteNode}
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

  /** Alias for `probeCapture()`. */
  capture() {}

}


