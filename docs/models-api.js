/**
 * =============================================================================
 * bro.models — where model weights live
 * =============================================================================
 *
 * The ML namespaces (bro.vision, bro.lm, bro.tts, bro.diffusion, ...) load
 * weights from a directory you name; bro.models is the one convention for
 * where those directories are, so an app asks for a model by name instead of
 * carrying a path that only works on the machine it was written on.
 *
 * The search list, in order (each entry made absolute, listed once):
 *   1. BRO_MODELS     environment variable, a path list in the platform's PATH
 *                     form: ';' between entries on Windows, ':' elsewhere
 *                     (`require('path').delimiter`).
 *   2. models.dirs    the bro setting (docs/settings.md), a path list in the
 *                     same form. Set it as any setting:
 *                     `bro.settings.set('models.dirs', 'D:/models;E:/weights')`
 *                     (persisted for the app), or `bro.json`'s defaults.
 *   3. <bro user data dir>/models, always last: %APPDATA%\bro\models on
 *                     Windows, ~/Library/Application Support/bro/models on
 *                     macOS, $XDG_DATA_HOME/bro/models (~/.local/share/bro/models)
 *                     elsewhere. Not created by bro; a place to put weights.
 *
 * Empty entries are skipped and a relative entry is taken against the
 * working directory. A directory is listed whether or not it exists: the list
 * says where bro looks.
 *
 * A model is a directory: `<dir>/<name>`, with `name` the folder name the
 * weights ship as ("sam-vit-base", "sam2.1-hiera-tiny") or a relative path
 * below a models directory ("vision/sam-vit-base").
 *
 * Main realm only: the setting is the engine's, read on its thread. A Worker
 * that loads a model is handed the directory the page found.
 *
 * @example
 *   // Load SAM wherever the user keeps it, or leave the feature off.
 *   const dir = bro.models.find('sam-vit-base');
 *   if (!dir) {
 *     hint.textContent = `SAM weights not found in: ${bro.models.dirs.join(', ')}`;
 *   } else {
 *     const sam = bro.vision.loadSam(dir);
 *   }
 *
 * @example
 *   // A test that needs weights skips cleanly without them.
 *   const dir = bro.models.find('sam-vit-base');
 *   if (!dir) skipTest('no sam-vit-base in bro.models.dirs');
 */

/**
 * The directories searched, in order: BRO_MODELS's, then the `models.dirs`
 * setting's, then the per-user models directory. Absolute, each once (on
 * Windows compared without case). A fresh array on every read, following the
 * environment and the setting as they are now.
 * @readonly
 * @type {string[]}
 */
bro.models.dirs;

/**
 * The first `<dir>/<name>` in `bro.models.dirs` that is an existing
 * directory, as an absolute path in the platform's separators; null when no
 * directory holds it (a FILE of that name does not count).
 * @param {string} name a relative path: one or more segments, no "..";
 *   an empty, non-string or absolute name, or one with a ".." segment, is a
 *   TypeError
 * @returns {string|null}
 * @example
 *   bro.models.find('sam2.1-hiera-tiny');   // 'D:\\models\\sam2.1-hiera-tiny', or null
 */
bro.models.find = function(name) {};
