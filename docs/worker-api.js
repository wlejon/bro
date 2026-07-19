// =============================================================================
// bro Worker API Reference
// =============================================================================
//
// Workers run JavaScript in a dedicated background thread with their own
// JSRuntime and event loop. They communicate with the main thread via
// structured-clone message passing (postMessage / onmessage).
//
// Workers have access to all brokit APIs (console, timers, fetch, crypto,
// encoding, URL, streams, fs, noise, etc.) but NOT the DOM, canvas, or
// window.
//
// Basic usage (main thread):
//
//   const worker = new Worker('my-worker.js');
//   worker.onmessage = (e) => console.log('got:', e.data);
//   worker.postMessage({ cmd: 'start', n: 1000 });
//
// Worker script (my-worker.js):
//
//   self.onmessage = (e) => {
//     const result = heavyComputation(e.data.n);
//     self.postMessage({ result });
//   };
//
// =============================================================================


// -----------------------------------------------------------------------------
// Worker (main thread)
// -----------------------------------------------------------------------------
// Created on the main thread. Each Worker spawns a dedicated OS thread.

class Worker {

  // --- Constructor ----------------------------------------------------------

  /**
   * Create and start a new Worker.
   * @param {string} scriptPath - path to worker script (relative to app directory or absolute)
   */
  constructor(scriptPath) {}


  // --- Methods --------------------------------------------------------------

  /**
   * Send a message to the worker thread.
   * The value is serialized using structured clone (see Cloneable Types below).
   *
   * Throws TypeError "Worker is not running" if the worker has been
   * terminate()d or closed itself.
   *
   * @param {*} value - any cloneable value
   * @param {(ArrayBuffer|Mesh|ImageBitmap)[]} [transferList] - objects to
   *   transfer. Anything else in the list throws TypeError. ArrayBuffer
   *   transfer is NOT zero-copy today: the bytes are copied out and the
   *   source is then detached, so the sender loses the buffer either way.
   *   Mesh and ImageBitmap do transfer by pointer (true zero-copy).
   *
   * @example
   *   worker.postMessage('hello');
   *   worker.postMessage({ cmd: 'process', items: [1, 2, 3] });
   *
   *   // Transfer an ArrayBuffer (copies bytes, then detaches the source)
   *   const buf = new Float32Array([1, 2, 3]).buffer;
   *   worker.postMessage({ data: buf }, [buf]);
   *   // buf.byteLength === 0 after transfer
   *
   *   // A Mesh in the payload MUST be listed — otherwise postMessage throws
   *   // ("Mesh must be listed in the transferList").
   *   worker.postMessage({ geometry: mesh }, [mesh]);
   */
  postMessage(value, transferList) {}

  /**
   * Terminate the worker thread. The worker stops immediately and cannot
   * be restarted. Pending messages are discarded.
   */
  terminate() {}


  // --- Events ---------------------------------------------------------------

  /**
   * Called when the worker sends a message via self.postMessage().
   * The message is wrapped in an event object: { data: <value> }.
   *
   * @type {Function|null}
   * @example
   *   worker.onmessage = (e) => {
   *     console.log(e.data);  // the deserialized value
   *   };
   */
  onmessage;
}


// -----------------------------------------------------------------------------
// Worker Global Scope (inside the worker script)
// -----------------------------------------------------------------------------
// These globals are available inside the worker script. There is no `window`
// or `document` — use `self` to refer to the worker global scope.

/** @type {WorkerGlobalScope} */
var self;

class WorkerGlobalScope {

  // --- Messaging ------------------------------------------------------------

  /**
   * Send a message to the main thread.
   *
   * @param {*} value - any cloneable value
   * @param {(ArrayBuffer|Mesh|ImageBitmap)[]} [transferList] - objects to
   *   transfer. ArrayBuffer transfer copies the bytes and detaches the
   *   source; Mesh / ImageBitmap move by pointer.
   *
   * @example
   *   self.postMessage({ type: 'result', value: 42 });
   *
   *   // Transfer a buffer back to main thread
   *   const buf = new ArrayBuffer(1024);
   *   self.postMessage(buf, [buf]);
   */
  postMessage(value, transferList) {}

  /**
   * Terminate the worker from within. Equivalent to the main thread
   * calling worker.terminate().
   */
  close() {}

  /**
   * Called when the main thread sends a message via worker.postMessage().
   * The message is wrapped in an event object: { data: <value> }.
   *
   * @type {Function|null}
   * @example
   *   self.onmessage = (e) => {
   *     const cmd = e.data;
   *     // ... process command ...
   *     self.postMessage({ done: true });
   *   };
   */
  onmessage;


  // --- Available APIs -------------------------------------------------------
  //
  // Workers have their own instances of all brokit APIs:
  //
  //   console.log(), console.error(), ...
  //   setTimeout(), setInterval(), clearTimeout(), clearInterval()
  //   fetch(), Request, Response, Headers
  //   URL, URLSearchParams
  //   TextEncoder, TextDecoder
  //   atob(), btoa()
  //   crypto.getRandomValues(), crypto.randomUUID()
  //   structuredClone()
  //   queueMicrotask()
  //   ReadableStream, WritableStream, TransformStream
  //   fs.readFile(), fs.writeFile(), ...
  //   noise (Perlin/simplex)
  //
  // Engine APIs available in workers:
  //   Mesh (bromesh geometry)
  //   bro.net.*     — own subscriber against the shared NetService: host(),
  //                   connect(), send(), broadcast(), onconnect, ondisconnect,
  //                   onmessage. Safe to host a server entirely inside a
  //                   worker.
  //   bro.server.*  — worker-scoped: tickrate (this worker's event-loop
  //                   rate), uptime (seconds since this worker started),
  //                   stop() (terminate this worker).
  //   bro.ai.game.* — navmesh, pathfinding, LOS, steering — same API as
  //                   the main thread. All state lives on JS objects, so
  //                   worker and main contexts have independent worlds.
  //   bro.tensor.*  — GPU tensor + ops (brotensor).
  //   bro.diffusion.* — diffusion-model inference (brodiffusion).
  //   bro.lm.*      — Qwen3 text generation (brolm). Each worker owns its own
  //                   model + KV cache.
  //   bro.stt.* / bro.tts.* — Whisper STT / Kokoro TTS (brosoundml).
  //                   Run heavy inference here to keep the main thread
  //                   responsive; transfer Float32 audio buffers back.
  //   ImageBitmap / createImageBitmap — build frames here, transfer to main.
  //
  // NOT available: window, document, DOM, canvas, scene, Worker (no nesting),
  //                bro.physics, bro.audio (playback is main-thread only)
}


// -----------------------------------------------------------------------------
// MessageChannel
// -----------------------------------------------------------------------------
// Same-thread bidirectional message channel with two connected ports.
// Provided by brokit (available in both main thread and workers).

class MessageChannel {

  /**
   * Create a new channel with two connected ports.
   * Messages sent on port1 arrive on port2 and vice versa.
   *
   * @example
   *   const ch = new MessageChannel();
   *   ch.port1.onmessage = (e) => console.log('port1 got:', e.data);
   *   ch.port1.start();
   *   ch.port2.postMessage('hello');
   */
  constructor() {}

  /** @type {MessagePort} First port of the channel. */
  port1;

  /** @type {MessagePort} Second port of the channel. */
  port2;
}


// -----------------------------------------------------------------------------
// MessagePort
// -----------------------------------------------------------------------------
// One end of a MessageChannel. Extends EventTarget.

class MessagePort extends EventTarget {

  /**
   * Send a message to the connected port.
   * Data is cloned via structuredClone.
   * @param {*} data
   */
  postMessage(data) {}

  /**
   * Start receiving messages. Messages sent before start() are queued
   * and delivered once start() is called.
   */
  start() {}

  /**
   * Close this port. Disconnects from the other port and discards
   * any queued messages.
   */
  close() {}

  /**
   * Called when the connected port sends a message.
   * @type {Function|null}
   */
  onmessage;

  /**
   * Called when deserialization fails.
   * @type {Function|null}
   */
  onmessageerror;
}


// -----------------------------------------------------------------------------
// Cloneable Types (structured clone)
// -----------------------------------------------------------------------------
// postMessage serializes values using a binary structured clone format.
// The following types can be sent between main thread and workers:
//
//   Primitives:
//     undefined, null, boolean, number (int32 / float64), string, bigint
//
//   Objects:
//     Plain objects (own enumerable string properties, recursive)
//     Arrays (recursive)
//
//   Platform objects:
//     Date     (time value; an invalid Date stays invalid)
//     RegExp   (source + flags; lastIndex resets, per spec)
//     Map, Set (recursive, insertion order preserved)
//     Error    (name + message + stack; a TypeError arrives as a TypeError.
//               Own properties hung on the error are dropped, per spec)
//
//   Binary data:
//     ArrayBuffer (copied by default; listing it in transferList still copies
//                  the bytes, then detaches the source)
//     DataView (byte offset and length preserved)
//     Int8Array, Uint8Array, Uint8ClampedArray
//     Int16Array, Uint16Array
//     Int32Array, Uint32Array
//     Float16Array, Float32Array, Float64Array
//     BigInt64Array, BigUint64Array
//
//   NOT cloneable (throws TypeError):
//     Functions, Promises, and the weak collections (WeakMap, WeakSet,
//     WeakRef). Any of these nested anywhere in the payload throws the same
//     way — the send fails whole rather than delivering a partial payload.
//
//   Class instances are NOT reconstructed: an instance of an app-defined
//   class arrives as a plain object carrying its own enumerable string
//   properties, with the prototype gone. This matches structured clone.
//
// Nesting depth limit: 64 levels.
// Transfer list may contain ArrayBuffer, Mesh, or ImageBitmap; anything else
// throws TypeError. A Mesh in the payload MUST be listed or postMessage
// throws — Mesh has no clone path.


// -----------------------------------------------------------------------------
// Limitations
// -----------------------------------------------------------------------------
//
// - No SharedWorker or ServiceWorker
// - No importScripts() — worker script is a single file
// - No nested Workers (cannot create a Worker inside a worker)
// - No SharedArrayBuffer or Atomics
// - Message queue capacity: 255 messages per direction (a 256-slot ring with
//   one slot always left empty to distinguish full from empty).
//   postMessage() throws if the queue is full.
// - postMessage() on a worker that has been terminate()d, or that called
//   self.close(), throws TypeError "Worker is not running".
// - Worker thread polls at ~1ms intervals when idle
