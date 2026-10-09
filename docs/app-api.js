/**
 * =============================================================================
 * bro.app — The Running App: Identity, Launch, Directories, Single Instance
 * =============================================================================
 *
 * What the runtime knows about the app it is running, from its bro.json
 * (docs/apps.md) and its command line. A folder with a bro.json and a
 * page is a desktop application: `bro <dir> [args...]` runs it, and bro.app is
 * everything such an app would otherwise need native code for — its argv, where
 * its settings and data go, its log file, whether a second launch should hand
 * over to the first, and which privileged namespaces it was granted.
 *
 * (`bro.apps`, docs/apps-api.js, is the other direction: the catalog of every
 * application installed on the machine.)
 *
 * Same in every host: the windowed `bro`, `bro-headless` (argv is what follows
 * `--`), and an app.dll.
 *
 * @example
 *   // A single-instance app: open what a later launch asked for.
 *   console.log(bro.app.id, bro.app.version, bro.app.argv);
 *   const settings = bro.app.configDir + '/settings.json';
 *   bro.app.addEventListener('instance', (e) => {
 *     openFiles(e.argv, e.cwd);            // the second launch's argv and cwd
 *   });
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * A later launch of a single-instance app, handed to the running one.
 * @typedef {Object} AppInstanceEvent
 * @property {'instance'} type
 * @property {Object} target  bro.app
 * @property {string[]} argv  That launch's arguments after the app directory.
 * @property {string} cwd     The directory it was launched from (resolve
 *                            relative paths in argv against this, not
 *                            process.cwd()).
 */

/**
 * @typedef {Object} AppFileType
 * @property {string} name
 * @property {string[]} mimeTypes
 * @property {string[]} extensions  Without the dot.
 */

/**
 * @typedef {Object} AppAction
 * @property {string} id
 * @property {string} name
 * @property {string[]} args  Passed after the app directory when the action runs.
 * @property {string} icon
 */

// ── Identity ─────────────────────────────────────────────────────────────────

/**
 * The app's id: bro.json's `id` (reverse-DNS such as `org.example.Notes`, or a
 * slug), else the app folder's name reduced to `[A-Za-z0-9._-]`. It is the
 * window's Wayland app_id / X11 WM class / Windows AppUserModelID, the name of
 * its desktop entry (`<id>.desktop`), and the key of its directories.
 * @readonly @type {string}
 */
bro.app.id;

/** bro.json `name`, else the id. @readonly @type {string} */
bro.app.name;

/** bro.json `version` ("" when it names none). @readonly @type {string} */
bro.app.version;

/** bro.json `description`. @readonly @type {string} */
bro.app.description;

/** Absolute path of bro.json's `icon`, "" when it names none or the file is missing. @readonly @type {string} */
bro.app.icon;

/** The app directory, absolute (the same as bro.appDir). @readonly @type {string} */
bro.app.dir;

/** The executable running the app (bro, bro-headless, or a host linking bro_engine). @readonly @type {string} */
bro.app.exePath;

/**
 * The declared desktop keys of bro.json, as parsed.
 * @readonly
 * @type {{singleInstance: boolean, display: boolean, categories: string[], keywords: string[],
 *         fileTypes: AppFileType[], actions: AppAction[]}}
 */
bro.app.manifest;

// ── Launch ───────────────────────────────────────────────────────────────────

/**
 * The command line after the app directory, verbatim. bro takes out only its
 * own flags (--no-gpu, --no-splash, --splash, --drm, --new-instance) and only
 * before a `--`; the `--` itself is passed through. Under bro-headless it is
 * what follows `--`.
 * @readonly @type {string[]}
 * @example
 *   // bro ~/apps/editor --line 12 notes.txt
 *   bro.app.argv;   // ['--line', '12', 'notes.txt']
 */
bro.app.argv;

/** The working directory the app was launched from. @readonly @type {string} */
bro.app.cwd;

/**
 * Start another process of this app, detached: `bro [--new-instance] <dir>
 * ...args` from this app's working directory. With `newInstance`, the new
 * process skips the single-instance hand-off and opens a window of its own
 * (what a "New Window" command wants). Returns false headless, or when the
 * process could not be started.
 * @param {string[]} [args]
 * @param {{newInstance?: boolean}} [options]
 * @returns {boolean}
 */
bro.app.spawn = function(args, options) {};

// ── Directories ──────────────────────────────────────────────────────────────

/**
 * Settings the user edits. `$XDG_CONFIG_HOME/<id>` (Linux),
 * `%APPDATA%\<id>` (Windows), `~/Library/Application Support/<id>` (macOS).
 * Created on first read. With BRO_APP_HOME set, `$BRO_APP_HOME/config`.
 * @readonly @type {string}
 */
bro.app.configDir;

/**
 * State the app owns. `$XDG_DATA_HOME/<id>`, `%APPDATA%\<id>\data`,
 * `~/Library/Application Support/<id>/data`. Created on first read.
 * @readonly @type {string}
 */
bro.app.dataDir;

/**
 * Data the app can rebuild. `$XDG_CACHE_HOME/<id>`,
 * `%LOCALAPPDATA%\<id>\cache`, `~/Library/Caches/<id>`. Created on first read.
 * @readonly @type {string}
 */
bro.app.cacheDir;

/**
 * Where this process's stdout and stderr (console.*, the engine log) go. An
 * app with an `id` in its bro.json logs to `<state>/<id>.log`
 * (`$XDG_STATE_HOME/<id>/<id>.log`, `%LOCALAPPDATA%\<id>\<id>.log`; a second
 * running instance writes `<id>-<pid>.log` beside it); an anonymous app writes
 * bro.log in its working directory. "" under bro-headless, which logs to its
 * own stdout.
 * @readonly @type {string}
 */
bro.app.logFile;

// ── Single instance ──────────────────────────────────────────────────────────

/**
 * True when this process holds the app's single-instance channel: bro.json
 * says `"singleInstance": true` and this was the first launch (bro-headless
 * claims it only with --single-instance). A launch made while it runs hands
 * its argv and cwd over, as an `instance` event here, and exits before it
 * opens a window; bro also raises this app's window.
 * @readonly @type {boolean}
 */
bro.app.singleInstance;

/**
 * Listen for launches handed over by later instances. Launches that arrive
 * before any listener are held and delivered to the first one, so a page that
 * subscribes after its startup work still sees them.
 * @param {'instance'} type
 * @param {function(AppInstanceEvent): void} listener
 */
bro.app.addEventListener = function(type, listener) {};

/** @param {'instance'} type @param {function(AppInstanceEvent): void} listener */
bro.app.removeEventListener = function(type, listener) {};

/** One handler, alongside the listeners. @type {?function(AppInstanceEvent): void} */
bro.app.oninstance;

// ── Permissions ──────────────────────────────────────────────────────────────

/**
 * The privileged namespaces bro.json asked for (`permissions`) and what was
 * granted (docs/desktop-trust.md): everything asked, for an app installed in a
 * trusted location; otherwise what the user's permissions file grants this id.
 * A namespace not granted is the unavailable stub: `bro.<ns>.available ===
 * false`, `bro.<ns>.reason` says why, and every call throws.
 * @readonly
 * @type {{requested: string[], granted: string[], shell: boolean}}
 * @example
 *   if (!bro.app.permissions.granted.includes('remote')) {
 *     console.log(bro.remote.reason);
 *   }
 */
bro.app.permissions;
