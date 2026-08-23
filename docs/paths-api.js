/**
 * =============================================================================
 * bro.paths (bro.appDir / bro.resolvePath) — Application Filesystem Path Resolution
 * =============================================================================
 *
 * Resolves virtual asset mount paths and relative application asset paths into
 * absolute native filesystem paths suitable for external child processes and tools.
 *
 * @example
 *   // Resolve application asset path to absolute filesystem path
 *   const absPath = bro.resolvePath('bin/ffmpeg.exe');
 *   console.log('App dir:', bro.appDir, 'resolved path:', absPath);
 *
 * @example
 *   // Resolve mount path
 *   const configPath = bro.resolvePath('/app/preset.json');
 *   console.log('Real config path:', configPath);
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Engine asset and application filesystem path resolution namespace.
 */
/**
 * Absolute native filesystem path of the running application's root directory.
 * @readonly
 * @type {string}
 */
bro.paths.appDir;

/**
 * Resolves a virtual mount path or relative application path to an absolute native filesystem path.
 *
 * @param {string} path - Input path string
 * @returns {string} Resolved absolute filesystem path
 */
bro.paths.resolvePath = function(path) {};

