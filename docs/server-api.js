/**
 * =============================================================================
 * bro.server — Dedicated Server & Worker Host Runtime Control
 * =============================================================================
 *
 * Dedicated server lifecycle control: fixed tick rate configuration, uptime
 * queries, and graceful shutdown requests.
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

