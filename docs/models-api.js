/**
 * bro.models — on-demand model-weight acquisition
 *
 * AI apps need large model weights (LLM GGUFs, Whisper/Kokoro checkpoints,
 * wake-word models) that aren't bundled into a bro release. bro.models lets an
 * app declare the files it needs and fetch any that are missing at first run,
 * straight from the artifacts' upstream Hugging Face homes, into a shared
 * per-user cache — so a downloaded build is self-sufficient without shipping
 * gigabytes of weights.
 *
 * Cache location (override with the BRO_MODELS_DIR env var):
 *   Windows : %APPDATA%\bro\models
 *   macOS   : ~/Library/Application Support/bro/models
 *   Linux   : $XDG_DATA_HOME/bro/models  (or ~/.local/share/bro/models)
 * Files land at <cache>/<repo>/<file> (dataset repos get a datasets/ prefix),
 * mirroring the HF URL namespace.
 *
 * In a source checkout you usually already have the weights in sibling repos
 * (../brolm/weights, ../brosoundml/weights, ...). Give each spec a `dev` path
 * and resolve()/ensure() prefer it when present, so development never
 * re-downloads (notably the multi-GB LLM GGUF).
 *
 * Implementation: pure JS over brokit's fetch (streaming, redirect-following so
 * HF LFS CDN redirects work), fs, and crypto. Downloads stream to <file>.part
 * and rename on success, so a present file is always complete; large weights
 * never sit fully in memory.
 *
 * Declaring models in bro.json (recommended): add a top-level "models" array;
 * the app reads it and the `bro --fetch <app>` CLI reads the same array.
 *
 *   {
 *     "app": ".",
 *     "models": [
 *       { "id": "qwen.gguf", "repo": "Qwen/Qwen3-8B-GGUF", "kind": "model",
 *         "file": "Qwen3-8B-Q8_0.gguf",
 *         "dev": "../brolm/weights/Qwen3-8B-GGUF/Qwen3-8B-Q8_0.gguf" },
 *       { "id": "wake", "repo": "wlejon/brosoundml-data", "kind": "dataset",
 *         "file": "wake/computer.bw", "dev": "../brosoundml-data/wake/computer.bw" }
 *     ]
 *   }
 */


// ── A model spec ──────────────────────────────────────────────────────────────

/**
 * @typedef {Object} ModelSpec
 * @property {string} id     - Caller-chosen key; ensure() returns paths keyed by it.
 * @property {string} repo   - Hugging Face repo, e.g. "openai/whisper-tiny".
 * @property {string} file   - Path within the repo, e.g. "model.safetensors"
 *                             or "wake/computer.bw".
 * @property {('model'|'dataset')} [kind='model'] - Selects the HF URL form
 *                             (datasets/<repo> vs <repo>).
 * @property {string} [dev]  - Optional local sibling path; preferred over the
 *                             cache when it exists (source-checkout convenience).
 * @property {number} [bytes]  - Optional expected size; a cache file of a
 *                             different size is treated as missing + re-fetched.
 * @property {string} [sha256] - Optional lowercase hex digest; verified after
 *                             download. Best for small files (the whole file is
 *                             read back to hash).
 */


// ── ensure(specs, opts) ───────────────────────────────────────────────────────

/**
 * Make every spec present locally, downloading the missing ones, and return a
 * map of { id: absolute-path } to feed the model loaders. Downloads run
 * sequentially. Honors the HF_TOKEN env var (sent as a bearer token) for gated
 * repos; the in-tree models are public and need none.
 *
 * @param {ModelSpec[]} specs
 * @param {Object}   [opts]
 * @param {Function} [opts.onProgress] - Called with
 *        { id, received, total, cached? }. `total` is 0 when the server sends
 *        no content-length and the spec carries no `bytes`. `cached:true` fires
 *        once for an already-present file (no download).
 * @returns {Promise<Object<string,string>>} resolves to { [id]: path }
 * @throws if a spec lacks id/repo/file, on HTTP failure, or on a size/sha
 *         mismatch (the partial .part file is removed).
 *
 * @example
 * const manifest = await (await fetch('bro.json')).json();
 * const p = await bro.models.ensure(manifest.models, {
 *   onProgress: e => { if (e.total) meter.style.width = (100*e.received/e.total)|0 + '%'; },
 * });
 * bro.lm.loadQwen(p['qwen.gguf'], { onReady, onError });
 * bro.wake.listen({ weights: p['wake'], onFire });
 */
function ensure(specs, opts) {}


// ── resolve(spec) ─────────────────────────────────────────────────────────────

/**
 * Where a spec's file is / would be, without downloading. Order: an existing
 * cache hit, then an existing `dev` sibling, else the (not-yet-present) cache
 * path. Useful to check on-disk state or build a loader path without fetching.
 *
 * @param {ModelSpec} spec
 * @returns {string} absolute path
 */
function resolve(spec) {}


// ── cacheDir() ────────────────────────────────────────────────────────────────

/**
 * The base model-cache directory (honors BRO_MODELS_DIR). Files live under
 * <cacheDir>/<repo>/<file>.
 * @returns {string}
 */
function cacheDir() {}


// ── CLI prefetch ──────────────────────────────────────────────────────────────
//
// Download an app's declared models ahead of time (e.g. to bake an offline /
// air-gapped install, or seed the cache during packaging) without launching the
// app's UI:
//
//   bro --fetch <appDir>            # windowed binary, headless + no-GPU for the fetch
//   bro-headless --fetch <appDir>   # same, via the headless tool
//
// Reads <appDir>/bro.json "models", downloads anything missing into the cache
// with per-file progress on stdout, and exits 0 on success / 1 on error.
