/**
 * =============================================================================
 * bro Native File Dialogs & Modal Dialogs API
 * =============================================================================
 *
 * Native open/save file dialogs backed by SDL3's portable file dialog API, plus
 * the browser's standard modal dialog trio (alert, confirm, prompt).
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

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Native modal dialogs and file system pickers interface.
 */
/**
 * @param {string} [message]
 */
bro.dialogs.alert = function(message) {};

/**
 * @param {string} [message]
 * @returns {boolean}
 */
bro.dialogs.confirm = function(message) {};

/**
 * @param {string} [message]
 * @param {string} [defaultText]
 * @returns {string}
 */
bro.dialogs.prompt = function(message, defaultText) {};

/**
 * @param {string} [filter]
 * @param {string} [defaultName]
 * @returns {string}
 */
bro.dialogs.showSaveFileDialog = function(filter, defaultName) {};

/**
 * @param {string} [filter]
 * @param {boolean} [allowMultiple]
 * @returns {string}
 */
bro.dialogs.showOpenFileDialog = function(filter, allowMultiple) {};

/**
 * @param {string} [defaultLocation]
 * @param {boolean} [allowMultiple]
 * @returns {string}
 */
bro.dialogs.showOpenFolderDialog = function(defaultLocation, allowMultiple) {};

