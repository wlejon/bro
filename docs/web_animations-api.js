// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * Web Animations API
 * =============================================================================
 *
 * Implements the W3C Web Animations API for DOM elements.
 * Provides element.animate(), element.getAnimations(), and Animation object controls.
 * @example
 * const anim = element.animate([
 *     { transform: 'translateY(0px)', opacity: 1 },
 *     { transform: 'translateY(100px)', opacity: 0 }
 *   ], { duration: 1000, iterations: Infinity });
 *   anim.pause();
 */
class Animation {

  /**
   * @type {number}
   */
  currentTime;

  /**
   * @type {number}
   */
  playbackRate;

  /**
   * @readonly
   * @type {string}
   */
  playState;

  /**
   * @readonly
   * @type {boolean}
   */
  pending;

  /**
   * @readonly
   * @type {Promise<Animation>}
   */
  finished;

  /**
   * @readonly
   * @type {Promise<Animation>}
   */
  ready;

  /**
   * @type {EventHandler}
   */
  onfinish;

  /**
   * @type {EventHandler}
   */
  oncancel;

  play() {}

  pause() {}

  finish() {}

  cancel() {}

  reverse() {}

}

class WebAnimations {

}

