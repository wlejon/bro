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
   * Structured clone. Cost is roughly proportional to the bytes sent:
   * objects sharing a shape send their keys once per message (a Map of 10k
   * small records clones in ~2 ms and rebuilds in ~4 ms). ArrayBuffer bytes
   * of 4 KB or more are copied ONCE, out of band, and the receiver adopts
   * that block without a second copy; a buffer in `transfer` is copied once
   * the same way and detached on the sender. Transferring an ImageBitmap
   * moves its pixels; a Mesh must be transferred (it moves by pointer).
   * @param {*} message
   * @param {Array<Object>} [transfer]  ArrayBuffers, ImageBitmaps, Meshes
   */
  postMessage(message, transfer) {}

  terminate() {}

}

