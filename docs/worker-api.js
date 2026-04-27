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
   * @param {*} value - any cloneable value
   * @param {ArrayBuffer[]} [transferList] - ArrayBuffers to transfer (zero-copy).
   *   Transferred buffers are detached from the sender and become unusable.
   *
   * @example
   *   worker.postMessage('hello');
   *   worker.postMessage({ cmd: 'process', items: [1, 2, 3] });
   *
   *   // Transfer an ArrayBuffer (zero-copy, detaches from sender)
   *   const buf = new Float32Array([1, 2, 3]).buffer;
   *   worker.postMessage({ data: buf }, [buf]);
   *   // buf.byteLength === 0 after transfer
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
   * @param {ArrayBuffer[]} [transferList] - ArrayBuffers to transfer (zero-copy)
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
  //
  // NOT available: window, document, DOM, canvas, scene, Worker (no nesting),
  //                bro.physics, bro.audio
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
//   Binary data:
//     ArrayBuffer (copied by default, or transferred via transferList)
//     Int8Array, Uint8Array, Uint8ClampedArray
//     Int16Array, Uint16Array
//     Int32Array, Uint32Array
//     Float32Array, Float64Array
//
//   NOT cloneable (throws TypeError):
//     Functions, Symbols, DOM nodes, classes with methods,
//     Map, Set, Date, RegExp, Error, Promise, WeakRef
//
// Nesting depth limit: 64 levels.
// Transfer list must contain only ArrayBuffers.


// -----------------------------------------------------------------------------
// Limitations
// -----------------------------------------------------------------------------
//
// - No SharedWorker or ServiceWorker
// - No importScripts() — worker script is a single file
// - No nested Workers (cannot create a Worker inside a worker)
// - No SharedArrayBuffer or Atomics
// - Message queue capacity: 256 messages per direction.
//   postMessage() throws if the queue is full.
// - Worker thread polls at ~1ms intervals when idle
