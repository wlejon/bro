/**
 * =============================================================================
 * bro.profiler — script-driven sampling profiler
 * =============================================================================
 *
 * bronze's sampling profiler (the one BRONZE_SAMPLE=1 arms for a whole run,
 * reporting at exit) started and stopped from script, over a window of the
 * app's own choosing, with the result handed back as data.
 *
 * A sampler thread wakes `hz` times a second, suspends each selected thread,
 * walks its native stack (module unwind data, and brass's own copy of the JIT
 * unwind tables) together with the fast interpreter's frames on it, resumes
 * it, and names every frame while its code is still installed. stop() joins
 * the sampler and aggregates.
 *
 * Every function row carries the TIER the samples found it running in; one
 * JS function that ran interpreted and then tiered up shows as two rows:
 *
 *   'interpreter'  run by brass's fast interpreter (Tier 0)
 *   'tier 1'       brass baseline JIT code
 *   'tier 2'       brass optimizing JIT code ('tier 2 osr': entered mid-loop)
 *   'aot'          compiled ahead of time (an app folder's app.dll, bro's own JS)
 *   'stub'         JIT trampolines and lazy-link stubs
 *   'native'       everything else: bronze runtime helpers, bro, the OS
 *
 * SELF goes to the innermost frame that is not the interpreter's own dispatch
 * loop: a function the interpreter runs is billed for its bytecode, a runtime
 * helper it calls (a property store, an allocation) is billed to the helper.
 * TOTAL counts each sample once for every distinct row on its stack.
 *
 * Cost: while stopped, nothing — no sampler thread exists and nothing on any
 * call path checks for one. While running, each sample suspends a thread for
 * the length of one stack walk (a few microseconds). stop() symbolizes native
 * frames through the PDBs beside the binaries, which is slow the first time
 * (up to a second or two) and fast after.
 *
 * Threads: 'main' samples the page's thread, 'workers' every Worker thread,
 * 'js' both, 'all' those plus every other thread of the process (compile
 * workers, audio, the raster thread) reported as kind 'other'. A Worker
 * started while a profile runs is sampled from its start.
 *
 * Main realm only: a Worker has no bro.profiler (profile it from the page
 * with threads: 'workers'). One profile at a time per process.
 *
 * Windows x64 only for now; elsewhere start() throws.
 *
 * @example
 *   bro.profiler.start({ hz: 2000 });
 *   rebuildWorld();
 *   const p = bro.profiler.stop({ callers: true, report: true });
 *   console.log(p.report);
 *   for (const f of p.functions.slice(0, 10)) console.log(f.self, f.tier, f.name);
 *
 * @example
 *   // Who calls the hot helper?
 *   const p = bro.profiler.profile(() => step(600), { callers: true });
 *   const hot = p.functions.findIndex((f) => f.name === 'bronze_prop_set');
 *   const who = p.callers.filter((e) => e.callee === hot)
 *     .map((e) => `${p.functions[e.caller].name} x${e.count}`);
 *
 * @example
 *   // A worker's startup, sampled from the page
 *   bro.profiler.start({ threads: 'workers' });
 *   const w = new Worker('gen.js');
 *   w.onmessage = () => { const p = bro.profiler.stop(); ... };
 */

/**
 * @typedef {Object} ProfilerStartOptions
 * @property {number} [hz=1000]  Samples per second per thread, clamped to [50, 4000].
 * @property {'main'|'workers'|'js'|'all'} [threads='main']  Which threads to sample.
 */

/**
 * @typedef {Object} ProfilerStopOptions
 * @property {boolean} [callers=false]  Include caller -> callee edges (`callers`).
 * @property {boolean} [report=false]   Include a text table (`report`), the shape BRONZE_SAMPLE prints.
 * @property {number} [top=40]          Rows per text table.
 */

/**
 * @typedef {Object} ProfileFunction
 * @property {string} name    JS function name (its IL name: `Class.method`, `main.seg3`), or the native symbol.
 * @property {string} tier    'interpreter' | 'tier 1' | 'tier 2' | 'tier 2 osr' | 'aot' | 'stub' | 'native'.
 * @property {string} module  The image a native or aot frame is in; 'jit' or 'interpreter' otherwise.
 * @property {string} [file]  A JS function's source file, when known.
 * @property {number} [line]  Its definition line, when known.
 * @property {number} self    Samples with this function innermost.
 * @property {number} total   Samples with this function anywhere on the stack.
 */

/**
 * @typedef {Object} ProfileEdge
 * @property {number} caller  Index into `functions`.
 * @property {number} callee  Index into `functions`.
 * @property {number} count   Samples in which `caller` called `callee` directly
 *                            (interpreter dispatch frames skipped).
 */

/**
 * @typedef {Object} ProfileThread
 * @property {number} id       OS thread id.
 * @property {string} kind     'main' | 'worker' | 'other'.
 * @property {string} name     'main', a Worker's script path, or '' for 'other'.
 * @property {number} samples  Samples taken on it.
 */

/**
 * @typedef {Object} ProfileResult
 * @property {number} hz
 * @property {number} durationMs      Wall time from start() to stop().
 * @property {number} samples         Samples with at least one frame, all threads.
 * @property {boolean} truncated      The 256 MB sample log filled; sampling stopped early.
 * @property {ProfileThread[]} threads
 * @property {ProfileFunction[]} functions  By self, then total, descending.
 * @property {ProfileEdge[]} [callers]      With `callers: true`, by count descending.
 * @property {string} [report]              With `report: true`.
 */

const profiler = {
  /**
   * Starts a profile. Throws if one is running, if an option is invalid, or on
   * a platform without the sampler.
   * @param {ProfilerStartOptions} [opts]
   */
  start(opts) {},

  /**
   * Stops the running profile and returns it. Throws if none is running.
   * @param {ProfilerStopOptions} [opts]
   * @returns {ProfileResult}
   */
  stop(opts) {},

  /**
   * Starts, runs `body`, stops. The options are both start's and stop's. If
   * `body` throws, the profile is stopped and discarded and the error rethrown.
   * @param {() => void} body
   * @param {ProfilerStartOptions & ProfilerStopOptions} [opts]
   * @returns {ProfileResult}
   */
  profile(body, opts) {},

  /** Whether a profile is running. @type {boolean} */
  running: false,
};
