# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Apps live in `../broworkshop/`** (launcher + starter apps: `games/`, `tools/`, `demos/`, `ai/`). bro is the runtime only — run any app by passing its directory to `bro` or `bro-headless`. Naked `bro` opens the project manager (`system/projects/`); new projects seed from `system/skeletons/<name>/`; registry persists in the OS user-data dir. See [docs/projects.md](docs/projects.md).

**Modular build:** `-DBRO_PROFILE=<minimal|app|full>`, individual `-DBRO_WITH_*` flags override. Default `app` = full renderer + net/video/steam (needs vcpkg), no AI tower. `minimal` = 2D/canvas/WebGL/audio floor, no vcpkg. `full` adds the AI tower (CUDA opt-in via `-DBRO_WITH_TENSOR_CUDA=ON`). Compiled-out features install `{ available: false }` JS stubs. See [BUILDING.md](BUILDING.md), [docs/build-options.md](docs/build-options.md).

Windows (VS multi-config generator — do not use MinGW; one build dir, pick config at build time):
```bash
cmake -B build
cmake --build build --config Release        # or Debug
./build/Release/bro.exe ../broworkshop/demos/example    # bro-headless.exe alongside
```

Linux/macOS (Ninja, single-config — separate build dir per config):
```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
./build-release/bro ../broworkshop/demos/example
```
`scripts/package-release.sh` on Linux/macOS needs `--build-dir build-release` (its `--config` default is the Windows-style selector, a no-op for Ninja).

Headless: `bro-headless <appdir>` (JS REPL), `bro-headless <appdir> test.js`, or `-e "expr"`; `--no-gpu` = CPU-only fallback. Submodules: `git submodule update --init`.

**Skia is pre-built** — headers + Release lib auto-download at configure on Windows/Linux/arm64-macOS, pinned to one Skia commit (`chrome/m147`) so lib always matches headers; `-DBRO_FETCH_SKIA=OFF` disables. Hand-build only for Intel macOS, a Windows Debug lib, or a version change: `third_party/skia/build_skia_{linux,mac}.sh`, lib into `third_party/skia/lib/{Debug,Release}/`.

macOS: `tests/run_tests.sh` needs bash 4+ (`brew install bash`); system bash is 3.2.

## Architecture

Lightweight app runtime: HTML/CSS/JS apps, GPU-accelerated. ~175K LOC C++20 (`src/js/` bindings are over half). Stack: QuickJS + qjsbind + brokit + htmlayout + broaudio + bromesh + Jolt + Skia (Ganesh-GL on GPU, CPU raster otherwise) + SDL3. All GPU work is OpenGL 3.3 core via glad — no SDL_GPU/D3D12/Metal path.

Three executables, one `Engine` (via `EngineConfig.displayMode`): `bro` (windowed), `bro-headless` (JS-scripted; GPU by default through a hidden SDL window — same pipeline incl. WebGL), and `bro-server` (`bro-server <appdir> <script.js>` — dedicated game server, fixed-tickrate JS loop with `bro.net`/`bro.physics`/`bro.mesh`/`bro.noise`, no window or renderer). Headless globals: `screenshot()`, `advanceTime(ms)` (virtual time → deterministic tests), `flush()`, `sleep()`, `assert()`; all standard DOM APIs work. Full reference: [docs/headless.md](docs/headless.md).

Module layering (each names only layers left of it):
```
util → platform (SDL3, event loop) → render (Renderer iface) → svg → layout (htmlayout adapters, DrawTraversal) → dom → canvas | webgl | scene | physics → js (bindings) → engine (main loop)
```
The js↔engine edge is in practice a cycle (a dozen `*_bindings` files take `Engine*`, wired in `engine_init.cpp`); scene/canvas stay clean via callbacks.

Key patterns:
- **Pipeline:** gumbo parse → `bro::dom` tree; `htmlayout::css::Cascade` resolves style, `layoutTree()` lays out, `DrawTraversal` issues Skia calls. Mutations `markDirty()`; the loop re-layouts only when dirty.
- **GPU rendering — three GL contexts, one share group, three threads.** Main context composites and runs WebGL + 3D scene. The raster thread replays the frame's recorded `CommandBuffer` (from `RecordingRenderer` — it never reads the DOM) into FBO layer surfaces with its own `GrDirectContext`. One shared canvas worker rasterizes all `CanvasScene` surfaces serially (per-canvas contexts crashed on Windows/NVIDIA — `canvas_scene.h`). Handoff = GLsync fences + the lock-free `FrameWorker` CAS machine (`render/frame_worker.h`). The compositor (`engine_compositor.cpp`) draws DOM-ordered quads: HTML segments interleaved with canvas/WebGL/scene textures at `LayerBreak` points.
- **Threading policy: data plane lock-free, control plane may lock.** Per-frame handoffs, RT-audio rings, JS-poll rings = atomics/snapshots — never a lock on an RT audio thread. Cold control paths (service command queues, physics phase handshake, canvas sync RPC) use mutex+condvar.
- **Renderer abstraction:** `bro::render::Renderer` is a CSS-shaped 2D interface implemented by `SkiaRenderer`, `RasterRenderer` (pure CPU — headless `--no-gpu` + layout-thread text metrics), `RecordingRenderer`. Native font backends (DirectWrite / FreeType+fontconfig). 3D scene, WebGL, and compositing bypass it.
- **Text shaping:** all text goes through HarfBuzz behind a byte-domain `ShapedRun` (`render/shaped_run.h`), recorded as an `SkTextBlob`; bidi levels resolve via Skia's UAX#9 subset (`render/bidi.h`) and runs reorder into visual order. The shaper's cluster map is what answers htmlayout's caret/selection queries, so carets snap to clusters. HarfBuzz + the ICU bidi subset compile from the Skia source bundle (`third_party/skia/skia_modules.cmake`) — `BRO_WITH_TEXT_SHAPING` defaults ON in *every* profile, minimal included, so there is one text path rather than two. `bro.text` (`src/js/text_bindings.cpp`) exposes the cluster map for diagnostics.
- **Events:** SDL → `EventLoop` → `Engine::handle*` → `hitTest()` → `js::dispatchDomEvent()` (`src/js/event_dispatch.cpp`): full three-phase dispatch with shadow retargeting. Keys also fire `"action"` events via `bro.settings` bindings.
- **Settings:** three-layer (engine < app < user), persisted to `.bro_settings.json`, exposed as `bro.settings.*` — [docs/settings.md](docs/settings.md).
- **JS lifetime:** the QuickJS context must outlive all DOM elements (they hold JS function refs).

## Third-party dependencies (third_party/)

bro-* siblings build from `../<name>` working trees when present, else submodules ([docs/multi-repo-workflow.md](docs/multi-repo-workflow.md)). ML siblings depend on brotensor (+ broimage for preprocessing).

| Library | Target | What |
|---------|--------|------|
| QuickJS | `qjs` | JS engine |
| qjsbind | `qjsbind` | header-only C++20 QuickJS bindings (all bindings go through it) |
| bromath | `bromath` | header-only math: Vec/Quat/Mat, Color, AABB, easing |
| brokit | `brokit` | web/system APIs: fetch, streams, storage, fs, crypto, child_process |
| htmlayout | `htmlayout` | HTML5 parsing (gumbo), CSS cascade/selectors, layout |
| broaudio | `broaudio` | real-time audio engine (synthesis, effects, spatial, MIDI) |
| bromesh | `bromesh` | mesh generation/manipulation/analysis/IO |
| broflora | `broflora` | ecosystem simulation (plants, foliage, blooms) |
| brotensor | `brotensor` | unified Tensor + device-neutral ops incl. full training surface; CPU always, CUDA/Metal opt-in |
| brogameagent | `brogameagent` | game AI: navmesh, pathfinding, steering, perception |
| brolm | `brolm` | text-model inference: tokenizers, CLIP/T5 encoders, LLMs |
| brodiffusion | `brodiffusion` | diffusion text-to-image: U-Net/VAE, schedulers, LoRA |
| broimage | `broimage::broimage` | image decode/encode + CPU kernels + ML preprocessing (no WebGL — `bro.image.gpu` is bro-side JS) |
| brosoundml | `brosoundml` | audio-ML inference: TTS/STT/diarization/codec/wake |
| brovisionml | `brovisionml::brovisionml` | vision-ML inference: SAM, depth, normals, matting, ControlNet annotators |
| Jolt Physics | `Jolt::Jolt` | rigid-body physics |
| SDL3 | `SDL3::SDL3` | windowing, input (static) |
| Skia | `skia` (imported) | pre-built 2D rasterization |
| glad | `glad` | OpenGL 3.3 core loader |
| stb_image / FastNoise2 | `stb_image` / `FastNoise` | image IO / SIMD noise |

## JS API Documentation (docs/)

Annotated `.js` files with JSDoc + examples — read the file before using or changing an API; don't invent shapes from this table. ML namespaces (`bro.lm/stt/tts/diar/rave/vision/diffusion/tensor/triposplat/motion`) are CUDA-by-default; gate big loads on `bro.gpu`.

| File | Surface |
|------|---------|
| `audio-api.js` | `AudioContext` — Web-Audio-style nodes, synth, sequencing, spatial, buses |
| `mesh-api.js` | `bro.mesh` — primitives, CSG, simplification, UV, import/export |
| `flora-api.js` | `bro.flora` — ecosystem sim: prototypes, step, mesh/foliage/bloom emit |
| `math-api.js` | `bro.math` — bromath types in JS (`SpatialHash3D`) |
| `noise-api.js` | `bro.noise` — FastNoise2 SIMD noise |
| `image-api.js` | `bro.image` — typed-array kernels (CPU, broimage) + `bro.image.gpu.*` WebGL2 renderer (bro-side) |
| `imagebitmap-api.js` | `ImageBitmap` / `createImageBitmap` — drawImage + texImage2D source, Worker transfer |
| `scene-api.js` | `bro.scene` — 3D scene graph, shapes, sprites, meshes, splats, physics nodes |
| `animation-api.js` | `scene.createAnimationPlayer()` — data-driven keyframe clips for node properties (JSON tracks, events, crossfade) |
| `lighting-api.js` | PBR lighting — LightNode, materials, tonemap, ambient |
| `net-api.js` | `bro.net` — game networking (host/connect/send) via GNS |
| `net-sync-api.js` | `bro.net.sync` — high-level replication + RPCs (pure JS module, host-star) |
| `gamepad-api.js` | Gamepad API — W3C snapshots, rumble, settings action bindings, headless injection |
| `pointer-api.js` | Pointer + Touch Events — capture, compat mouse synthesis, headless touch injection |
| `web-animations-api.js` | `element.animate()` — WAAPI subset on the CSS-transition interpolator |
| `matchmedia-api.js` | `window.matchMedia()` — MediaQueryList, live matches + change events, per-realm |
| `window-api.js` | `bro.window` — borderless, always-on-top, size limits, position, minimize/maximize/restore, displays; + `window.screen`, `window.open`, `navigator.getBattery` |
| `wake-api.js` | `bro.wake` — streaming wake-word detection |
| `kws-api.js` | `bro.kws` — open-vocabulary streaming keyword spotting |
| `mic-api.js` | `bro.mic` — live mic chunks, peak/RMS, resample + AGC |
| `sense-api.js` | `bro.sense` — model-free acoustic sensors (VAD/onset/tonality), poll-only |
| `gesture-api.js` | `bro.gesture` — non-speech gesture matching (rhythm/tone); needs `bro.sense` |
| `listen-api.js` | `bro.listen` — N concurrent unmixed streams (mic / system loopback / per-app) with sense/kws/wake/gesture attach |
| `worker-api.js` | `Worker` — web worker threads |
| `ai-game-api.js` | `bro.ai.game` — navmesh, pathfinding, steering, perception, AgentBinding |
| `gpu-api.js` | `bro.gpu` — runtime backend probe (`available`/`backend`/`devices`/`compiledBackends`) |
| `tensor-api.js` | `bro.tensor` — GPU tensor + ops (dense/attention/optim, batched) |
| `diffusion-api.js` | `bro.diffusion` — text-to-image pipelines, step-wise API, attention trace, LoRA |
| `lm-api.js` | `bro.lm` — text generation: Qwen3/Mistral (GGUF), Qwen3.5 (safetensors); streaming `generate` + cancel |
| `stt-api.js` | `bro.stt` — speech-to-text: Whisper, Parakeet-TDT (timestamps), Qwen3-ASR (+streaming encoder) |
| `diar-api.js` | `bro.diar` — diarization: streaming Sortformer (4 speakers) + ClusterDiarizer (similar voices, discovers count) |
| `tts-api.js` | `bro.tts` — text-to-speech: Kokoro (phoneme), Qwen3-TTS (text) → 24 kHz PCM |
| `rave-api.js` | `bro.rave` — RAVE neural audio autoencoder: encode → edit latents → decode |
| `vision-api.js` | `bro.vision` — SAM, Depth-Anything-V2, DSINE normals, BiRefNet matting, ControlNet annotators |
| `triposplat-api.js` | `bro.triposplat` — single image → 3D Gaussian splat for `createGaussianSplat` |
| `worldgen-api.js` | `bro.worldgen` — learned terrain: infinite deterministic elevation in metres (30 m/cell) |
| `motion-api.js` | `bro.motion` — text-to-motion (G1 skeleton, 25 fps); sync/blocking — run in a Worker |
| `brokit-api.js` | brokit runtime — Node modules (fs, path, os, child_process) + web globals |
| `physics-api.js` | `Physics` — Jolt bodies, shapes, raycasts, contacts, characters, vehicles (wheeled/tracked/motorcycle) |
| `terrain-api.js` | `scene.createTerrain` — voxel terrain: noise, chunk streaming, edits, raycast |
| `tile-api.js` | `scene.createTileWorld` — tile-grid meshing, square + hex, elevation/cliffs/AO |
| `dialogs-api.js` | native file/folder dialogs (blocking — never trigger in tests) |
| `menu-api.js` | `bro.menu` — native menu bar |
| `time-api.js` | `bro.time` — global pause + timescale over one engine-owned scaled clock |
| `gizmo-api.js` | `bro.gizmo` — 3D transform handles |
| `video-api.js` | `<video>` playback (HTMLMediaElement subset, WebM/VP9+Opus) + `VideoEncoder` (WebM/VP9) / `GifEncoder` — RGBA in, file out |
| `iframe-api.js` | `<iframe src=dir>` — isolated sub-document (own realm/DOM/timers), input routed in |

Other docs: `docs/headless.md` (headless reference incl. input/IME injection + WebGL2 support matrix), `docs/settings.md`, `docs/inspect.md` (DOM inspector, great in headless), `docs/system-panels.md`, `docs/multi-repo-workflow.md`, `docs/coverage.md` (Windows-only line coverage).

## Namespace

All code under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::js`, `bro::platform`, `bro::engine`, `bro::layout`, `bro::canvas`, `bro::webgl`, `bro::scene`, `bro::physics`, `bro::svg`.
