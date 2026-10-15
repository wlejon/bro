/**
 * @file docs/ime-api.js
 * @summary Documentation and examples for the `bro.ime` JavaScript API.
 */

/**
 * `bro.ime` provides input method composition, dead-key / Multi_key sequence
 * handling, XKB keysym conversion, candidate popup placement math, and prefix
 * dictionary autocomplete (backed by native `broime`):
 * - Dead-key and compose sequence state machine (`feedKey`, `reset`, `isComposing`, `addRule`, `clearRules`)
 * - XKB keysym name, code, and UTF-8 string conversion (`keysymFromName`, `keysymToName`, `keysymToUtf8`, `keysymFromUtf8`)
 * - Screen-aware IME candidate popup placement (`computePopupPlacement`)
 * - Prefix dictionary trie lookup and insertion (`findPrefixSuggestions`, `insertWord`)
 *
 * Mounted automatically in Bronze when `BRO_WITH_IME` is enabled.
 */

// ============================================================================
// Type Definitions
// ============================================================================

/**
 * Result of feeding a key into the compose engine via `bro.ime.feedKey`.
 * @typedef {Object} ImeComposeResult
 * @property {'none'|'pending'|'matched'} status - `'pending'` while a multi-key sequence is in progress, `'matched'` when a sequence completes, or `'none'` when not composing
 * @property {string} text - Committed UTF-8 text when `status === 'matched'`, otherwise `""`
 */

/**
 * Rectangle used for cursor anchor and display bounds in `computePopupPlacement`.
 * Accepts `{ x, y, width, height }` or `{ left, top, w, h }`.
 * @typedef {Object} ImeAnchorRect
 * @property {number} [x] - Left coordinate in pixels (alias: `left`)
 * @property {number} [y] - Top coordinate in pixels (alias: `top`)
 * @property {number} [width] - Width in pixels (alias: `w`)
 * @property {number} [height] - Height in pixels (alias: `h`)
 */

/**
 * Computed candidate window placement returned by `bro.ime.computePopupPlacement`.
 * @typedef {Object} ImePopupPlacement
 * @property {number} x - Clamped X coordinate in display pixels
 * @property {number} y - Clamped Y coordinate in display pixels
 * @property {number} width - Popup width in pixels
 * @property {number} height - Popup height in pixels
 * @property {boolean} flippedY - True when the popup was flipped above the cursor to stay within `displayBounds`
 */

// ============================================================================
// API Stubs
// ============================================================================

/**
 * Whether the `broime` subsystem is compiled into this binary.
 * @type {boolean}
 */
bro.ime.available;

/**
 * Feeds a keysym number, XKB keysym name (e.g. `'Multi_key'`, `'dead_acute'`),
 * or single UTF-8 character string into the compose state machine.
 *
 * @param {number|string} keyNameOrSym - Numeric keysym, XKB keysym name, or UTF-8 character
 * @returns {ImeComposeResult}
 */
bro.ime.feedKey = function(keyNameOrSym) {};

/**
 * Resets any in-progress compose sequence back to the idle state.
 */
bro.ime.reset = function() {};

/**
 * Returns whether the compose engine is currently in the middle of a multi-key
 * or dead-key sequence.
 *
 * @returns {boolean}
 */
bro.ime.isComposing = function() {};

/**
 * Registers a custom compose rule mapping a sequence specification string to
 * a UTF-8 output string.
 *
 * @param {string} spec - Space-separated sequence of keysym names or `<Key>` tokens (e.g. `'<Multi_key> <c> <o>'`)
 * @param {string} output - Replacement UTF-8 text committed when the sequence matches
 * @returns {boolean} True if the rule was added
 */
bro.ime.addRule = function(spec, output) {};

/**
 * Clears all compose rules from the active compose engine.
 */
bro.ime.clearRules = function() {};

/**
 * Converts an XKB keysym name (or single UTF-8 character fallback) to its
 * numeric keysym code (`0` / `VoidSymbol` if unrecognized).
 *
 * @param {string} name - Keysym name (e.g. `'Return'`, `'dead_grave'`, `'a'`)
 * @returns {number}
 */
bro.ime.keysymFromName = function(name) {};

/**
 * Converts a numeric keysym code to its canonical XKB keysym name (`""` if unknown).
 *
 * @param {number} sym - Numeric keysym code
 * @returns {string}
 */
bro.ime.keysymToName = function(sym) {};

/**
 * Converts a numeric keysym code to its corresponding UTF-8 string (`""` for non-printable keysyms).
 *
 * @param {number} sym - Numeric keysym code
 * @returns {string}
 */
bro.ime.keysymToUtf8 = function(sym) {};

/**
 * Converts the first Unicode codepoint of a UTF-8 string to its numeric keysym
 * code (`0` / `VoidSymbol` if empty or invalid).
 *
 * @param {string} str - UTF-8 character string
 * @returns {number}
 */
bro.ime.keysymFromUtf8 = function(str) {};

/**
 * Computes the screen-clamped position for an IME candidate popup window
 * anchored to the text caret, flipping above the caret when there is
 * insufficient space below.
 *
 * @param {ImeAnchorRect} cursorRect - Caret rectangle `{ x, y, width, height }`
 * @param {number} popupWidth - Desired candidate popup width in pixels
 * @param {number} popupHeight - Desired candidate popup height in pixels
 * @param {ImeAnchorRect} displayBounds - Usable display work area `{ x, y, width, height }`
 * @param {number} [margin=4] - Vertical spacing between the caret and popup in pixels
 * @returns {ImePopupPlacement}
 */
bro.ime.computePopupPlacement = function(cursorRect, popupWidth, popupHeight, displayBounds, margin) {};

/**
 * Inserts a word into the prefix dictionary trie for autocomplete suggestions.
 *
 * @param {string} word - Word text to insert
 * @param {number} [score=0] - Frequency/priority score (higher scores rank first)
 * @param {string} [annotation=''] - Optional reading or annotation string
 * @returns {boolean} True if inserted
 */
bro.ime.insertWord = function(word, score, annotation) {};

/**
 * Looks up candidate words matching `prefix` in the dictionary trie, ordered
 * by score descending.
 *
 * @param {string} prefix - Prefix string to match
 * @param {number} [maxResults=20] - Maximum number of suggestions to return
 * @returns {string[]}
 */
bro.ime.findPrefixSuggestions = function(prefix, maxResults) {};

// ============================================================================
// Examples
// ============================================================================

if (bro.ime.available) {
    // 1. Dead-key / Multi_key composition
    bro.ime.reset();
    console.log(bro.ime.feedKey('dead_acute')); // { status: 'pending', text: '' }
    console.log(bro.ime.isComposing());         // true
    console.log(bro.ime.feedKey('e'));          // { status: 'matched', text: 'é' }

    // 2. Custom compose rule
    bro.ime.addRule('<Multi_key> <s0> <h>', '¯\\_(ツ)_/¯');

    // 3. Keysym conversions
    const sym = bro.ime.keysymFromName('EuroSign');
    console.log(bro.ime.keysymToName(sym), '->', bro.ime.keysymToUtf8(sym)); // 'EuroSign' -> '€'

    // 4. Candidate popup placement near bottom of screen
    const placement = bro.ime.computePopupPlacement(
        { x: 400, y: 1050, width: 2, height: 20 },
        240, 160,
        { x: 0, y: 0, width: 1920, height: 1080 },
        4
    );
    console.log('Popup placement:', placement); // flippedY: true

    // 5. Prefix dictionary suggestions
    bro.ime.insertWord('compositor', 100);
    bro.ime.insertWord('compose', 80);
    console.log(bro.ime.findPrefixSuggestions('comp', 5)); // ['compositor', 'compose']
}
