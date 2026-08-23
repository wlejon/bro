// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * Worker — Web Workers Dedicated Background Execution
 * =============================================================================
 *
 * Dedicated background thread running an isolated JavaScript runtime
 * communicating via structured clone postMessage/onmessage.
 * @example
 * const worker = new Worker('worker.js');
 *   worker.onmessage = (e) => console.log('From worker:', e.data);
 *   worker.postMessage({ task: 'compute', count: 1000 });
 */
class Worker {

  /**
   * @param {string} scriptURL
   * @param {Object} [options]
   */
  constructor(scriptURL, options) {}

  /**
   * @type {EventHandler}
   */
  onmessage;

  /**
   * @type {EventHandler}
   */
  onerror;

  /**
   * @type {EventHandler}
   */
  onmessageerror;

  /**
   * @param {*} message
   * @param {Array<Object>} [transfer]
   */
  postMessage(message, transfer) {}

  terminate() {}

}

