// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.vision — Vision-model inference (brovisionml)
 * =============================================================================
 *
 * Image-understanding and dense prediction models: promptable segmentation (SAM),
 * monocular depth (Depth-Anything-V2), surface normals (DSINE), ControlNet annotators
 * (HED, Lineart, MLSD, OpenPose, SegFormer, BiRefNet, StyleGAN3, DINOv2, DINOv3).
 * @example
 * bro.vision.init();
 *   const depth = bro.vision.loadDepth('weights/depth-anything-v2-small');
 *   const result = depth.estimate(imageBitmap);
 */
class DepthEstimator {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Sam {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @readonly
   * @type {boolean}
   */
  hasImage;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {*}
   */
  setImage(image, opts) {}

  /**
   * @param {Object} [opts]
   * @returns {Object}
   */
  segment(opts) {}

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {*}
   */
  segmentEverything(image, opts) {}

}

class NormalEstimator {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Hed {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Lineart {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Mlsd {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Openpose {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Segformer {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Birefnet {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class StyleGAN3 {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @readonly
   * @type {number}
   */
  zDim;

  /**
   * @readonly
   * @type {number}
   */
  cDim;

  /**
   * @readonly
   * @type {number}
   */
  wDim;

  /**
   * @readonly
   * @type {number}
   */
  imgResolution;

  /**
   * @readonly
   * @type {number}
   */
  imgChannels;

  /**
   * @param {Float32Array} z
   * @param {Object} [opts]
   * @returns {Object}
   */
  generate(z, opts) {}

}

class Dinov2 {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

class Dinov3 {

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  estimate(image, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

bro.vision.init = function() {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadDepth = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadSam = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadNormal = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadHed = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadLineart = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadMlsd = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadOpenpose = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadSegformer = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadBirefnet = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadStyleGAN3 = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadDinov2 = function(modelDir, opts) {};

/**
 * @param {string} modelDir
 * @param {Object} [opts]
 * @returns {*}
 */
bro.vision.loadDinov3 = function(modelDir, opts) {};

