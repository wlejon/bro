# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Apps live in a sibling repo** — `../broworkshop/` holds the launcher and starter apps (`games/`, `tools/`, `demos/`, `ai/`). bro is the runtime only; no apps are bundled here. Run any app by passing its directory to `bro` or `bro-headless`.

**Naked `bro` opens the built-in project manager** at `system/projects/` (the no-args fallback in `src/main.cpp`). New projects are seeded from `system/skeletons/<name>/`. Registry persists at the OS user-data dir (`%APPDATA%/bro/projects.json` etc.). See [docs/projects.md](docs/projects.md). System-panel scanning (`src/engine/system_panels.cpp`) skips any `system/<dir>/` containing a `bro.json` so these self-contained apps aren't double-loaded as overlay panels.

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

Skia is a pre-built dependency. On Linux, run `third_party/skia/build_skia_linux.sh`; on macOS, run `third_party/skia/build_skia_mac.sh` (uses CoreText, freetype/fontconfig off). On Windows, build Skia separately and place `skia.lib` in `third_party/skia/lib/{Debug,Release}/`.

On macOS, `tests/run_tests.sh` uses `mapfile` and needs bash 4+ (`brew install bash` → `/opt/homebrew/bin/bash tests/run_tests.sh`); the system `/bin/bash` is 3.2.

## Architecture

Bro is a lightweight app runtime: HTML/CSS/JS apps rendered with GPU acceleration. ~103K LOC of C++20.

**Stack:** QuickJS (JS engine) + qjsbind (C++/JS bindings) + brokit (web/system APIs) + htmlayout (HTML parsing + CSS + layout) + broaudio (audio engine) + bromesh (mesh generation/manipulation) + Jolt (physics) + Skia (raster rendering) + SDL3 (windowing + GPU display)

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
js  (QuickJS + qjsbind bindings: DOM, canvas, WebGL, audio, mesh, physics, scene)
  ↑
engine  (orchestrates all subsystems, main loop)
```

### Key design patterns

- **Single DOM:** HTML is parsed with gumbo into a `bro::dom` tree. CSS is resolved by `htmlayout::css::Cascade`, layout by `htmlayout::layout::layoutTree()`, and rendering by `DrawTraversal` which walks the tree and issues Skia draw calls.
- **GPU rendering:** `GPUContext` owns the `SDL_GPUDevice`, shader pipelines (color + texture), and manages the D3D12 render passes. `SkiaRenderer` rasterizes HTML/CSS to a Skia surface, uploads to a `SDL_GPUTexture` via transfer buffers, and composites as a fullscreen textured quad. Canvas 2D commands are batched into vertex buffers and drawn via the color pipeline.
- **Renderer abstraction:** `bro::render::Renderer` is a pure virtual interface for 2D rasterization. `SkiaRenderer` (GPU-accelerated, windowed) and `RasterRenderer` (CPU Skia with real fonts, headless) both implement it. Both use Skia with platform-native font backends (DirectWrite on Windows, FreeType/fontconfig on Linux) for accurate text metrics.
- **Event flow:** SDL event → `EventLoop` → `Engine::handleMouse*/Key*()` → hit test via `htmlayout::layout::hitTest()` → create `MouseEvent`/`KeyboardEvent` → `dispatchEvent()` with manual bubbling → JS listeners. Key events also dispatch `"action"` events if the key is bound to a named action via `bro.settings`.
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
| brolm | `brolm` | Language/text-model inference: BPE + Unigram tokenizers, transformer text encoders (CLIP, T5), CLIP vision encoder + scorer. The text frontend brodiffusion consumes. Depends on bromath + brotensor (standalone or submodule) |
| brodiffusion | `brodiffusion` | Diffusion-model text-to-image inference: U-Net + VAE, DDIM/LCM schedulers, LoRA, INT8. Text encoders come from brolm. CPU FP32 by default; CUDA/Metal additive. Depends on bromath + brotensor + brolm (standalone or submodule) |
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
| `docs/wake-api.js` | `bro.wake` — streaming wake-word detection (brosoundml::WakeWord) driven by broaudio's mic tap |
| `docs/mic-api.js` | `bro.mic` — live mic chunk consumer: fixed-size frames (chunkFrames) from broaudio's mic tap, per-chunk peak/RMS, resample + AGC |
| `docs/worker-api.js` | `Worker` — web worker threads |
| `docs/ai-game-api.js` | `bro.ai.game` — navmesh, pathfinding, steering, perception, capabilities, AgentBinding |
| `docs/gpu-api.js` | `bro.gpu` — always-present runtime backend probe (brotensor): `available`, `backend` ('cuda'/'metal'/'cpu'), `devices`. Honest signal for "will an ML model run on GPU or fall back to CPU" — gate large-model loads on it |
| `docs/tensor-api.js` | `bro.tensor` — GPU tensor + ops (brotensor sibling, CUDA / Metal): GpuTensor, dense/elementwise/softmax/layernorm/attention/MHA/embedding/concat/optim ops, batched inference variants |
| `docs/diffusion-api.js` | `bro.diffusion` — diffusion-model inference (brodiffusion sibling): Pipeline, one-shot generate, step-wise prime/stepOnce/decode, cross-attention trace + logit-bias steering, LoRA |
| `docs/lm-api.js` | `bro.lm` — Qwen3 text generation (brolm sibling): loadQwen → model + tokenizer, KV cache, chat templating, sampled/greedy generate. CUDA by default |
| `docs/stt-api.js` | `bro.stt` — speech-to-text (brosoundml sibling): Whisper (loadWhisper + loadTokenizer, prompted, 30 s windows) and Parakeet-TDT v3 (loadParakeet + loadParakeetTokenizer, unconditional, per-token timestamps via tokenFrames). 16 kHz mono audio in, transcribe → token ids → text. CUDA by default |
| `docs/tts-api.js` | `bro.tts` — text-to-speech (brosoundml sibling): Kokoro (phonemize, loadKokoro + loadVoice, synthesize phoneme ids) and Qwen3-TTS (loadQwen, text-driven, preset CustomVoice speakers) → 24 kHz mono PCM. Async `synthesize` dispatches on model type. CUDA by default |
| `docs/vision-api.js` | `bro.vision` — vision-model inference (brovisionml sibling): SAM promptable + automatic segmentation, Depth-Anything-V2 depth, DSINE surface normals, and the ControlNet annotators (HED softedge, lineart, MLSD lines, OpenPose pose, SegFormer seg). Image in (ImageBitmap / ImageData) → ImageBitmap + typed-array out. Async loaders/inference; CUDA by default |
| `docs/triposplat-api.js` | `bro.triposplat` — single image → 3D Gaussian Splat (VAST-AI/TripoSplat) composed across siblings: DINOv3 (brovisionml) + Flux.2 VAE / flow-matching DiT / octree Gaussian decoder (brodiffusion). `load({dinov3,vae,flow,decoder})` → `pipeline.generate(image, {seed,steps,guidanceScale,numGaussians})` → splat SoA typed arrays for `scene.createGaussianSplat({cloud})`. GPU/FP16 by default |
| `docs/brokit-api.js` | brokit runtime — Node modules (fs, path, os, child_process) + web globals (fetch, crypto, WebSocket, streams, storage, etc.) |
| `docs/physics-api.js` | `Physics` — Jolt rigid bodies, shapes, raycasts, contact events (paired with PhysicsNode) |
| `docs/terrain-api.js` | `scene.createTerrain` — voxel terrain: noise config, chunk streaming, voxel edits, raycast |
| `docs/dialogs-api.js` | `showOpenFileDialog` / `showOpenFolderDialog` / `showSaveFileDialog` — SDL3-backed native dialogs |
| `docs/menu-api.js` | `bro.menu` — native menu bar (set/addItem/updateItem, item event) |
| `docs/gizmo-api.js` | `bro.gizmo` — 3D transform handles (translate/rotate/scale) for scene editing |
| `docs/video-api.js` | `VideoEncoder` (WebM/VP9 software) + `GifEncoder` (GIF89a, median-cut palette) — RGBA in, file out |

Other docs: `docs/headless.md` (headless mode), `docs/settings.md` (settings system, including action binding/rebinding), `docs/inspect.md` (DOM inspector that's very useful in headless), `docs/multi-repo-workflow.md` (sibling library development), `docs/coverage.md` (OpenCppCoverage line-coverage reports, Windows-only — `pwsh scripts/coverage.ps1` in any repo).

## Namespace

All code is under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::js`, `bro::platform`, `bro::engine`, `bro::layout`, `bro::canvas`, `bro::webgl`, `bro::scene`, `bro::physics`, `bro::svg`.
