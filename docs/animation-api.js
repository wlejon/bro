// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Animation & Tweening API Reference
 * =============================================================================
 *
 * Data-driven animation clips, blend spaces, state machines, and property tweening.
 * @typedef {Object} AnimationKeyDef
 * @property {number} [time]
 * @property {*} [value]
 * @property {string} [easing]
 */

/**
 * @typedef {Object} AnimationTrackDef
 * @property {string} [target]
 * @property {string} [property]
 * @property {Array<AnimationKeyDef>} [keys]
 * @property {string} [interpolation]
 */

/**
 * @typedef {Object} AnimationClipDef
 * @property {string} [name]
 * @property {number} [duration]
 * @property {boolean} [loop]
 * @property {Array<AnimationTrackDef>} [tracks]
 */

/**
 * @typedef {Object} TweenPropertyTargets
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {Array<number>} [color]
 * @property {number} [opacity]
 * @property {number} [intensity]
 * @property {number} [fov]
 * @property {number} [custom]
 */

/**
 * @typedef {Object} TweenOptions
 * @property {number} [duration]
 * @property {string} [easing]
 * @property {number} [loop]
 * @property {boolean} [yoyo]
 * @property {number} [delay]
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
   * @param {Object} target
   * @param {TweenPropertyTargets} props
   * @param {number} duration
   * @param {string} [easing]
   * @returns {Tween}
   */
  to(target, props, duration, easing) {}

  /**
   * @param {Array<Tween>} tweens
   * @returns {Tween}
   */
  parallel(tweens) {}

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
   * @param {string} name
   * @param {*} clip
   */
  addClip(name, clip) {}

  /**
   * @param {string} name
   * @param {AnimationClipDef} def
   */
  clipDef(name, def) {}

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

