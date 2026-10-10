/**
 * =============================================================================
 * bro.conf — Layered Settings Store with Schemas and Change Watchers
 * =============================================================================
 *
 * `bro.conf` provides access to a typed, schema-validated, layered configuration
 * store (backed by native broconf). It supports user overrides, system-wide
 * defaults, schema validation, type conversions, and asynchronous change notifications
 * across processes and threads.
 *
 * Availability: Gated by `BRO_WITH_CONF`. Check `bro.conf.available`.
 *
 * @example
 *   if (bro.conf.available) {
 *     // Register a schema with defaults and validation
 *     bro.conf.registerSchema({
 *       id: 'org.bro.desktop.interface',
 *       path: 'desktop.interface',
 *       keys: {
 *         'dark-mode': { type: 'bool', default: false },
 *         'font-size': { type: 'int', default: 11, min: 8, max: 32 },
 *         'theme': { type: 'string', default: 'BroLight' },
 *         'accent-color': { type: 'color', default: { r: 53, g: 132, b: 228, a: 255 } }
 *       }
 *     });
 *
 *     // Read configuration values
 *     const darkMode = bro.conf.get('desktop.interface.dark-mode');
 *     const theme = bro.conf.get('desktop.interface', 'theme');
 *
 *     // Set configuration value (returns Promise)
 *     await bro.conf.set('desktop.interface.theme', 'BroDark');
 *
 *     // Watch for changes
 *     const handle = bro.conf.watch('desktop.interface', (key, newVal, oldVal) => {
 *       console.log(`Setting ${key} changed from ${oldVal} to ${newVal}`);
 *     });
 *     // Unwatch when done
 *     handle.unwatch();
 *   }
 */

/**
 * Whether the broconf settings subsystem was compiled into this binary.
 * @type {boolean}
 */
bro.conf.available;

/**
 * Gets a configuration value. Supports either "section.key" (split at the last dot)
 * or separate section and key arguments.
 *
 * Throws an Error if the key is not found and no schema default exists.
 *
 * @param {string} keyOrPath - Dotted key path ("section.key") or section path
 * @param {string} [optKey] - Key name if first parameter is section path
 * @returns {boolean|number|string|string[]|Object} Converted value
 */
bro.conf.get = function(keyOrPath, optKey) {};

/**
 * Gets an optional configuration value, returning undefined if the key does not exist.
 *
 * @param {string} keyOrPath - Dotted key path ("section.key") or section path
 * @param {string} [optKey] - Key name if first parameter is section path
 * @returns {boolean|number|string|string[]|Object|undefined} Value or undefined
 */
bro.conf.getOptional = function(keyOrPath, optKey) {};

/**
 * Sets a configuration value for the user layer. Validates against any registered
 * schema for the key and updates the store in memory at once: a `get` right after
 * reads the new value, and this process's watchers hear it on the next frame pump.
 *
 * Writing is cheap and asynchronous: `set` does not touch the disk. The settings
 * file is written about 50 ms after the first unwritten change, on a writer
 * thread, as ONE write for everything changed in that window (a burst of sets is
 * one write), atomically (a temp file renamed over the old one, so a crash never
 * leaves a torn file). The write merges the changed keys onto the file as it is
 * on disk, so keys another process wrote meanwhile are kept. Other processes
 * watching the file hear the change once it is written. Pending changes are
 * written by `bro.conf.flush()`, at engine/realm teardown, and when the store is
 * destroyed at exit, so nothing set is lost on a normal exit. A large store makes
 * `set` no slower: the cost is the value set, not the file.
 *
 * Returns a Promise that resolves once the value is in memory (not on disk: call
 * `flush()` for that), or rejects on validation error (e.g. out of range, type
 * mismatch, invalid enum).
 *
 * @param {string} keyOrPath - Dotted key path ("section.key") or section path
 * @param {string|*} [optKeyOrValue] - Key name if 3 arguments, or value if 2 arguments
 * @param {*} [optValue] - Value if 3 arguments
 * @returns {Promise<void>} Resolves on success, rejects on validation error
 *
 * @example
 *   // Cheap enough per frame or per keystroke; the file is written once.
 *   for (const p of paths) list.push(p);
 *   bro.conf.set('music.library.paths', list);
 *   bro.conf.set('music.ui.volume', 0.8);
 */
bro.conf.set = function(keyOrPath, optKeyOrValue, optValue) {};

/**
 * Writes every change made by `set`/`reset` so far to the settings file now, and
 * returns once it is there. Blocking (it waits for the write, a few ms for a large
 * store), so call it where the file must be current: before handing off to
 * another process that will read it, before a deliberate hard exit, or in a test
 * that reads the file. Not needed for normal exits: teardown flushes.
 *
 * A failed write keeps the changes pending; they are retried by the next change
 * or flush.
 *
 * @returns {boolean} true when everything is on disk; false if the write failed
 *
 * @example
 *   bro.conf.set('app.session.lastFile', path);
 *   bro.conf.flush();   // the file holds lastFile now: a process started next reads it
 */
bro.conf.flush = function() {};

/**
 * Resets a configuration key to its default value by removing any user override.
 * Like `set`, it changes memory at once and the file on the next coalesced write.
 *
 * @param {string} keyOrPath - Dotted key path ("section.key") or section path
 * @param {string} [optKey] - Key name if first parameter is section path
 * @returns {boolean} True if successfully reset
 */
bro.conf.reset = function(keyOrPath, optKey) {};

/**
 * Checks whether a configuration key exists in storage or in registered schemas.
 *
 * @param {string} keyOrPath - Dotted key path ("section.key") or section path
 * @param {string} [optKey] - Key name if first parameter is section path
 * @returns {boolean} True if the key exists
 */
bro.conf.has = function(keyOrPath, optKey) {};

/**
 * Checks whether a configuration key currently holds its default value (no user override).
 *
 * @param {string} keyOrPath - Dotted key path ("section.key") or section path
 * @param {string} [optKey] - Key name if first parameter is section path
 * @returns {boolean} True if the key has no user override
 */
bro.conf.isDefault = function(keyOrPath, optKey) {};

/**
 * Lists all keys defined in storage and schema under a given section path.
 *
 * @param {string} path - Section path
 * @returns {string[]} Array of key names
 */
bro.conf.listKeys = function(path) {};

/**
 * @typedef {Object} WatchHandle
 * @property {number} token - Numeric watch token
 * @property {function(): boolean} unwatch - Cancels the watch registration
 */

/**
 * Registers a change watcher for a section path or specific key. Notifications
 * from other threads or external processes are safely queued and delivered on
 * the JavaScript main thread during the engine frame pump.
 *
 * Another process changing the same settings file is heard when its write lands
 * (the file is watched); this process's own writes are not mistaken for external
 * changes, and a value set here but not yet written is never reverted by such a
 * reload.
 *
 * Callback receives `(key, newValue, oldValue, path)`.
 *
 * @param {string} pathOrKey - Section path or "section.key"
 * @param {string|function} [optKeyOrCallback] - Key name or callback function
 * @param {function} [optCallback] - Callback function if 3 arguments
 * @returns {WatchHandle} Handle with token and unwatch() method
 */
bro.conf.watch = function(pathOrKey, optKeyOrCallback, optCallback) {};

/**
 * Unregisters a watcher by its numeric token or WatchHandle object.
 *
 * @param {number|WatchHandle} tokenOrHandle - Watch token or WatchHandle
 * @returns {boolean} True if watcher was found and removed
 */
bro.conf.unwatch = function(tokenOrHandle) {};

/**
 * @typedef {Object} KeySchemaDefinition
 * @property {'bool'|'int'|'double'|'string'|'string_list'|'enum'|'color'|'rect'|'dictionary'} type - Value type
 * @property {*} [default] - Default value
 * @property {number} [min] - Minimum allowed value for numeric types
 * @property {number} [max] - Maximum allowed value for numeric types
 * @property {string[]} [enum] - Allowed values for enum types
 * @property {string} [summary] - One-line summary
 * @property {string} [description] - Detailed description
 */

/**
 * @typedef {Object} SchemaDefinition
 * @property {string} id - Unique schema identifier (e.g. 'org.bro.desktop.interface')
 * @property {string} [path] - Path prefix in the settings store (defaults to id)
 * @property {Object.<string, KeySchemaDefinition>} keys - Key schema specifications
 */

/**
 * Registers a configuration schema with the settings store. Schemas provide
 * default values and validate types, numeric ranges, and enum values.
 *
 * @param {SchemaDefinition} schemaDefinition - Schema definition object
 */
bro.conf.registerSchema = function(schemaDefinition) {};
