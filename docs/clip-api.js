/**
 * @file docs/clip-api.js
 * @summary Documentation and examples for the `bro.clip` JavaScript API.
 */

/**
 * `bro.clip` provides a desktop clipboard manager and history service (backed
 * by native `broclip`) for desktop shells and clipboard utilities:
 * - Multi-entry clipboard and primary-selection history with search, MIME filtering, and pagination (`getHistory`, `getEntry`, `count`)
 * - Reading and writing clipboard or primary selection text (`getText`, `setText`, `current`)
 * - Pinning, removing, clearing, and re-activating history entries (`setPinned`, `remove`, `clear`, `paste`, `copyEntry`)
 * - Live clipboard change and clear events (`on`, `off`, `addEventListener`, `removeEventListener`, `onclip`, `onclear`)
 *
 * For standard web clipboard access in any application (`navigator.clipboard.readText`,
 * `writeText`, `read`, `write`, `ClipboardItem`), see `docs/clipboard-api.js`.
 *
 * Gated by the `clip` capability (docs/apps.md, Permissions): an app that declares it in
 * bro.json's `permissions` is asked at first touch; until granted it is the unavailable stub.
 * Compiled in with `BRO_WITH_CLIP`.
 */

// ============================================================================
// Type Definitions
// ============================================================================

/**
 * Clipboard selection target: `'clipboard'` (or `0`) for the standard system
 * clipboard, `'primary'` (or `1`) for the X11/Wayland primary selection.
 * @typedef {'clipboard'|'primary'|'Clipboard'|'Primary'|0|1} SelectionKind
 */

/**
 * Query options for `bro.clip.getHistory(options?)`.
 * @typedef {Object} ClipHistoryQuery
 * @property {string} [filterText] - Case-insensitive substring filter on text entries (aliases: `filter_text`, `text`)
 * @property {string} [mimeFilter] - Filter entries offering a matching MIME type (aliases: `mime_filter`, `mime`)
 * @property {SelectionKind} [selectionKind] - Filter by `'clipboard'` or `'primary'` (aliases: `selection_kind`, `kind`)
 * @property {boolean} [pinnedOnly] - When true, return only pinned entries (aliases: `pinned_only`, `pinned`)
 * @property {number} [since] - Only return entries copied at or after this Unix epoch timestamp in milliseconds
 * @property {number} [maxResults=50] - Maximum number of entries to return (aliases: `max_results`, `limit`)
 * @property {number} [offset=0] - Number of matching entries to skip for pagination
 */

/**
 * Summary entry returned by `bro.clip.getHistory(options?)`.
 * @typedef {Object} ClipHistoryItem
 * @property {number} id - Unique entry identifier
 * @property {number} timestamp - Capture timestamp in milliseconds since Unix epoch
 * @property {string} source - Source backend (`'local_app'`, `'wayland_data_control'`, `'x11'`, `'win32'`, `'mac_pasteboard'`, `'portal'`, `'internal'`, `'unknown'`)
 * @property {string} [appName] - Source application name if known (alias: `app_name`)
 * @property {boolean} isPinned - Whether the entry is pinned against LRU eviction (alias: `pinned`)
 * @property {string} previewText - Short single-line preview string (alias: `preview`)
 * @property {string[]} mimeTypes - Offered MIME types for this entry (alias: `mime_types`)
 * @property {number} byteSize - Total payload size in bytes (alias: `byte_size`)
 * @property {'clipboard'|'primary'} kind - Selection kind (alias: `selectionKind`)
 * @property {'clipboard'|'primary'} selectionKind - Selection kind
 * @property {string} [text] - Full UTF-8 text payload when the entry carries text
 */

/**
 * Individual MIME representation inside `ClipEntry.payloads`.
 * @typedef {Object} ClipPayloadItem
 * @property {string} mime - MIME type string (e.g. `'text/plain;charset=utf-8'`, `'image/png'`)
 * @property {number} size - Payload size in bytes
 * @property {string} [text] - Decoded UTF-8 string for text MIME types
 * @property {ArrayBuffer} [data] - Raw binary bytes for non-text MIME types
 */

/**
 * Full clipboard entry returned by `bro.clip.getEntry(id)`, `bro.clip.current()`, and `'clip'` events.
 * @typedef {Object} ClipEntry
 * @property {number} id - Unique entry identifier
 * @property {number} timestamp - Capture timestamp in milliseconds since Unix epoch
 * @property {string} source - Source backend kind
 * @property {string} [appName] - Source application name if known (alias: `app_name`)
 * @property {boolean} isPinned - Whether the entry is pinned (alias: `pinned`)
 * @property {string} previewText - Short preview string (alias: `preview`)
 * @property {number} byteSize - Total payload size in bytes (alias: `byte_size`)
 * @property {'clipboard'|'primary'} kind - Selection kind (alias: `selectionKind`)
 * @property {'clipboard'|'primary'} selectionKind - Selection kind
 * @property {string|null} text - UTF-8 text content, or `null` if the entry has no text representation
 * @property {string[]} mimeTypes - Offered MIME types (alias: `mime_types`)
 * @property {Object.<string, string|ArrayBuffer>} types - Map of MIME type to string (for text) or `ArrayBuffer` (for binary)
 * @property {Object.<string, string|ArrayBuffer>} data - Alias for `types`
 * @property {ClipPayloadItem[]} payloads - Array of all MIME payloads carried by the entry
 */

// ============================================================================
// API Stubs
// ============================================================================

/**
 * Whether `bro.clip` is compiled in and granted to this application.
 * @type {boolean}
 */
bro.clip.available;

/**
 * Queries the clipboard history ring with optional filtering and pagination.
 *
 * @param {ClipHistoryQuery} [options]
 * @returns {ClipHistoryItem[]}
 */
bro.clip.getHistory = function(options) {};

/**
 * Retrieves a complete clipboard entry (including all MIME payloads) by ID.
 *
 * @param {number|string} id - Entry ID
 * @returns {ClipEntry|null}
 */
bro.clip.getEntry = function(id) {};

/**
 * Reads plain text from the current clipboard, primary selection, or a specific
 * history entry ID. Returns `""` when no text representation is available.
 *
 * @param {number|string|SelectionKind} [idOrSelectionKind='clipboard'] - Entry ID, `'clipboard'`, or `'primary'`
 * @returns {string}
 */
bro.clip.getText = function(idOrSelectionKind) {};

/**
 * Writes plain text to the clipboard or primary selection and records it in history.
 *
 * @param {string} text - UTF-8 text to copy
 * @param {SelectionKind} [selectionKind='clipboard'] - Target selection
 * @returns {boolean} True on success
 */
bro.clip.setText = function(text, selectionKind) {};

/**
 * Pins or unpins a history entry so it is exempt from LRU capacity eviction
 * and `clear()`.
 *
 * @param {number|string} id - Entry ID
 * @param {boolean} [isPinned=true] - Pin state
 * @returns {boolean} True if the entry was found and updated
 */
bro.clip.setPinned = function(id, isPinned) {};

/**
 * Removes a single entry from clipboard history by ID.
 *
 * @param {number|string} id - Entry ID
 * @returns {boolean} True if the entry was found and removed
 */
bro.clip.remove = function(id) {};

/**
 * Clears unpinned entries from clipboard history (all selections when omitted,
 * or only the specified selection kind).
 *
 * @param {SelectionKind} [selectionKind]
 * @returns {boolean}
 */
bro.clip.clear = function(selectionKind) {};

/**
 * Promotes an existing history entry back to the active clipboard (or primary
 * selection) with a fresh timestamp.
 *
 * @param {number|string} id - Entry ID to activate
 * @param {SelectionKind} [selectionKind] - Optional override for target selection
 * @returns {boolean} True if the entry was found and set as current
 */
bro.clip.paste = function(id, selectionKind) {};

/**
 * Alias for `bro.clip.paste(id, selectionKind)`.
 *
 * @param {number|string} id - Entry ID to activate
 * @param {SelectionKind} [selectionKind] - Optional override for target selection
 * @returns {boolean}
 */
bro.clip.copyEntry = function(id, selectionKind) {};

/**
 * Returns the currently active entry for the given selection kind, or `null` if empty.
 *
 * @param {SelectionKind} [selectionKind='clipboard']
 * @returns {ClipEntry|null}
 */
bro.clip.current = function(selectionKind) {};

/**
 * Returns the number of entries currently stored in clipboard history.
 *
 * @param {SelectionKind} [selectionKind] - Optional filter by selection kind
 * @returns {number}
 */
bro.clip.count = function(selectionKind) {};

/**
 * Subscribes to clipboard manager events:
 * - `'clip'`: fired when a new entry is captured; callback receives `(entry: ClipEntry)`
 * - `'clear'`: fired when history is cleared; callback receives `({ type: 'clear', selectionKind, kind })`
 * - `'change'` / `'*'`: fired on any `'clip'` or `'clear'` event
 *
 * @param {'clip'|'clear'|'change'|'*'} event
 * @param {function( Object ): void} callback
 * @returns {typeof bro.clip} `bro.clip` for chaining
 */
bro.clip.on = function(event, callback) {};

/**
 * Unsubscribes a previously registered clipboard event listener.
 *
 * @param {'clip'|'clear'|'change'|'*'} event
 * @param {function( Object ): void} callback
 * @returns {typeof bro.clip}
 */
bro.clip.off = function(event, callback) {};

/**
 * Alias for `bro.clip.on(event, callback)`.
 * @param {'clip'|'clear'|'change'|'*'} event
 * @param {function( Object ): void} callback
 * @returns {typeof bro.clip}
 */
bro.clip.addEventListener = function(event, callback) {};

/**
 * Alias for `bro.clip.off(event, callback)`.
 * @param {'clip'|'clear'|'change'|'*'} event
 * @param {function( Object ): void} callback
 * @returns {typeof bro.clip}
 */
bro.clip.removeEventListener = function(event, callback) {};

/**
 * Optional property handler invoked when a `'clip'` event fires.
 * @type {?function(ClipEntry): void}
 */
bro.clip.onclip = null;

/**
 * Optional property handler invoked when a `'clear'` event fires.
 * @type {?function({type: 'clear', selectionKind: string, kind: string}): void}
 */
bro.clip.onclear = null;

// ============================================================================
// Examples
// ============================================================================

if (bro.clip.available) {
    // 1. Copy and read text from the clipboard manager
    bro.clip.setText('Hello from bro.clip!', 'clipboard');
    console.log('Current text:', bro.clip.getText());
    console.log('History count:', bro.clip.count());

    // 2. Query recent clipboard history
    const items = bro.clip.getHistory({ maxResults: 20, selectionKind: 'clipboard' });
    for (const item of items) {
        console.log(`[#${item.id}] ${item.previewText} (${item.mimeTypes.join(', ')})`);
    }

    // 3. Pin an important entry and inspect its full MIME payloads
    if (items.length > 0) {
        bro.clip.setPinned(items[0].id, true);
        const full = bro.clip.getEntry(items[0].id);
        console.log('Full entry payloads:', full.payloads.length);
    }

    // 4. Listen for live clipboard updates
    bro.clip.on('clip', (entry) => {
        console.log(`New ${entry.selectionKind} clip #${entry.id}: ${entry.previewText}`);
    });
}
