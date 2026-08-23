/**
 * =============================================================================
 * AbortController and AbortSignal — Asynchronous Task Cancellation
 * =============================================================================
 *
 * Implements the W3C DOM AbortController and AbortSignal APIs for cancelling
 * asynchronous operations such as fetch requests, timers, and streaming tasks.
 *
 * @example
 *   // Basic AbortController usage with fetch/async operation
 *   const controller = new AbortController();
 *   const signal = controller.signal;
 *
 *   signal.addEventListener("abort", () => {
 *     console.log("Operation aborted with reason:", signal.reason);
 *   });
 *
 *   // Trigger abort
 *   controller.abort("User cancelled operation");
 *   console.log("Is aborted:", signal.aborted);
 *
 * @example
 *   // AbortSignal.timeout usage
 *   const timeoutSignal = AbortSignal.timeout(5000);
 *   timeoutSignal.addEventListener("abort", () => {
 *     console.log("Timed out!");
 *   });
 *
 * @example
 *   // AbortSignal.any composite signal
 *   const c1 = new AbortController();
 *   const c2 = new AbortController();
 *   const anySignal = AbortSignal.any([c1.signal, c2.signal]);
 *   anySignal.addEventListener("abort", () => {
 *     console.log("At least one controller aborted");
 *   });
 *   c1.abort("First abort");
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * A signal object that allows you to communicate with an asynchronous operation
 * and abort it if desired via an AbortController.
 */
class AbortSignal {

  /**
   * Returns an AbortSignal instance that is already set to aborted with the given reason.
   *
   * @param {*} [reason] - The abort reason
   * @returns {AbortSignal}
   */
  static abort(reason) {}

  /**
   * Returns an AbortSignal instance that will automatically abort after a specified duration in milliseconds.
   *
   * @param {number} milliseconds - Duration in milliseconds
   * @returns {AbortSignal}
   */
  static timeout(milliseconds) {}

  /**
   * Returns an AbortSignal that aborts as soon as any of the given signals abort.
   *
   * @param {Array<AbortSignal>} signals - Sequence of AbortSignal instances
   * @returns {AbortSignal}
   */
  static any(signals) {}

  /**
   * Returns true if this signal has been aborted, false otherwise.
   * @readonly
   * @type {boolean}
   */
  aborted;

  /**
   * Returns the reason why this signal was aborted, or undefined.
   * @readonly
   * @type {*}
   */
  reason;

  /**
   * Event handler callback for the "abort" event.
   * @type {Function|null}
   */
  onabort;

  /**
   * Appends an event listener for "abort" events.
   *
   * @param {string} type - Event type string ("abort")
   * @param {Function} listener - Callback function
   */
  addEventListener(type, listener) {}

  /**
   * Removes an event listener for "abort" events.
   *
   * @param {string} type - Event type string ("abort")
   * @param {Function} listener - Callback function to remove
   */
  removeEventListener(type, listener) {}

  /**
   * Dispatches an event to this signal instance.
   *
   * @param {Object} event - Event object to dispatch
   * @returns {boolean}
   */
  dispatchEvent(event) {}

  /**
   * Throws the signal's reason if the signal is aborted.
   */
  throwIfAborted() {}

}

/**
 * Controller object that allows aborting DOM and async requests when desired.
 */
class AbortController {

  /**
   * Initializes a new AbortController instance.
   */
  constructor() {}

  /**
   * Returns the AbortSignal object associated with this controller.
   * @readonly
   * @type {AbortSignal}
   */
  signal;

  /**
   * Aborts the asynchronous operation, setting the associated signal to aborted state.
   *
   * @param {*} [reason] - Optional abort reason
   */
  abort(reason) {}

}

