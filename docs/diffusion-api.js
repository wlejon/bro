// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.diffusion — text-to-image neural diffusion inference pipeline
 * =============================================================================
 */
class Pipeline {

}

class PipelineState {

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @readonly
 * @type {string}
 */
bro.diffusion.version;

bro.diffusion.init = function() {};

/**
 * @param {Object} config
 * @returns {Pipeline}
 */
bro.diffusion.createPipeline = function(config) {};

/**
 * @param {string} dir
 * @param {Object} [opts]
 * @returns {Pipeline}
 */
bro.diffusion.loadModel = function(dir, opts) {};

/**
 * @param {Float32Array} src
 * @param {Object} opts
 * @returns {Float32Array}
 */
bro.diffusion.expandNoise = function(src, opts) {};

