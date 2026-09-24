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
   * @param {string|URL} scriptURL  a path relative to the app directory
   *   ('sim/worker.js'), or a URL: the standard module form
   *   `new Worker(new URL('./worker.js', import.meta.url), { type: 'module' })`
   *   (a file: URL, since import.meta.url is the module's file URL) or a
   *   `bro://app/...` URL. Other schemes throw a TypeError.
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

