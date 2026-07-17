// bro.gpu — runtime GPU-backend probe
// =============================================================================
//
// A thin, always-present view of the brotensor backends registered in this
// build at runtime. Use it to decide whether an ML model will run on a GPU or
// fall back to CPU *before* you load it — large models (LLMs, diffusion U-Nets,
// Whisper) are unbearably slow on CPU, so apps typically warn or gate on this.
//
// Why this is separate from `bro.tensor`:
//   - `bro.tensor` is the GPU tensor/op surface. It compiles OUT to a stub
//     `{ available: false }` when no GPU backend (CUDA/Metal) is built in, and
//     `bro.tensor.available` reflects that *compile-time* decision.
//   - `bro.gpu` is ALWAYS present (brotensor's CPU backend is always linked)
//     and reflects *runtime* reality: a CUDA build still reports CPU here when
//     no CUDA device is actually present at run time. This is the device the ML
//     loaders (bro.lm / bro.stt / bro.tts / bro.vision / bro.diffusion) default
//     to, so it is the honest signal for "will this be slow."
//
// The properties are lazy getters: the CUDA/Metal driver probe runs on first
// access, not at startup, so an app that never touches ML pays nothing.

/**
 * Whether a GPU device is registered and is the default compute device.
 * `false` on a CPU-only build, and also on a GPU build with no usable device
 * (e.g. no CUDA driver / no card present).
 * @type {boolean}
 */
bro.gpu.available;

/**
 * The default compute device — what a freshly-loaded model lands on.
 * One of 'cuda' | 'metal' | 'cpu' (best available: CUDA > Metal > CPU).
 * @type {string}
 */
bro.gpu.backend;

/**
 * Every backend registered in this binary at runtime, e.g. ['cpu'] on a
 * CPU-only build or ['cpu', 'cuda'] when a CUDA device is present. CPU is
 * always included.
 * @type {string[]}
 */
bro.gpu.devices;

/**
 * The tensor backends COMPILED INTO this binary — a static build-time fact from
 * the BRO_WITH_TENSOR_CUDA / _METAL flags, independent of whether a matching GPU
 * is present. 'cpu' is always included; 'cuda'/'metal' appear when built in.
 *
 * This is distinct from `devices` (and `backend`/`available`): those report the
 * runtime device and read ['cpu']/'cpu'/false on a machine with no GPU driver
 * even for a CUDA-capable binary. `compiledBackends` answers "can this build
 * EVER use a GPU," so it's the right signal for build/packaging checks — e.g. a
 * CI smoke test verifying a release binary actually ships the GPU backend on a
 * GPU-less runner, where the runtime probes can't tell.
 * @type {string[]}
 */
bro.gpu.compiledBackends;

/**
 * Device-wide free/total VRAM in bytes for `device` (e.g. cudaMemGetInfo).
 * Lets a loader print a real budget line instead of guessing from nvidia-smi,
 * or gate a large model load on available headroom. Returns `null` when the
 * backend isn't registered or can't report — always `null` for 'cpu'.
 *
 * @param {string} [device=bro.gpu.backend] - 'cuda' | 'metal' | 'cpu'
 * @returns {?{freeBytes: number, totalBytes: number}}
 *
 * @example
 *   const mem = bro.gpu.memoryInfo();
 *   if (mem && mem.freeBytes < 4e9) {
 *     status('Less than 4 GB VRAM free — model may not fit.', 'warn');
 *   }
 */
bro.gpu.memoryInfo(device) {}

/**
 * The card's human-readable name (e.g. `cudaDeviceProp.name`,
 * "NVIDIA GeForce RTX 4090") for `device`. Returns `null` when the backend
 * isn't registered or can't report — always `null` for 'cpu'. Pair with
 * `memoryInfo()` to label a VRAM budget line with the actual card.
 *
 * @param {string} [device=bro.gpu.backend] - 'cuda' | 'metal' | 'cpu'
 * @returns {?string}
 *
 * @example
 *   const card = bro.gpu.deviceName() || bro.gpu.backend.toUpperCase();
 *   const mem = bro.gpu.memoryInfo();
 *   status(card + ' · ' + (mem.totalBytes / 1e9).toFixed(1) + ' GB');
 */
bro.gpu.deviceName(device) {}

/**
 * Return the backend allocator's cached-but-unused memory to the driver,
 * keeping at most `keepBytes` cached. Synchronizes the device first so
 * stream-ordered frees are actually reclaimable.
 *
 * Call this between pipeline phases with very different scratch shapes (e.g.
 * switching from a diffusion U-Net to a VAE decode, or between successive
 * model loads): cached blocks count against device residency, and on Windows
 * (WDDM) sustained near-full commit makes the OS silently demote large
 * resident allocations to shared memory — turning what should be a VRAM
 * weight read into PCIe traffic. Returns `false` when the backend isn't
 * registered or has no trimmable allocator — always `false` for 'cpu'.
 *
 * @param {string} [device=bro.gpu.backend] - 'cuda' | 'metal' | 'cpu'
 * @param {number} [keepBytes=0] - bytes to keep cached
 * @returns {boolean}
 *
 * @example
 *   const unet = bro.diffusion.loadModel(unetDir);
 *   // ... run denoise steps ...
 *   bro.gpu.trim(); // release U-Net scratch before the VAE decode allocates
 */
bro.gpu.trim(device, keepBytes) {}

// --- Typical use: warn before loading a large model on CPU -------------------

if (!bro.gpu.available) {
  // Non-blocking warning — the model still loads and runs, just slowly.
  status('No GPU detected (' + bro.gpu.backend + ') — loading on CPU will be '
       + 'slow. Continue?', 'warn');
}

// Drive a backend badge honestly in any build:
const badge = document.querySelector('#backend');
badge.textContent = bro.gpu.backend.toUpperCase();      // 'CUDA' | 'METAL' | 'CPU'
badge.className = 'badge ' + (bro.gpu.available ? 'ok' : 'bad');

// Confirm-gate a heavy load:
function maybeLoad(loadFn) {
  if (!bro.gpu.available &&
      !confirm('This model will run on CPU and may take minutes. Load anyway?')) {
    return;
  }
  loadFn();
}
