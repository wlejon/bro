/**
 * =============================================================================
 * bro.permissions — Every App's Grants, for the Desktop Shell
 * =============================================================================
 *
 * The grant store (docs/apps.md, Permissions) seen and changed from script:
 * every installed app and every app the store knows, what each declares, and
 * the user's decisions. It is what a shell's Apps settings page is built on.
 *
 * Only the host executable grants it (the `permissions` capability is a shell
 * capability: never asked for, never granted by the store or dev mode); helm's
 * main grants it to helm's UI, and `bro-headless --grant permissions` to a
 * test. Without it `bro.permissions` is the unavailable stub:
 * `bro.permissions.available === false`, `.reason` says why, every call throws.
 *
 * Changes are written to the store at once (read, changed and renamed into
 * place, so a concurrent `bro --grant-app` is not lost) and take effect at
 * the app's next launch; a running app keeps what it was started with.
 *
 * @example
 *   for (const app of bro.permissions.list()) {
 *     console.log(app.name, app.identity, JSON.stringify(app.decisions));
 *   }
 *   bro.permissions.set('org.helm.Recorder', 'mic', 'allow');
 *   bro.permissions.devMode = false;
 */

/**
 * @typedef {Object} PermissionCapability
 * @property {string} name     e.g. 'files', 'net', 'sys.processes', 'compositor'.
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
 * @property {string[]} declared The capabilities its bro.json declares.
 * @property {Object<string, 'allow'|'deny'|'undecided'|'host'>} decisions
 *                              Each declared capability ('host' for a shell
 *                              capability, which no decision grants), and any
 *                              decision kept for one it no longer declares.
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
 * one, for a shell capability, and for an app it cannot find.
 * @param {string} app  An identity from list(), an installed app's id, or an
 *                      app folder.
 * @param {string} capability
 * @param {'allow'|'deny'|'undecided'} decision  'undecided' forgets the
 *                      decision, so the app asks again.
 * @returns {boolean} true
 */
bro.permissions.set = function(app, capability, decision) {};

/**
 * Dev mode: every app is granted every ask capability it declares, over the
 * user's denials (never a shell capability), and each launch logs it loudly.
 * Read from and written to the store.
 * @type {boolean}
 */
bro.permissions.devMode;
