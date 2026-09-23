/**
 * =============================================================================
 * bro.appDir / bro.userDataDir / bro.resolvePath — Filesystem Path Resolution
 * =============================================================================
 *
 * Resolves virtual asset mount paths and relative application asset paths into
 * absolute native filesystem paths suitable for external child processes and tools.
 * These are members of `bro` itself; there is no `bro.paths` namespace.
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

/**
 * Absolute native filesystem path of the running application's root directory.
 * @readonly
 * @type {string}
 */
bro.appDir;

/**
 * Absolute native filesystem path for user data / save directory.
 * @readonly
 * @type {string}
 */
bro.userDataDir;

/**
 * Resolves a virtual mount path or relative application path to an absolute native filesystem path.
 *
 * @param {string} path - Input path string
 * @returns {string} Resolved absolute filesystem path
 */
bro.resolvePath = function(path) {};

/**
 * Resolves a path for writing.
 *
 * @param {string} path - Input path string
 * @returns {string} Resolved absolute filesystem path for write target
 */
bro.resolveWritePath = function(path) {};
