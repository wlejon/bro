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
 * Absolute native path of the directory this app's persistent data belongs in —
 * saves, caches, per-user state. `<user data>/bro/apps/<app folder name>`, where
 * `<user data>` is `%APPDATA%` on Windows, `~/Library/Application Support` on
 * macOS and `$XDG_DATA_HOME` (or `~/.local/share`) elsewhere. Created on first
 * read, so an app can write into it directly. Empty when there is no app
 * directory (a bare `bro-headless -e` session).
 *
 * `appDir` is where the app is installed; it may be read-only, shared between
 * users, or a checked-out repository. Write here instead.
 * @readonly
 * @type {string}
 */
bro.paths.userDataDir;

/**
 * Resolves a virtual mount path or relative application path to an absolute native filesystem path.
 *
 * @param {string} path - Input path string
 * @returns {string} Resolved absolute filesystem path
 */
bro.paths.resolvePath = function(path) {};

