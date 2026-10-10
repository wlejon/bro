// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * Worker — Web Workers Dedicated Background Execution
 * =============================================================================
 *
 * Dedicated background thread running an isolated JavaScript runtime
 * communicating via structured clone postMessage/onmessage.
 *
 * The script is compiled as written: a worker with `import` / `export` is a
 * module, and top-level `await` works in a module or a classic script (its
 * messages wait until the top level finishes, as in a module worker).
 *
 * Event loop: each message, and each due timer, is a task followed by a
 * microtask checkpoint (a promise a timer resolves continues before the next
 * timer runs). `setTimeout(fn, 0)` runs on the worker's next turn — well under
 * a millisecond (50 chained zero timers take ~0-1 ms), so yielding to the loop
 * with `await new Promise(r => setTimeout(r, 0))` is cheap — and an idle
 * worker sleeps until its next timer or message. Timers run on real time.
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

