/**
 * =============================================================================
 * bro Native File Dialogs & Modal Dialogs API
 * =============================================================================
 *
 * Native open/save file dialogs backed by SDL3's portable file dialog API, plus
 * the browser's standard modal dialog trio (alert, confirm, prompt). All six are
 * globals (on `globalThis`), not members of `bro`.
 *
 * In a window every one of them blocks until the user answers (the frames
 * stop behind it); the `*Async` file dialogs below do not. Headless never
 * blocks: the file dialogs answer with the paths queued by `setPickedFiles()`
 * (none queued is a cancelled pick), and alert/confirm/prompt follow
 * `setDialogAnswer()` (see docs/headless.md).
 *
 * @example
 *   const files = showOpenFileDialog('Audio Files|wav;flac;mp3;ogg;opus');
 *   if (files.length) {
 *     console.log('Selected file:', files[0]);
 *   }
 *
 * @example
 *   if (confirm('Are you sure you want to proceed?')) {
 *     alert('Action confirmed');
 *   }
 */

/**
 * @param {string} [message]
 */
function alert(message) {}

/**
 * @param {string} [message]
 * @returns {boolean}
 */
function confirm(message) {}

/**
 * @param {string} [message]
 * @param {string} [defaultText]
 * @returns {string|null} The entered text, or null when cancelled.
 */
function prompt(message, defaultText) {}

/**
 * @param {string} [filter] `'Label|ext;ext'`. A filter SDL refuses is thrown
 *   as a TypeError rather than returned.
 * @param {string} [defaultName]
 * @returns {string|null} The chosen path, or null when cancelled.
 */
function showSaveFileDialog(filter, defaultName) {}

/**
 * @param {string} [filter] `'Label|ext;ext'`. A filter SDL refuses is thrown
 *   as a TypeError rather than returned.
 * @param {boolean} [allowMultiple]
 * @returns {string[]} The chosen paths; empty when cancelled.
 */
function showOpenFileDialog(filter, allowMultiple) {}

/**
 * @param {string} [defaultLocation]
 * @param {boolean} [allowMultiple]
 * @returns {string[]} The chosen folders; empty when cancelled.
 */
function showOpenFolderDialog(defaultLocation, allowMultiple) {}

/**
 * The three file dialogs without blocking: each returns at once with a
 * Promise, and the page keeps running its frames, timers and script-fed
 * audio while the dialog is up (a blocking dialog stops them: a player fed
 * from requestAnimationFrame falls silent). The promise settles at a frame
 * after the answer, never inside the call; headless answers from
 * `setPickedFiles()` / `setDialogAnswer()` as the blocking dialogs do.
 * A filter SDL refuses rejects with a TypeError.
 *
 * @example
 *   const path = await showSaveFileDialogAsync('WAV|wav', 'song.wav');
 *   if (path) save(path);
 *
 * @param {string} [filter]
 * @param {string} [defaultName]
 * @returns {Promise<string|null>} The chosen path, or null when cancelled.
 */
function showSaveFileDialogAsync(filter, defaultName) {}

/**
 * @param {string} [filter]
 * @param {boolean} [allowMultiple]
 * @returns {Promise<string[]>} The chosen paths; empty when cancelled.
 */
function showOpenFileDialogAsync(filter, allowMultiple) {}

/**
 * @param {string} [defaultLocation]
 * @param {boolean} [allowMultiple]
 * @returns {Promise<string[]>} The chosen folders; empty when cancelled.
 */
function showOpenFolderDialogAsync(defaultLocation, allowMultiple) {}
