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

/**
 * @typedef {Object} AppOpenResult
 * @property {string} id       The id asked for.
 * @property {string} dir      The app's folder.
 * @property {string} from     Where it was found: "installed" (a `bro --install`
 *                             root, as `bro <id>` finds it), "project" (beside
 *                             this app in its project: BRO_PROJECT_ROOT, else the
 *                             nearest project above this app, else this app's
 *                             parent folder; by bro.json id or by folder name), or
 *                             "self".
 * @property {string[]} command The command line: the stock bro beside this
 *                             executable, `--new-instance` if asked, the folder,
 *                             then `args`.
 * @property {string[]} args   The app's own arguments (its bro.app.argv).
 * @property {string} cwd      The working directory it starts in (this app's).
 * @property {boolean} spawned Whether a process was started (false headless).
 */

/**
 * Open another folder app by its id, on every platform: found as `bro <id>`
 * finds it (the install roots of `bro --install`: `%LOCALAPPDATA%\bro\apps`
 * and `%ProgramFiles%\bro\apps` on Windows, `~/Library/Application Support/bro/apps`
 * on macOS, `$XDG_DATA_HOME/bro/apps` and `$XDG_DATA_DIRS/bro/apps` on Linux),
 * else beside this app in its project, and started detached with the stock
 * bro: `bro <dir> ...args`. A single-instance app that is already running
 * gets an `instance` event with these args instead of a second window (bro
 * does the hand-off, as for any launch). Rejects when no app has the id, or
 * the process could not be started.
 *
 * Headless starts nothing: it resolves with `spawned: false` and records the
 * call, which `openedApps()` lists (docs/headless.md).
 *
 *   await bro.app.open('org.helm.Music', ['--play', file]);
 *
 * @param {string} id
 * @param {string[]} [args]
 * @param {{newInstance?: boolean}} [options]
 * @returns {Promise<AppOpenResult>}
 */
bro.app.open = function(id, args, options) {};

/**
 * Where `open(id)` would find the app, without opening it: `{ id, dir, from }`,
 * or null when no app has that id.
 * @param {string} id
 * @returns {?{id: string, dir: string, from: string}}
 */
bro.app.find = function(id) {};

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

// ── Startup ──────────────────────────────────────────────────────────────────

/**
 * How long this launch took, in ms since the process started:
 * `{ loadedMs, firstFrameMs }`. `loadedMs` is when the page's scripts had
 * run; `firstFrameMs` when its first frame was drawn (0 until then). The log
 * says the same once: `app <id>: first frame N ms after start (page loaded
 * at M ms)`. bro-headless draws the first frame before its script runs, so a
 * test reads both:
 *
 *   const { loadedMs, firstFrameMs } = bro.app.startup;
 *   assert(firstFrameMs - loadedMs < 250, 'nothing between the load and the first frame');
 *
 * Most of a launch is the engine's (the window, the GPU device, compiling the
 * page); what an app adds shows against an empty page's numbers.
 * `graphics` times the first of those: `windowMs` (creating the window),
 * `gpuMs` (the Vulkan device, the presenter and Skia's context) and `totalMs`
 * (both; -1 for a phase that did not run). Headless brings the GPU up on a
 * thread while it creates its window, so `totalMs` is about the longer of the
 * two rather than their sum (the page's host globals, installed meanwhile,
 * are `page`'s, not part of it). `startAtMs` / `readyAtMs` are when the GPU
 * began and was up.
 *
 * While the GPU comes up (headless and windowed alike) bro installs the
 * realm's host globals and compiles the page's first script unit (its
 * classic scripts together, else its first module script) on a thread;
 * the script runs once the device is up. `page` times that: `globalsAtMs` /
 * `globalsMs` (the host globals), `compileStartMs` / `compileEndMs` /
 * `compileMs`, `waitMs` (how long the run waited for the compile to finish),
 * `codeCache` (`'hit'`, `'miss'`, `'off'`, ...) and `overlapMs` (how much of
 * that work ran before `readyAtMs`). Every field is -1 (and `codeCache` '')
 * when nothing compiled ahead: a page with no script, or
 * `BRO_STARTUP_OVERLAP=0`, which keeps the old order (the device, then the
 * page) for comparing the two.
 * @readonly @type {{loadedMs: number, firstFrameMs: number,
 *                   graphics: {windowMs: number, gpuMs: number, totalMs: number,
 *                              startAtMs: number, readyAtMs: number},
 *                   page: {globalsAtMs: number, globalsMs: number, compileStartMs: number,
 *                          compileEndMs: number, compileMs: number, waitMs: number,
 *                          codeCache: string, overlapMs: number}}}
 */
bro.app.startup;

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
 * granted (docs/apps.md, Permissions): everything asked, for an app installed in a
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
