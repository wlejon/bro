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

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Native modal dialogs and file system pickers interface.
 */
class Dialogs {

  /**
   * Displays a modal alert dialog with an optional message.
   *
   * @param {*} [message] - Text to display
   */
  static alert(message) {}

  /**
   * Displays a modal confirmation dialog with OK and Cancel buttons.
   *
   * @param {*} [message] - Prompt message to display
   * @returns {boolean} True if OK was clicked, false if cancelled
   */
  static confirm(message) {}

  /**
   * Displays a modal dialog with a text prompt and default value.
   *
   * @param {*} [message] - Prompt message to display
   * @param {string} [defaultText] - Default input value
   * @returns {string|null} String response or null if cancelled
   */
  static prompt(message, defaultText) {}

  /**
   * Opens a native modal file picker dialog.
   *
   * @param {string} [filter] - Name and pattern, alternating, for one filter or
   *   several (e.g. "Images|png;jpg" or "Images|png;jpg|All files|*"). A
   *   pattern is `[a-zA-Z0-9_.-]` extensions separated by `;`, or a bare `*`;
   *   anything else is refused before the dialog opens and throws.
   * @param {boolean} [allowMultiple] - Whether to allow multiple file selection
   * @returns {Array<string>} Array of selected absolute file paths
   */
  static showOpenFileDialog(filter, allowMultiple) {}

  /**
   * Opens a native modal directory picker dialog.
   *
   * @param {string} [defaultLocation] - Starting directory path
   * @param {boolean} [allowMultiple] - Whether to allow multiple folder selection
   * @returns {Array<string>} Array of selected absolute folder paths
   */
  static showOpenFolderDialog(defaultLocation, allowMultiple) {}

  /**
   * Opens a native modal file save dialog.
   *
   * @param {string} [filter] - Name and pattern, alternating, for one filter or
   *   several (e.g. "JSON|json" or "JSON|json|All files|*"). Refused patterns
   *   throw.
   * @param {string} [defaultName] - Default location or file path
   * @returns {string|null} Selected file path string or null if cancelled
   */
  static showSaveFileDialog(filter, defaultName) {}

}

