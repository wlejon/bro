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
