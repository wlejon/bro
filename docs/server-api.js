/**
 * =============================================================================
 * bro.server — Dedicated Server & Worker Host Runtime Control
 * =============================================================================
 *
 * Dedicated server lifecycle control: fixed tick rate configuration, uptime
 * queries, and graceful shutdown requests.
 *
 * `bro-server [--grant <caps>] [--aot <dir>] <app> [script.js]` runs the
 * script (default: the app's server.js) as the app's one module script; the
 * page is not run. A server asks capabilities as any app does and cannot
 * prompt: `--grant net` (or `declared`) for one run, or `bro --grant-app`.
 * `--aot <dir>` runs the script from a module built ahead of time on its own
 * first run (docs/apps.md "Ahead-of-time scripts"): with none yet the run is
 * harvested, and `bro-server <app> <script> --aot-build <dir>` is started in
 * the background as it ends; a module out of date compiles in process and is
 * harvested again.
 *
 * @example
 *   bro.server.tickrate = 120.0;
 *   console.log('Server tickrate:', bro.server.tickrate, 'Uptime:', bro.server.uptime);
 *   // Gracefully request shutdown
 *   bro.server.stop();
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Dedicated server and worker host runtime control namespace.
 */
/**
 * Server tick rate in Hertz (ticks per second, range [1, 1000]).
 * @type {number}
 */
bro.server.tickrate;

/**
 * Server uptime in seconds since launch.
 * @readonly
 * @type {number}
 */
bro.server.uptime;

/**
 * Requests graceful termination of the dedicated server or worker loop.
 */
bro.server.stop = function() {};

