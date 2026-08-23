// ── Typedefs ─────────────────────────────────────────────────────────────────

/**
 * @type {(string|ListenSourceOptions)}
 */
// typedef (string|ListenSourceOptions) ListenSource;

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} ListenSourceOptions
 * @property {boolean} [mic]
 * @property {boolean} [system]
 * @property {number} [process]
 * @property {number} [pid]
 * @property {boolean} [exclude]
 * @property {number} [channel]
 */

/**
 * @typedef {Object} ListenRetentionInfo
 * @property {boolean} [active]
 * @property {number} [seconds]
 * @property {number} [rate]
 * @property {number} [hop]
 * @property {number} [frameRate]
 * @property {number} [streamFrame]
 * @property {number} [heldFrames]
 * @property {number} [heldSeconds]
 */

/**
 * @typedef {Object} AudioApp
 * @property {number} [pid]
 * @property {string} [name]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class ListenStream {

  /**
   * @readonly
   * @type {number}
   */
  id;

  /**
   * @readonly
   * @type {string}
   */
  kind;

  /**
   * @readonly
   * @type {boolean}
   */
  valid;

  /**
   * @readonly
   * @type {WakeStreamView}
   */
  wake;

  /**
   * @readonly
   * @type {KwsStreamView}
   */
  kws;

  /**
   * @readonly
   * @type {SenseStreamView}
   */
  sense;

  /**
   * @readonly
   * @type {GestureStreamView}
   */
  gesture;

  /**
   * @param {number} [seconds=0]
   */
  retain(seconds) {}

  /**
   * @param {number} startFrame
   * @param {number} endFrame
   * @returns {Float32Array|null}
   */
  audio(startFrame, endFrame) {}

  /**
   * @returns {number}
   */
  frame() {}

  /**
   * @returns {ListenRetentionInfo}
   */
  info() {}

  /**
   * @param {Float32Array} samples
   */
  feed(samples) {}

  close() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {ListenSource} [source]
 * @returns {ListenStream}
 */
bro.listen.open = function(source) {};

/**
 * @returns {boolean}
 */
bro.listen.supported = function() {};

/**
 * @returns {Array<AudioApp>}
 */
bro.listen.apps = function() {};

/**
 * @param {number} [seconds=0]
 */
bro.listen.retain = function(seconds) {};

/**
 * @param {number} startFrame
 * @param {number} endFrame
 * @returns {Float32Array|null}
 */
bro.listen.audio = function(startFrame, endFrame) {};

/**
 * @returns {number}
 */
bro.listen.frame = function() {};

/**
 * @returns {ListenRetentionInfo}
 */
bro.listen.info = function() {};

