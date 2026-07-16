# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Apps live in a sibling repo** — `../broworkshop/` holds the launcher and starter apps (`games/`, `tools/`, `demos/`, `ai/`). bro is the runtime only; no apps are bundled here. Run any app by passing its directory to `bro` or `bro-headless`.

**Naked `bro` opens the built-in project manager** at `system/projects/` (the no-args fallback in `src/main.cpp`). New projects are seeded from `system/skeletons/<name>/`. Registry persists at the OS user-data dir (`%APPDATA%/bro/projects.json` etc.). See [docs/projects.md](docs/projects.md). System-panel scanning (`src/engine/system_panels.cpp`) skips any `system/<dir>/` containing a `bro.json` so these self-contained apps aren't double-loaded as overlay panels.

**Modular build.** `-DBRO_PROFILE=<minimal|app|full>` selects how much is compiled in; individual `-DBRO_WITH_*` flags override it. Default is **`app`** (full renderer — 3D/physics/audio/core game-AI — plus net/video/steam; needs vcpkg for net/video, no AI tower). `minimal` is the 2D/canvas/WebGL + audio floor and builds with **no vcpkg** and no AI tower. `full` adds the AI tower (CUDA stays opt-in via `-DBRO_WITH_TENSOR_CUDA=ON`). A compiled-out feature installs a `{ available: false }` JS stub. See [BUILDING.md](BUILDING.md) and [docs/build-options.md](docs/build-options.md).

**Windows** uses the Visual Studio multi-config generator — one build dir, pick the config at build time:
```bash
cmake -B build                                 # configure (do not use MinGW)
cmake --build build --config Debug
cmake --build build --config Release
./build/Debug/bro.exe ../broworkshop/demos/example
./build/Release/bro.exe ../broworkshop/demos/example          # bro-headless.exe lives alongside
```

**Linux / macOS** use Ninja (single-config) — `--config` is ignored, so use a separate build dir per config:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug        # debug
cmake --build build
./build/bro ../broworkshop/demos/example

cmake -B build-release -DCMAKE_BUILD_TYPE=Release   # release
cmake --build build-release
./build-release/bro ../broworkshop/demos/example
```

For `scripts/package-release.sh` on Linux/macOS, pass `--build-dir build-release` so it picks up the Release binaries (the script's `--config Release` default is the Windows-style config selector and is a no-op for Ninja).

**Common headless invocations** (paths differ per platform as above):
```bash
# Interactive JS REPL (GPU — default)
bro-headless ../broworkshop/demos/example

# JS script file
bro-headless ../broworkshop/demos/example test.js

# Inline JS expression
bro-headless ../broworkshop/demos/example -e "document.querySelector('#btn').click()" -e "screenshot('out.png')"

# CPU-only, no GPU/WebGL
bro-headless --no-gpu ../broworkshop/demos/example
```

Submodules must be initialized: `git submodule update --init`

Skia is a pre-built dependency. On Windows, Linux, and Apple Silicon (arm64) macOS, both the Skia headers/source and the Release library are auto-downloaded from the repo's GitHub releases at configure time — all pinned to one Skia commit (currently `chrome/m147`) so the lib always matches the headers; no manual step needed (`-DBRO_FETCH_SKIA=OFF` disables it). To build it by hand instead — required on Intel (x86_64) macOS, for a Windows Debug lib, or to change the Skia version — run `third_party/skia/build_skia_linux.sh` (Linux) or `third_party/skia/build_skia_mac.sh` (macOS, uses CoreText, freetype/fontconfig off), or clone Skia into `third_party/skia/src`, run `tools/git-sync-deps`, and place the lib in `third_party/skia/lib/{Debug,Release}/`.

On macOS, `tests/run_tests.sh` uses `mapfile` and needs bash 4+ (`brew install bash` → `/opt/homebrew/bin/bash tests/run_tests.sh`); the system `/bin/bash` is 3.2.

## Architecture

Bro is a lightweight app runtime: HTML/CSS/JS apps rendered with GPU acceleration. ~125K LOC of C++20 (the JS binding layer in `src/js/` is over half of it).

**Stack:** QuickJS (JS engine) + qjsbind (C++/JS bindings) + brokit (web/system APIs) + htmlayout (HTML parsing + CSS + layout) + broaudio (audio engine) + bromesh (mesh generation/manipulation) + Jolt (physics) + Skia (rasterization — Ganesh-GL on GPU, CPU raster headless) + SDL3 (windowing, OpenGL contexts, input). All GPU work is OpenGL 3.3 core via glad on SDL_GL contexts — there is no SDL_GPU/D3D12/Metal path.

**Two executables, one Engine:**
- `bro` — windowed app runner (DisplayMode::Windowed)
- `bro-headless` — headless tool with JS scripting (DisplayMode::Headless)

Both share the same `Engine` class configured via `EngineConfig`. Headless defaults to GPU rendering via a hidden SDL window (same pipeline as windowed, including WebGL). Use `--no-gpu` to fall back to `RasterRenderer` (CPU Skia) for environments without a GPU. Headless exposes JS globals (`screenshot()`, `advanceTime()`, `flush()`, `sleep()`, `assert()`) for scripted testing.

### Module dependency graph

```
util  (logging, string helpers — standalone)
  ↑
platform  (SDL3 window, event loop)
  ↑
render  (abstract Renderer interface, SkiaRenderer, RasterRenderer)
  ↑
svg  (SVG rendering via Skia SVG module)
  ↑
layout  (htmlayout adapters, draw traversal, replaced elements)
  ↑
dom  (Document/Element/TextNode tree, events, style proxy)
  ↑
canvas  (Canvas 2D API)    webgl  (WebGL 2.0 context)    scene  (3D scene graph, meshes, terrain, sprites)    physics  (Jolt physics world)
  ↑
js  (QuickJS + qjsbind bindings: DOM, canvas, WebGL, audio, mesh, physics, scene, ML)
  ↑
engine  (orchestrates all subsystems, main loop)
```

The graph is aspirational at the js↔engine edge: a dozen `src/js/*_bindings` files take an `Engine*` (crosshair, gizmo, menu, headless, DOM's `setEngine`), so js and engine are in practice a cycle, wired up in `engine_init.cpp`. scene/canvas stay clean via callbacks and never name dom or engine types.

### Key design patterns

- **Single DOM:** HTML is parsed with gumbo into a `bro::dom` tree. CSS is resolved by `htmlayout::css::Cascade`, layout by `htmlayout::layout::layoutTree()`, and rendering by `DrawTraversal` which walks the tree and issues Skia draw calls.
- **GPU rendering:** three GL contexts in one share group, on three threads. The **main context** composites the frame, runs WebGL and 3D scene-graph rendering. A **UI raster context** (raster thread) owns its own Skia Ganesh-GL `GrDirectContext` and replays the frame's recorded draw commands into a pool of FBO-backed layer surfaces. A single shared **canvas raster context** (canvas worker thread) rasterizes all `CanvasScene` surfaces serially (one-context-per-canvas crashed on Windows/NVIDIA — see `canvas_scene.h`). Handoff between threads is GLsync fences + the lock-free `FrameWorker` CAS state machine (`render/frame_worker.h`). Threading policy: **data plane lock-free, control plane may lock.** The per-frame render handoffs, real-time audio producers (`PcmRing`, mic rings), and JS-thread poll rings stay atomics/snapshots/phase discipline — never a lock on an RT audio thread or in a signal handler. Cold control paths (service command queues in net/steam, the physics phase handshake, canvas worker sync RPC, `FileClock`, `AudioInference` barriers) use mutex+condvar where that is simpler and lets threads block instead of poll. The main thread records HTML paint into a `CommandBuffer` via `RecordingRenderer`; the raster thread replays it and never reads the DOM. The compositor (`engine_compositor.cpp`) then draws a DOM-ordered list of textured quads: HTML layer segments interleaved with canvas/WebGL/scene textures at `LayerBreak` points emitted by `DrawTraversal`.
- **Canvas 2D:** JS calls record Skia-typed `CanvasCmd`s into a `CanvasScene`; the canvas worker replays them onto a per-canvas SkSurface (Ganesh GPU windowed, CPU raster + `glTexSubImage2D` upload otherwise), and the resulting GL texture composites as a layer quad. It is Skia command replay, not vertex batching.
- **Renderer abstraction:** `bro::render::Renderer` is a pure virtual interface for 2D rasterization (CSS-shaped: rrects, gradients, CSS filters, text). `SkiaRenderer` (Ganesh-GL or CPU-with-upload), `RasterRenderer` (pure CPU Skia — headless `--no-gpu`, and the layout thread's text metrics), and `RecordingRenderer` (records to `CommandBuffer` for cross-thread replay) implement it. All use Skia with platform-native font backends (DirectWrite on Windows, FreeType/fontconfig on Linux) for accurate text metrics. 3D scene, WebGL, and compositing bypass `Renderer` entirely.
- **Event flow:** SDL event → `EventLoop` → `Engine::handleMouse*/Key*()` → hit test via `htmlayout::layout::hitTest()` → create `MouseEvent`/`KeyboardEvent` → `js::dispatchDomEvent()` (`src/js/event_dispatch.cpp`) — a full three-phase DOM dispatch: window capture → ancestor capture → target → bubble → window bubble, with shadow-boundary retargeting and `composedPath()`. Key events also dispatch `"action"` events if the key is bound to a named action via `bro.settings`.
- **Dirty tracking:** DOM mutations call `document_->markDirty()`. Main loop only re-layouts when `isDirty()` is true.
- **Settings system:** Three-layer priority (engine defaults < app overrides < user overrides). `Settings` class in `engine/settings.h` manages resolution, persistence (`.bro_settings.json`), and runtime change callbacks. JS API exposed as `bro.settings.*`. See `docs/settings.md`.
- **Virtual time in headless:** `advanceTime(ms)` manually ticks timers without real delays, enabling deterministic testing.
- **JS lifetime:** QuickJS context must outlive all DOM elements (they hold JS function references).

### Headless JS interface

Headless mode is driven by JavaScript — the same language apps are written in. Three invocation modes: JS REPL (interactive), script file (`test.js`), or inline expressions (`-e "expr"`). Headless-specific globals: `screenshot(path)`, `advanceTime(ms)`, `flush()`, `sleep(ms)`, `assert(cond, msg?)`. All standard DOM APIs work (`querySelector`, `.click()`, `.textContent`, `getBoundingClientRect()`, etc.). See `docs/headless.md` for full reference.

## Third-party dependencies (in third_party/)

| Library | Target | Notes |
|---------|--------|-------|
| QuickJS | `qjs` | JS engine, built as library (submodule) |
| qjsbind | `qjsbind` | Header-only C++20 QuickJS binding library (standalone or submodule) |
| bromath | `bromath` | Header-only math: Vec/Quat/Mat, Color, AABB, easing — used transitively by most siblings (standalone or submodule) |
| brokit | `brokit` | Web-standard + system APIs (fetch, streams, storage, fs, crypto, events) (standalone or submodule) |
| htmlayout | `htmlayout` | HTML5 parsing (gumbo), CSS parsing, selector matching, style cascade, layout (standalone or submodule) |
| broaudio | `broaudio` | Real-time audio engine (synthesis, effects, spatial, MIDI, mixing) (standalone or submodule) |
| bromesh | `bromesh` | Mesh generation, manipulation, analysis, and I/O (standalone or submodule) |
| broflora | `broflora` | Ecosystem simulation (Makowski et al. "Synthetic Silviculture"): plants, foliage, blooms (standalone or submodule) |
| brotensor | `brotensor` | Tensor + ops — one unified `brotensor::Tensor` (runtime `Device` tag), device-neutral ops behind a flat `brotensor::` namespace. Owns the full **training** surface, not just inference: forward + backward ops (e.g. flash-attention backward), losses, and optimizers — the model siblings above run inference, but brotensor itself trains models in C++ (e.g. `wake_train.cpp`). CPU backend always built; CUDA + Metal additive/opt-in. Hard dependency of brogameagent (owns the Tensor type + CPU ops); loaded transitively |
| brogameagent | `brogameagent` | Game AI: navmesh, pathfinding, steering, perception (standalone or submodule) |
| brolm | `brolm` | Language/text-model inference: BPE + Unigram tokenizers, transformer text encoders (CLIP, T5), CLIP vision encoder + scorer. The text frontend brodiffusion consumes. Depends on bromath + brotensor + broimage (standalone or submodule) |
| brodiffusion | `brodiffusion` | Diffusion-model text-to-image inference: U-Net + VAE, DDIM/LCM schedulers, LoRA, INT8. Text encoders come from brolm. CPU FP32 by default; CUDA/Metal additive. Depends on bromath + brotensor + brolm + broimage (standalone or submodule) |
| broimage | `broimage::broimage` | Image decode/encode (stb) + composable kernels (reduce/map/combine/lookup/stencil/resample/gradient), geometric (resize/crop/letterbox/flip/rotate), alpha-correct ops, color/HSV/sRGB, normalization with CLIP/ImageNet/SAM presets, NHWC↔NCHW preproc. Backs the **CPU** `bro.image` JS kernels (via brokit) and host-side preprocessing in brolm/brodiffusion. CPU-only + brotensor (CUDA/Metal) compute for GPU tensors; it has **no WebGL** — the `bro.image.gpu.*` WebGL2 renderer is a bro-side module (`src/js/js/image_gpu.js`), not broimage. Depends on bromath + brotensor (standalone or submodule) |
| brosoundml | `brosoundml` | Audio-ML model inference (TTS / STT / neural codec) composed from brotensor's FP32 audio op family (FFT/STFT, 1D conv, vocoder/codec activations, resampling, AR sampling). Depends on brotensor (standalone or submodule) |
| brovisionml | `brovisionml::brovisionml` | Vision-model inference: SAM segmentation, Depth-Anything-V2 depth, DSINE surface normals, BiRefNet background removal (Swin-L + ASPP-deformable), and the ControlNet conditioning annotators (HED, lineart, MLSD, OpenPose, SegFormer). image→X tasks, no tokenizer. Composes brotensor ops + broimage preprocessing; ships its own DSINE CUDA kernels. Depends on bromath + brotensor + broimage (standalone or submodule) |
| Jolt Physics | `Jolt::Jolt` | Rigid body physics engine (submodule) |
| SDL3 | `SDL3::SDL3` | Windowing, input, GPU display (submodule, static) |
| Skia | `skia` (imported) | Pre-built 2D rasterization binaries, auto-detected |
| glad | `glad` | OpenGL 3.3 Core loader for WebGL and scene rendering |
| stb_image | `stb_image` | Image loading and writing (stb_image.h, stb_image_write.h) |
| FastNoise2 | `FastNoise` | SIMD noise generation (via brokit) |

## JS API Documentation (in docs/)

API references are written as annotated `.js` files with JSDoc comments and usage examples:

| File | API Surface |
|------|-------------|
| `docs/audio-api.js` | `AudioContext`, Web Audio-inspired nodes, synth, sequencing, spatial, mix buses |
| `docs/mesh-api.js` | `bro.mesh` — primitives, CSG, simplification, UV, import/export |
| `docs/flora-api.js` | `bro.flora` — broflora ecosystem sim: prototypes, plants, step, mesh/segment/foliage/bloom emit |
| `docs/math-api.js` | `bro.math` — bromath types surfaced to JS (currently `SpatialHash3D`) |
| `docs/noise-api.js` | `bro.noise` — FastNoise2 SIMD noise generation |
| `docs/image-api.js` | `bro.image` — composable typed-array kernels (reduce, map, combine, lookup, stencil, resample), backed by broimage C++ (via brokit). Plus `bro.image.gpu.{colormap,fbm2D}` — a bro-side WebGL2 renderer that shares the namespace but is **not** broimage (renders to a canvas; lives in `src/js/js/image_gpu.js`) |
| `docs/imagebitmap-api.js` | `ImageBitmap` / `createImageBitmap` — immutable pixels-to-drawable primitive; `drawImage` + WebGL `texImage2D` source; zero-copy Worker transfer |
| `docs/scene-api.js` | `bro.scene` — 3D scene graph, shapes, sprites, meshes, physics nodes |
| `docs/lighting-api.js` | PBR lighting — LightNode (dir/point/spot), materials, tonemap, ambient |
| `docs/net-api.js` | `bro.net` — game networking (host/connect/send/broadcast) via GNS |
| `docs/crosshair-api.js` | `bro.crosshair` — engine-level crosshair overlay (cross/dot/circle/crossdot) |
| `docs/gamepad-api.js` | Gamepad API — W3C-style `navigator.getGamepads()` snapshots (standard mapping over SDL3), `gamepadconnected`/`gamepaddisconnected` window events, dual-rumble `vibrationActuator`, `"gamepad:<button>"` bindings in `bro.settings` actions, headless virtual-pad injection |
| `docs/wake-api.js` | `bro.wake` — streaming wake-word detection (brosoundml::WakeWord) driven by broaudio's mic tap |
| `docs/kws-api.js` | `bro.kws` — open-vocabulary streaming keyword spotting (brosoundml::PhonemeSpotter): enroll phrases from `bro.tts.phonemize` ids or reference audio, named onSpot events off the mic tap |
| `docs/mic-api.js` | `bro.mic` — live mic chunk consumer: fixed-size frames (chunkFrames) from broaudio's mic tap, per-chunk peak/RMS, resample + AGC |
| `docs/sense-api.js` | `bro.sense` — tier-0 acoustic sensor bus (brosoundml::SensorHub): model-free per-frame level/VAD, spectral-flux onset, autocorrelation tonality off the mic tap; poll-only lock-free snapshot with monotonic event counters |
| `docs/gesture-api.js` | `bro.gesture` — open-vocabulary NON-speech gesture matching (brosoundml::GestureSpotter): enroll rhythm (onset-interval) and tone (sustained-pitch) gestures by example over the shared SensorHub stream, named onGesture events — the tier-0 path for clicks/taps/whistles the speech model can't represent. Needs bro.sense active |
| `docs/listen-api.js` | `bro.listen` — open N concurrent, unmixed listening streams: each `open(source)` (mic / whole-system loopback / one app's audio by pid) is its own 16 kHz ring + PCEN mel front-end with up to one each of {sense, kws, wake, gesture} attached; model weights load once and are shared across streams. The bro.kws/wake/sense/gesture globals target the implicit default-mic stream |
| `docs/worker-api.js` | `Worker` — web worker threads |
| `docs/ai-game-api.js` | `bro.ai.game` — navmesh, pathfinding, steering, perception, capabilities, AgentBinding |
| `docs/gpu-api.js` | `bro.gpu` — always-present runtime backend probe (brotensor): `available`, `backend` ('cuda'/'metal'/'cpu'), `devices`. Honest signal for "will an ML model run on GPU or fall back to CPU" — gate large-model loads on it |
| `docs/tensor-api.js` | `bro.tensor` — GPU tensor + ops (brotensor sibling, CUDA / Metal): GpuTensor, dense/elementwise/softmax/layernorm/attention/MHA/embedding/concat/optim ops, batched inference variants |
| `docs/diffusion-api.js` | `bro.diffusion` — diffusion-model inference (brodiffusion sibling): Pipeline, one-shot generate, step-wise prime/stepOnce/decode, cross-attention trace + logit-bias steering, LoRA |
| `docs/lm-api.js` | `bro.lm` — text generation (brolm sibling): Qwen3 (loadQwen, GGUF + ChatML), Mistral 3.1 (loadMistral, GGUF + tekken tokenizer + [INST] template; same LMModel surface, model.family tells them apart), and Qwen3.5 (loadQwen35, safetensors dir via the VLM driver — string prompts, driver-owned tokenizer). KV cache, chat templating, sampled/greedy generate, shared async bro.lm.generate with streaming + cancel. CUDA by default |
| `docs/stt-api.js` | `bro.stt` — speech-to-text (brosoundml sibling): Whisper (loadWhisper + loadTokenizer, prompted, 30 s windows), Parakeet-TDT v3 (loadParakeet + loadParakeetTokenizer, unconditional, per-token timestamps via tokenFrames), and Qwen3-ASR (loadQwenAsr, 52-language + language ID, context biasing, encoder latent tap + loadQwenAsrStream incremental mic-feed encoder; detokenize via bro.lm.loadTokenizer). 16 kHz mono audio in, transcribe → token ids → text. CUDA by default |
| `docs/diar-api.js` | `bro.diar` — speaker diarization (brosoundml sibling): streaming Sortformer (loadSortformer, nvidia/diar_streaming_sortformer_4spk-v2.1) → per-80 ms-frame activity probabilities for up to 4 arrival-ordered speakers. Offline diarize() + streaming createSession()/feed(window, isLast) over an Arrival-Order Speaker Cache. PLUS ClusterDiarizer (loadClusterDiarizer + clusterDiarize) — Sortformer VAD + ECAPA x-vectors + centered-cosine clustering for telling apart acoustically SIMILAR voices Sortformer's 4-slot head collapses; tunable clusterThreshold, discovers speaker count. 16 kHz mono in. CUDA by default |
| `docs/tts-api.js` | `bro.tts` — text-to-speech (brosoundml sibling): Kokoro (phonemize, loadKokoro + loadVoice, synthesize phoneme ids) and Qwen3-TTS (loadQwen, text-driven, preset CustomVoice speakers) → 24 kHz mono PCM. Async `synthesize` dispatches on model type. CUDA by default |
| `docs/rave-api.js` | `bro.rave` — RAVE neural audio autoencoder (ACIDS/IRCAM, brosoundml sibling): encode a waveform to a PCA-sorted latent (nLatent × frames; dim 0 ≈ loudness, dim 1 ≈ pitch), edit the curves, decode back — deterministic round-trip, optional stochastic noise branch (addNoise + seed) and stereoWidth. Models converted offline from RAVE v2 TorchScript. CUDA by default |
| `docs/vision-api.js` | `bro.vision` — vision-model inference (brovisionml sibling): SAM promptable + automatic segmentation, Depth-Anything-V2 depth, DSINE surface normals, BiRefNet background removal (loadBirefnet → matte + cutout), and the ControlNet annotators (HED softedge, lineart, MLSD lines, OpenPose pose, SegFormer seg). Image in (ImageBitmap / ImageData) → ImageBitmap + typed-array out. Async loaders/inference; CUDA by default |
| `docs/triposplat-api.js` | `bro.triposplat` — single image → 3D Gaussian Splat (VAST-AI/TripoSplat) composed across siblings: DINOv3 (brovisionml) + Flux.2 VAE / flow-matching DiT / octree Gaussian decoder (brodiffusion). `load({dinov3,vae,flow,decoder})` → `pipeline.generate(image, {seed,steps,guidanceScale,numGaussians})` → splat SoA typed arrays for `scene.createGaussianSplat({cloud})`. GPU/FP16 by default |
| `docs/motion-api.js` | `bro.motion` — text-to-motion (nvidia ARDY-G1-RP) composed across siblings: LLM2Vec-Llama-3 text encoder + Llama-3 tokenizer (brolm) + ARDY denoiser / FSQ decoder / AR window rollout / G1 forward kinematics (brodiffusion::ardy). `load({checkpoint,textEncoder})` → `pipeline.generate(text, {frames,steps,cfg,seed,heading})` → per-frame world joint positions + parents + footContacts for the 34-joint Unitree G1 skeleton @ 25 fps (frames round up to 52-frame windows). Sync/blocking — run in a Worker. GPU by default |
| `docs/brokit-api.js` | brokit runtime — Node modules (fs, path, os, child_process) + web globals (fetch, crypto, WebSocket, streams, storage, etc.) |
| `docs/physics-api.js` | `Physics` — Jolt rigid bodies, shapes, raycasts, contact events (paired with PhysicsNode) |
| `docs/terrain-api.js` | `scene.createTerrain` — voxel terrain: noise config, chunk streaming, voxel edits, raycast |
| `docs/tile-api.js` | `scene.createTileWorld` — chunked tile-grid meshing (`bro::tile` core): per-cell tile layers + elevation + flags, flat-top + auto-cliff geometry, corner AO, palette colour, authoring/query. Square and hex topology |
| `docs/dialogs-api.js` | `showOpenFileDialog` / `showOpenFolderDialog` / `showSaveFileDialog` — SDL3-backed native dialogs |
| `docs/menu-api.js` | `bro.menu` — native menu bar (set/addItem/updateItem, item event) |
| `docs/time-api.js` | `bro.time` — global pause + timescale (Godot time_scale/paused analog): `scale`/`paused`/`now` over one engine-owned scaled clock driving timers, rAF, performance.now, CSS transitions, physics, agents, iframes; system panels/audio-rate/Date.now stay wall-clock, pause suspends audio output |
| `docs/gizmo-api.js` | `bro.gizmo` — 3D transform handles (translate/rotate/scale) for scene editing |
| `docs/video-api.js` | `VideoEncoder` (WebM/VP9 software) + `GifEncoder` (GIF89a, median-cut palette) — RGBA in, file out |
| `docs/iframe-api.js` | `<iframe src=dir>` — embedded isolated sub-document (own JS realm/DOM/timers/canvas), rendered at the element box; `src` get/set, `reload()`, `load` event, mouse input routed into the sub-doc |

Other docs: `docs/headless.md` (headless mode), `docs/settings.md` (settings system, including action binding/rebinding), `docs/inspect.md` (DOM inspector that's very useful in headless), `docs/system-panels.md` (system panels: the `__bro` bridge, panel authoring, `PanelLayout`), `docs/multi-repo-workflow.md` (sibling library development), `docs/coverage.md` (OpenCppCoverage line-coverage reports, Windows-only — `pwsh scripts/coverage.ps1` in any repo).

## Namespace

All code is under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::js`, `bro::platform`, `bro::engine`, `bro::layout`, `bro::canvas`, `bro::webgl`, `bro::scene`, `bro::physics`, `bro::svg`.
