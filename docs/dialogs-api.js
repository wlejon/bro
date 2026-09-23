/**
 * =============================================================================
 * bro Native File Dialogs & Modal Dialogs API
 * =============================================================================
 *
 * Native open/save file dialogs backed by SDL3's portable file dialog API, plus
 * the browser's standard modal dialog trio (alert, confirm, prompt). All six are
 * globals (on `globalThis`), not members of `bro`.
 *
 * Every one of them blocks until the user answers, so never call one from a
 * headless test: there is nobody to answer it.
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
