/**
 * =============================================================================
 * Clipboard API Reference — navigator.clipboard, ClipboardItem
 * =============================================================================
 *
 * The async Clipboard API as Chromium ships it: text with readText/writeText,
 * and items carrying several representations (text and an image) with
 * write/read over `ClipboardItem`. The clipboard is the same one the editing
 * shortcuts, `document.execCommand('copy')`, the `paste` event and every
 * `<terminal>` use.
 *
 * Where the pieces live:
 *   - readText / writeText        src/bronze_host/host_navigator.cpp
 *   - ClipboardItem, write, read  src/bronze_host/js/clipboard.js
 *   - the OS clipboards           src/platform/clipboard.h (SDL, Win32, Wayland,
 *                                 the in-process one for DRM and headless)
 *
 * SUPPORTED TYPES: "text/plain" (UTF-8) and "image/png". `ClipboardItem.supports`
 * answers for exactly these; `write` refuses anything else with NotAllowedError.
 *
 * WHAT ANOTHER APP SEES / WHAT BRO READS:
 *   - Windows: text is CF_UNICODETEXT (LF written as CRLF). A PNG is put up as
 *     the registered "PNG" format (browsers, Office, chat apps: alpha kept)
 *     AND as CF_DIB + CF_DIBV5 (Paint and other native apps), in one write.
 *     `read()` takes "PNG" when it is there, else CF_DIBV5 / CF_DIB (a
 *     screenshot from PrtScn or Snipping Tool), converted to PNG.
 *   - Linux X11 / Wayland (SDL or bro's own Wayland backend): every
 *     representation is offered under its MIME type, text also under the
 *     usual text targets (UTF8_STRING, text/plain;charset=utf-8, ...).
 *     `read()` takes image/png, else image/bmp, image/jpeg, image/gif, ...,
 *     converted to PNG.
 *   - macOS: through SDL, which maps MIME types to pasteboard types
 *     (image/png = public.png). Compiled, not verified on a Mac.
 *   - DRM (bro as the display server): the clipboard is the process's own.
 *
 * The OS clipboard is opened exclusively on Windows; a write or read that
 * finds another process holding it retries for a few milliseconds and then
 * fails (write rejects with NotAllowedError) rather than claiming a copy it
 * did not make.
 *
 * HEADLESS: bro-headless has an in-process clipboard. Nothing a test writes
 * reaches the machine's clipboard and nothing on it leaks into a test, so a
 * round trip is exact and deterministic. `BRO_HEADLESS_SYSTEM_CLIPBOARD=1`
 * points headless at the real OS clipboard instead (for checking what other
 * apps paste; never in the test suite).
 *
 * @example
 *   // --- Copy a canvas as an image ------------------------------------------
 *   canvas.toBlob(async (png) => {
 *     await navigator.clipboard.write([new ClipboardItem({ 'image/png': png })]);
 *   });
 *
 * @example
 *   // --- Text and an image in one item --------------------------------------
 *   // Pasted into an editor it is the caption; into an image app, the image.
 *   await navigator.clipboard.write([new ClipboardItem({
 *     'text/plain': 'sunset.png — 1920x1080',
 *     'image/png': pngBlob,              // or a Promise<Blob>, resolved at write
 *   })]);
 *
 * @example
 *   // --- Paste an image (a screenshot) --------------------------------------
 *   const [item] = await navigator.clipboard.read();
 *   if (item && item.types.includes('image/png')) {
 *     const blob = await item.getType('image/png');
 *     img.src = URL.createObjectURL(blob);
 *   }
 *
 * @example
 *   // --- Or from the paste event --------------------------------------------
 *   document.addEventListener('paste', (e) => {
 *     for (const it of e.clipboardData.items) {
 *       if (it.type === 'image/png') useImage(it.getAsFile());
 *     }
 *   });
 */

/**
 * The page's clipboard.
 * @namespace
 */
navigator.clipboard = {};

/**
 * The clipboard's text. "" for an empty clipboard, or one holding only an
 * image.
 * @returns {Promise<string>}
 */
navigator.clipboard.readText = function() {};

/**
 * Replace the clipboard with `text` (a non-string writes ""). Rejects with an
 * Error when the OS refused the write.
 * @param {string} text
 * @returns {Promise<void>}
 */
navigator.clipboard.writeText = function(text) {};

/**
 * Replace the clipboard with one item, every representation at once.
 * Each value is awaited (`getType`) and the whole item written in a single
 * OS clipboard write. Rejects:
 *   - TypeError: `items` is not a sequence of ClipboardItem.
 *   - NotAllowedError: more than one item (as in every browser), a type
 *     `ClipboardItem.supports` refuses, or the OS refused the write.
 *   - DataError: an "image/png" representation whose bytes are not a PNG.
 * An empty array clears the clipboard. A refused write leaves the clipboard as
 * it was.
 * @param {ClipboardItem[]} items
 * @returns {Promise<void>}
 */
navigator.clipboard.write = function(items) {};

/**
 * The clipboard as ClipboardItems: one item with "text/plain" when there is
 * text and "image/png" when there is an image (whatever format the OS holds
 * it in, re-encoded as PNG); `[]` when it holds neither.
 * @returns {Promise<ClipboardItem[]>}
 */
navigator.clipboard.read = function() {};

/**
 * One clipboard item: a set of representations keyed by MIME type.
 * @class
 * @param {Object<string, Blob|string|Promise<Blob|string>>} items  at least one
 *   entry; a string is a Blob of that type.
 * @param {{presentationStyle?: 'unspecified'|'inline'|'attachment'}} [options]
 * @throws {TypeError} when `items` is not an object, or is empty.
 * @example
 *   const item = new ClipboardItem({ 'text/plain': 'hi' });
 *   item.types;                                   // ['text/plain']
 *   await (await item.getType('text/plain')).text();  // 'hi'
 */
function ClipboardItem(items, options) {}

/** The MIME types, in the order given. Frozen. @type {ReadonlyArray<string>} */
ClipboardItem.prototype.types = [];

/** @type {'unspecified'|'inline'|'attachment'} */
ClipboardItem.prototype.presentationStyle = 'unspecified';

/**
 * The representation for `type` as a Blob. Rejects with NotFoundError when the
 * item has no such type.
 * @param {string} type
 * @returns {Promise<Blob>}
 */
ClipboardItem.prototype.getType = function(type) {};

/**
 * Whether `write` accepts `type`: "text/plain" and "image/png".
 * @param {string} type
 * @returns {boolean}
 */
ClipboardItem.supports = function(type) {};
