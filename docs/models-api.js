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
 *                     elsewhere; <BRO_APP_HOME>/models when that variable is
 *                     set (a scratch profile: tests, a clean run). A place to
 *                     put weights, and where bro.models.fetch installs them;
 *                     created by the first fetch.
 *
 * Empty entries are skipped and a relative entry is taken against the
 * working directory. A directory is listed whether or not it exists: the list
 * says where bro looks.
 *
 * A model is a directory: `<dir>/<name>`, with `name` the folder name the
 * weights ship as ("sam-vit-base", "sam2.1-hiera-tiny") or a relative path
 * below a models directory ("vision/sam-vit-base").
 *
 * Fetching: a model nobody has put on the machine can be installed from the
 * network, `await bro.models.fetch(name)`, from an ordered list of Hugging
 * Face datasets laid out as model folders (`bro.models.sources`; the
 * `models.sources` setting). The download runs on its own thread and the page
 * keeps running; the folder appears in the per-user models directory in one
 * rename once every file is in and verified, so find() never answers a
 * half-installed model. After that, find() is all an app needs: bro keeps
 * no other record of what was fetched.
 *
 * Main realm only: the settings are the engine's, read on its thread. A Worker
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
 *   // Offer the download when the model is missing, then carry on.
 *   let dir = bro.models.find('sam2.1-hiera-base-plus');
 *   if (!dir) {
 *     const ac = new AbortController();
 *     cancelButton.onclick = () => ac.abort();
 *     try {
 *       dir = await bro.models.fetch('sam2.1-hiera-base-plus', {
 *         signal: ac.signal,
 *         onProgress: ({ done, total }) => { bar.value = total ? done / total : 0; },
 *       });
 *     } catch (e) {
 *       if (e.name === 'AbortError') return;
 *       hint.textContent = e.code === 'network' ? 'Offline? ' + e.message : e.message;  // offer a retry
 *       return;
 *     }
 *   }
 *   const sam = bro.vision.loadSam2(dir);
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

/**
 * Where fetch() and available() look, in order: the `models.sources`
 * setting's entries (docs/settings.md), by default
 * `['wlejon/brovisionml-data', 'wlejon/brosoundml-data']`. An entry is a
 * Hugging Face dataset `<owner>/<repo>` (on https://huggingface.co), or a
 * full URL `<endpoint>/datasets/<owner>/<repo>` for a mirror or a local
 * server that answers the same paths:
 *   <endpoint>/api/datasets/<owner>/<repo>/tree/main/<name>     the folder's listing
 *   <endpoint>/datasets/<owner>/<repo>/resolve/main/<path>      a file
 * A fresh array on every read.
 * @readonly
 * @type {string[]}
 */
bro.models.sources;

/**
 * @typedef {object} ModelFetchProgress
 * @property {number} done   bytes of the folder's files on disk (a resumed
 *   file counts what was already there); grows, and falls only when bytes
 *   are thrown away (a server that will not resume)
 * @property {number} total  the folder's size in bytes
 * @property {string} file   the file being transferred, relative to the folder
 *   ('' once all are in)
 * @property {number} files  the folder's file count
 * @property {string} source the source supplying it
 */

/**
 * Install the model folder `name` into the per-user models directory (the
 * last of `bro.models.dirs`) and resolve with its absolute path. When find()
 * already answers `name` that directory is the result at once, with no
 * network.
 *
 * The first source whose listing has the folder supplies every file in it,
 * subfolders included. Files stream to `<models dir>/.bro-fetch/<name>/`, each
 * verified against the listing (an LFS file's sha256, any other file's git
 * blob sha1) before it counts; when all are in, the folder is renamed into
 * place in one step. A transfer that fails (connection lost, timeout, a 5xx)
 * is retried, resuming from the bytes on disk with an HTTP Range request, a
 * few times with a growing pause; a fetch that fails or is aborted leaves
 * those partial files staged, and the next fetch of `name` resumes them. A
 * file whose hash does not match is deleted and the fetch fails. Two fetch()
 * calls for one name in one bro share one download.
 *
 * Runs on its own thread: the page keeps running, and onProgress is called
 * from the event loop (about every 50 ms while bytes arrive, and once at the
 * end with done === total). Main realm only.
 *
 * @param {string} name a relative folder name, as for find(): a TypeError
 *   (as a rejection) for an empty, absolute or ".." name
 * @param {object} [opts]
 * @param {(p: ModelFetchProgress) => void} [opts.onProgress] a throw from it
 *   stops the fetch and rejects with that error
 * @param {AbortSignal} [opts.signal] aborting stops the transfer (its bytes
 *   stay staged for a later resume) and rejects with the signal's reason, an
 *   AbortError by default
 * @returns {Promise<string>} the installed directory
 * @throws (as a rejection) an Error with `code`:
 *   'not-found'    no source has the folder
 *   'network'      a source could not be reached, or a file kept failing:
 *                  offline, DNS, a timeout, an HTTP error
 *   'integrity'    a file's bytes do not match the source's hash
 *   'io'           the models directory could not be written (disk full,
 *                  permissions)
 *   'unsupported'  this build has no HTTP client
 *   'permission'   the app may not use the network (the `net` capability,
 *                  docs/apps.md, Permissions); the message says why. A
 *                  model already in a models directory needs no network.
 * @example
 *   const dir = await bro.models.fetch('sam2.1-hiera-base-plus', {
 *     onProgress: ({ done, total }) => console.log(`${(100 * done / total).toFixed(0)}%`),
 *   });
 *   // 323 MB from wlejon/brovisionml-data: about 6 s on a fast line
 */
bro.models.fetch = function(name, opts) {};

/**
 * Whether a source has the model folder `name`, without downloading it: the
 * listing only (one request per folder, about a second). Resolves with what
 * fetch() would install, or null when no source has it; rejects (code
 * 'network') when a source could not be asked, so offline is not "absent".
 * Does not look at the machine: find() answers what is installed.
 * @param {string} name as for fetch()
 * @param {{signal?: AbortSignal}} [opts]
 * @returns {Promise<{source: string, bytes: number, files: number}|null>}
 * @example
 *   const a = await bro.models.available('sam2.1-hiera-base-plus');
 *   // { source: 'wlejon/brovisionml-data', bytes: 323497339, files: 7 }
 */
bro.models.available = function(name, opts) {};
