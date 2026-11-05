/**
 * =============================================================================
 * bro.permissions — Every App's Grants, for the Desktop Shell
 * =============================================================================
 *
 * The grant store (docs/apps.md, Permissions) seen and changed from script:
 * every installed app and every app the store knows, what each declares, the
 * user's decisions, the files and folders picked for it, when each capability
 * was last used, and whether an install is still what its decisions were made
 * for. It is what a shell's Apps settings page is built on (helm's is
 * ui/js/settings/pages/permissions.js).
 *
 * Only the host executable grants it (the `permissions` capability is a shell
 * capability: never asked for, never granted by the store or dev mode); helm's
 * main grants it to helm's UI, and `bro-headless --grant permissions` to a
 * test. Without it `bro.permissions` is the unavailable stub:
 * `bro.permissions.available === false`, `.reason` says why, every call throws.
 *
 * Changes are written to the store at once (read, changed and renamed into
 * place, so a concurrent `bro --grant-app` is not lost) and reach running apps
 * while they run: each bro process reads the store again within a quarter
 * second of a change. A capability taken back is revoked in the running app
 * at once: its calls throw an Error named `PermissionRevoked`, and what it
 * holds open under it (a microphone capture, a socket or server, a child
 * process or terminal, a watcher) is closed with an error saying why. One
 * granted, or a picked path added, takes effect at the app's next use.
 *
 * @example
 *   for (const app of bro.permissions.list()) {
 *     console.log(app.name, app.identity, JSON.stringify(app.decisions));
 *   }
 *   bro.permissions.set('org.helm.Recorder', 'mic', 'allow');
 *   bro.permissions.on('change', () => refresh());
 *   bro.permissions.on('request', (req) => askTheUser(req).then((ok) => req.respond(ok)));
 *   bro.permissions.devMode = false;
 */

/**
 * @typedef {Object} PermissionCapability
 * @property {string} name     e.g. 'files', 'net', 'env', 'sys.processes', 'compositor'.
 * @property {'ask'|'shell'} kind  'ask': the user decides, per app; 'shell':
 *                             only the host executable grants it.
 * @property {string} summary  What it lets an app do, in a few words.
 */

/**
 * @typedef {Object} AppPermissionsEntry
 * @property {string} identity  What grants attach to: `install:<id>` for an
 *                              app in an install root, else `path:<dir>`.
 * @property {string} id        Its bro.json id ('' if unknown).
 * @property {string} name      Its bro.json name ('' if unknown).
 * @property {string} dir       Its folder ('' if no longer found).
 * @property {boolean} installed Whether it is in an install root.
 * @property {''|'current'|'changed'|'unrecorded'|'linked'} install
 *                              An install against its record (`bro --install`
 *                              hashes what it installs): 'current', the
 *                              decisions were made for what is installed;
 *                              'changed', it was installed again or altered
 *                              since, so its next launch forgets them and asks
 *                              again; 'unrecorded', no record (not installed
 *                              by `bro --install`); 'linked', a `--link`
 *                              install, whose grants hold across edits. ''
 *                              for an app run from a folder.
 * @property {string[]} declared The capabilities its bro.json declares.
 * @property {Object<string, 'allow'|'deny'|'undecided'|'host'>} decisions
 *                              Each declared capability ('undecided' asks at
 *                              its first use; 'host' for a shell capability,
 *                              which no decision grants), and any decision
 *                              kept for one it no longer declares.
 * @property {Object<string, number>} lastUsed  When each capability was last
 *                              used by the app, in ms since the epoch (a
 *                              running app writes within a minute, and at
 *                              once on a capability's first use in a run;
 *                              host grants are not recorded).
 * @property {string[]} picked  Files and folders the user picked for it.
 */

/** @type {boolean} false when not granted (the stub). */
bro.permissions.available;

/** Why it is unavailable, on the stub. @type {string} */
bro.permissions.reason;

/**
 * The capability catalogue, in its order.
 * @returns {PermissionCapability[]}
 */
bro.permissions.capabilities = function() {};

/**
 * Every installed app, then every other app the store holds decisions for.
 * @returns {AppPermissionsEntry[]}
 */
bro.permissions.list = function() {};

/**
 * Decide an ask capability for an app. Throws for a capability that is not
 * one, for a shell capability, and for an app it cannot find. Deciding for an
 * install whose content changed since its other decisions forgets those first
 * (they were for other content).
 * @param {string} app  An identity from list(), an installed app's id, or an
 *                      app folder.
 * @param {string} capability
 * @param {'allow'|'deny'|'undecided'} decision  'undecided' forgets the
 *                      decision, so the app asks again (a running app that
 *                      had it granted has it revoked until it asks).
 * @returns {boolean} true
 */
bro.permissions.set = function(app, capability, decision) {};

/**
 * Forget every decision and picked path of an app: it asks again for each.
 * @param {string} app
 * @returns {boolean} true
 */
bro.permissions.reset = function(app) {};

/**
 * Take back one file or folder picked for an app (as list() reports it). A
 * running app loses it at once, a watcher on it included.
 * @param {string} app
 * @param {string} path
 * @returns {boolean} true; throws when it was not picked for the app.
 */
bro.permissions.removePicked = function(app, path) {};

/**
 * Listen for changes to the store, from this process or any other (`bro
 * --grant-app`, another shell, an app's prompt being answered): the listener
 * gets `{ type: 'change' }`, and list() reads the new state.
 * @param {'change'} type
 * @param {function({type: string}): void} listener
 */
bro.permissions.on = function(type, listener) {};
bro.permissions.off = function(type, listener) {};

/**
 * @typedef {Object} PermissionRequest
 * An app asking the user for a capability (docs/apps.md, "The shell's
 * prompts"). The `app` fields are what the asking process says about itself:
 * the shell's answer only ever decides for that process's own identity, as
 * the process keeps it, so a liar can grant only itself.
 * @property {'request'} type
 * @property {number} id       Unique among this process's requests.
 * @property {{name: string, id: string, identity: string, dir: string, pid: number}} app
 *                             Its bro.json name and id, its identity
 *                             (`install:<id>` or `path:<dir>`), its folder, and
 *                             the process that asked (from the connection,
 *                             not its say-so).
 * @property {string} capability  An ask capability (any other is dropped unseen).
 * @property {string} summary  What it lets an app do, from this process's own
 *                             catalogue ("start other programs").
 * @property {string} what     What the app was trying to do ("start git",
 *                             "reading /home/me/notes.txt"); may be ''.
 * @property {''|'allowed'|'denied'|'dropped'|'cancelled'} settled  '' while outstanding.
 * @property {function(boolean|null): boolean} respond  The user's answer: true
 *                             allows, false denies; null drops it with no
 *                             answer (refused, nothing kept, asked again
 *                             next time). False when it was settled already.
 */

/**
 * Serve the session's permission prompts: the apps that would otherwise show
 * a native dialog ask here instead (docs/apps.md, "The shell's prompts"). The
 * first 'request' listener starts serving, removing the last stops it (each
 * outstanding request is then cancelled); a shell without a prompt UI never
 * subscribes, so the apps fall back to the dialog. Several requests, from
 * several apps, may be outstanding at once; each stays open until answered.
 * 'cancel' is the asker gone first (it quit, crashed, or this process stopped
 * serving): `{ type: 'cancel', id, request }`; the request is settled
 * 'cancelled' and respond() returns false. Only a process the host granted
 * `permissions` has bro.permissions, so only the shell can serve prompts.
 *
 * @example
 *   bro.permissions.on('request', (req) => {
 *     showPrompt(`${req.app.name} wants to ${req.summary}`, req.what)
 *       .then((allow) => req.respond(allow));
 *   });
 *   bro.permissions.on('cancel', (e) => dismissPrompt(e.id));
 *
 * @param {'request'|'cancel'} type
 * @param {function(PermissionRequest|{type: 'cancel', id: number, request: PermissionRequest}): void} listener
 */
bro.permissions.on = function(type, listener) {};

/**
 * Whether this process serves the prompts now: false before a 'request'
 * listener, and when another live shell of the session (or of the same
 * scratch profile) already serves them.
 * @type {boolean}
 */
bro.permissions.serving;

/**
 * Dev mode: every app is granted every ask capability it declares, over the
 * user's denials (never a shell capability), and each launch logs it loudly.
 * Read from and written to the store; turning it off revokes, in running
 * apps, what only dev mode granted. helm shows it in its bar while on.
 * @type {boolean}
 */
bro.permissions.devMode;
