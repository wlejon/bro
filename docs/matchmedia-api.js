/**
 * =============================================================================
 * window.matchMedia(), MediaQueryList (CSSOM View subset)
 * =============================================================================
 *
 * Programmatic media-query evaluation that reuses the exact same evaluator
 * and MediaContext that filter the document's @media blocks (htmlayout), so
 * matchMedia and CSS can never disagree. Every realm, the app document, every
 * <iframe> sub-document, every secondary window opened with bro.window.open,
 * and every system panel: has its own matchMedia evaluating against ITS
 * document's context.
 *
 * @example
 *   const mql = window.matchMedia('(max-width: 600px)');
 *   console.log('Matches:', mql.matches, 'Media:', mql.media);
 *
 * @example
 *   const dark = matchMedia('(prefers-color-scheme: dark)');
 *   dark.addEventListener('change', (ev) => {
 *     console.log('dark mode:', ev.matches, 'query:', ev.media);
 *   });
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Event object dispatched to MediaQueryList change event listeners.
 * @typedef {Object} MediaQueryListEvent
 * @property {string} [type] -  Event type string ("change").
 * @property {boolean} [matches] -  Whether the media query matches current document media context.
 * @property {string} [media] -  The serialized media query string.
 * @property {MediaQueryList} [target] -  Target MediaQueryList object.
 * @property {MediaQueryList} [currentTarget] -  Current target MediaQueryList object.
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Stores information on a media query applied to a document, with support for real-time listener updates.
 */
class MediaQueryList {

  /**
   * Evaluates if the current document media context matches the media query.
   * @readonly
   * @type {boolean}
   */
  matches;

  /**
   * The serialized media query string.
   * @readonly
   * @type {string}
   */
  media;

  /**
   * Event handler called when the matching status changes.
   * @type {EventHandler}
   */
  onchange;

  /**
   * Adds an event listener callback for media query changes.
   *
   * @param {string} type - Event type string ("change")
   * @param {EventListener} listener - Callback function
   * @param {*} [options] - Optional options object or capture boolean
   */
  addEventListener(type, listener, options) {}

  /**
   * Removes a previously registered media query change event listener.
   *
   * @param {string} type - Event type string ("change")
   * @param {EventListener} listener - Callback function to remove
   * @param {*} [options] - Optional options object or capture boolean
   */
  removeEventListener(type, listener, options) {}

  /**
   * Legacy alias for adding a change listener.
   *
   * @param {EventListener} listener - Callback function
   */
  addListener(listener) {}

  /**
   * Legacy alias for removing a change listener.
   *
   * @param {EventListener} listener - Callback function to remove
   */
  removeListener(listener) {}

}

