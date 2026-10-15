// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Animation & Tweening API Reference
 * =============================================================================
 *
 * Data-driven animation clips, blend spaces, state machines, and property tweening.
 * Also mounted at `bro.animation.{Tween, AnimationPlayer}`.
 * @typedef {Object} AnimationKeyDef
 * @property {number} [time]
 * @property {*} [value]
 * @property {string} [easing]
 * @property {string} [ease]
 * @property {string} [interp]
 * @property {string} [event]
 */

/**
 * @typedef {Object} AnimationTrackDef
 * @property {'property'|'event'} [type]
 * @property {string} [target]
 * @property {string} [property]
 * @property {Array<AnimationKeyDef>} [keys]
 * @property {string} [interpolation]
 */

/**
 * @typedef {Object} AnimationClipDef
 * @property {string} [name]
 * @property {number} [duration]
 * @property {boolean|'none'|'loop'|'pingpong'} [loop]
 * @property {Array<AnimationTrackDef>} [tracks]
 */

/**
 * @typedef {Object} TweenPropertyTargets
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [quaternion]
 * @property {Array<number>} [scale]
 * @property {number} [opacity]
 * @property {Array<number>} [color]
 */

/**
 * @typedef {Object} TweenOptions
 * @property {string} [easing]
 * @property {number} [delay]
 * @property {Function} [onUpdate]
 */

/**
 * @typedef {Object} BlendSpace1DClip
 * @property {string} [clip]
 * @property {number} [pos]
 */

/**
 * @typedef {Object} BlendSpace2DClip
 * @property {string} [clip]
 * @property {Array<number>} [pos]
 */

/**
 * @typedef {Object} AnimStateMachineTransition
 * @property {string} [to]
 * @property {string} [trigger]
 * @property {number} [blendTime]
 */

/**
 * @typedef {Object} AnimStateMachineState
 * @property {string} [clip]
 * @property {boolean} [loop]
 * @property {Array<AnimStateMachineTransition>} [transitions]
 */

/**
 * @typedef {Object} AnimStateMachineDef
 * @property {string} [initialState]
 * @property {Object<string, AnimStateMachineState>} [states]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class Tween {

  /**
   * @readonly
   * @type {boolean}
   */
  isRunning;

  /**
   * @readonly
   * @type {boolean}
   */
  isPaused;

  /**
   * @readonly
   * @type {boolean}
   */
  isFinished;

  /**
   * @type {Function|null}
   */
  onFinished;

  /**
   * @param {Object} target
   * @param {TweenPropertyTargets} props
   * @param {number} duration
   * @param {TweenOptions} [opts]
   * @returns {Tween}
   */
  to(target, props, duration, opts) {}

  /**
   * @returns {Tween}
   */
  parallel() {}

  /**
   * @param {Function} callback
   * @returns {Tween}
   */
  call(callback) {}

  /**
   * @param {number} [count]
   * @returns {Tween}
   */
  loop(count) {}

  /**
   * @returns {Tween}
   */
  start() {}

  /**
   * @returns {Tween}
   */
  stop() {}

  /**
   * @returns {Tween}
   */
  pause() {}

  /**
   * @returns {Tween}
   */
  resume() {}

  destroy() {}

}

class AnimationPlayer {

  /**
   * @readonly
   * @type {boolean}
   */
  playing;

  /**
   * @readonly
   * @type {string}
   */
  currentClip;

  /**
   * @readonly
   * @type {number}
   */
  currentTime;

  /**
   * @type {number}
   */
  speed;

  /**
   * @type {Function|null}
   */
  onFinished;

  /**
   * @type {Function|null}
   */
  onEvent;

  /**
   * @param {string} name
   * @param {*} clip
   */
  addClip(name, clip) {}

  /**
   * @param {string} name
   * @returns {{duration: number, loop: string, trackCount: number}|null}
   */
  clipDef(name) {}

  /**
   * @param {string} clipName
   * @param {Object} [opts]
   */
  play(clipName, opts) {}

  pause() {}

  resume() {}

  stop() {}

  /**
   * @param {number} time
   */
  seek(time) {}

  destroy() {}

}


