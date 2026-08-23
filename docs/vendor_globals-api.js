/**
 * =============================================================================
 * Vendor Globals — Third-Party Vendored Library Bridges
 * =============================================================================
 *
 * Bridges global symbols loaded via script tags (signals, CodeMirror, acorn,
 * tern, esprima, jsonlint, draco_encoder) so AOT-compiled Bronze apps can read
 * and invoke them seamlessly.
 *
 * @example
 *   // Access vendored global library from compiled realm
 *   if (typeof CodeMirror !== 'undefined') {
 *     console.log('CodeMirror loaded');
 *   }
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Third-party vendor globals bridge namespace.
 */
/**
 *  Signals event emitter / reactive signal library.
 * @readonly
 * @type {*}
 */
bro.vendor_globals.signals;

/**
 *  CodeMirror in-browser code editor.
 * @readonly
 * @type {*}
 */
bro.vendor_globals.CodeMirror;

/**
 *  Acorn JavaScript parser.
 * @readonly
 * @type {*}
 */
bro.vendor_globals.acorn;

/**
 *  Tern JavaScript code-analysis engine.
 * @readonly
 * @type {*}
 */
bro.vendor_globals.tern;

/**
 *  Esprima ECMAScript parsing infrastructure.
 * @readonly
 * @type {*}
 */
bro.vendor_globals.esprima;

/**
 *  JSONLint JSON validator.
 * @readonly
 * @type {*}
 */
bro.vendor_globals.jsonlint;

/**
 *  Draco geometry mesh encoder.
 * @readonly
 * @type {*}
 */
bro.vendor_globals.draco_encoder;

