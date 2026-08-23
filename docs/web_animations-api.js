/**
 * =============================================================================
 * element.animate(), Web Animations API (commonly-used subset)
 * =============================================================================
 *
 * Script-driven animations that ride the CSS transitions/keyframes interpolator.
 *
 * @example
 *   const anim = element.animate([
 *     { opacity: 0, transform: 'translateX(0px)' },
 *     { opacity: 1, transform: 'translateX(100px)' }
 *   ], {
 *     duration: 1000,
 *     easing: 'ease-out',
 *     fill: 'forwards'
 *   });
 *   await anim.finished;
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Timing and configuration options for element animations.
 * @typedef {Object} KeyframeAnimationOptions
 * @property {number} [duration] -  Duration of single iteration in milliseconds.
 * @property {number} [delay] -  Start delay in milliseconds.
 * @property {number} [endDelay] -  End delay in milliseconds.
 * @property {number} [iterations] -  Number of iterations (or Infinity).
 * @property {string} [direction] -  Playback direction ("normal", "reverse", "alternate", "alternate-reverse").
 * @property {string} [easing] -  Timing function easing name.
 * @property {string} [fill] -  Fill mode ("none", "forwards", "backwards", "both").
 * @property {string} [id] -  Animation identifier string.
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Web Animations API Animation controller instance.
 */
class Animation {

  /**
   *  Current playback time in milliseconds, or null if idle.
   * @type {number|null}
   */
  currentTime;

  /**
   *  Playback rate multiplier (default 1.0).
   * @type {number}
   */
  playbackRate;

  /**
   *  Current playback state ("idle", "running", "paused", "finished").
   * @readonly
   * @type {string}
   */
  playState;

  /**
   *  Whether the animation has pending async tasks (always false).
   * @readonly
   * @type {boolean}
   */
  pending;

  /**
   *  Optional identifier for the animation.
   * @type {string}
   */
  id;

  /**
   *  Promise that resolves when animation finishes or rejects if cancelled.
   * @readonly
   * @type {Promise<Animation>}
   */
  finished;

  /**
   *  Event handler called when animation finishes.
   * @type {EventHandler}
   */
  onfinish;

  /**
   *  Event handler called when animation is cancelled.
   * @type {EventHandler}
   */
  oncancel;

  /**
   *  Starts or resumes playback of the animation.
   */
  play() {}

  /**
   *  Pauses playback of the animation.
   */
  pause() {}

  /**
   *  Cancels the animation and clears its effects.
   */
  cancel() {}

  /**
   *  Fast-forwards animation to completion.
   */
  finish() {}

  /**
   *  Reverses playback direction of the animation.
   */
  reverse() {}

}

