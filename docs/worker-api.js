// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * Worker — Web Workers Dedicated Background Execution
 * =============================================================================
 *
 * Dedicated background thread running an isolated JavaScript runtime
 * communicating via structured clone postMessage/onmessage.
 *
 * A worker made with `{ type: 'module' }` runs its script as a module: its
 * top-level declarations are its own. Without it the script is a classic
 * script, as on the web: its top-level `var` and function declarations are
 * properties of `self`. `import` / `export` work in either, and top-level
 * `await` works in either (its messages wait until the top level finishes,
 * as in a module worker).
 *
 * `importScripts(...urls)` in a classic worker runs each script in the
 * worker's global scope, in order, before it returns (each a classic
 * script, its top-level `var`s properties of `self`): a path (relative to the
 * worker script's folder), a file: or bro://app/ URL, or a blob: / data: URL.
 * A script that cannot be read throws a NetworkError DOMException; a script's
 * own throw propagates.
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
   * @param {string|URL} url  a path relative to the app directory
   *   ('sim/worker.js'), or a URL: the standard module form
   *   `new Worker(new URL('./worker.js', import.meta.url), { type: 'module' })`
   *   (a file: URL, since import.meta.url is the module's file URL), a
   *   `bro://app/...` URL, or a `blob:` URL (`URL.createObjectURL(new
   *   Blob([source]))`, classic or `{ type: 'module' }`) or `data:` URL
   *   carrying the script itself, whose imports resolve from the page's
   *   folder. A revoked object URL and other schemes throw a TypeError.
   * @param {Object} [options]
   */
  constructor(url, options) {}

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

