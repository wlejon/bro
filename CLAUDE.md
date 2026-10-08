# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Apps live in `../broworkshop/`** (launcher + starter apps: `games/`, `tools/`, `demos/`, `ai/`). bro is the runtime only; run any app by passing its directory to `bro` or `bro-headless`. Naked `bro` opens the project manager (`system/projects/`); new projects seed from `system/skeletons/<name>/`; registry persists in the OS user-data dir. See [docs/projects.md](docs/projects.md).

**Modular build:** `-DBRO_PROFILE=<minimal|app|full>`, individual `-DBRO_WITH_*` flags override. Default `app` = full renderer + net/video/steam (needs vcpkg), no AI tower. `minimal` = 2D/canvas/WebGL/audio floor, no vcpkg. `full` adds the AI tower (CUDA opt-in via `-DBRO_WITH_TENSOR_CUDA=ON`). Compiled-out features install `{ available: false }` JS stubs. See [BUILDING.md](BUILDING.md), [docs/build-options.md](docs/build-options.md).

Windows (VS multi-config generator; do not use MinGW, and use one build dir picking the config at build time):
```bash
cmake -B build
cmake --build build --config Release        # or Debug
./build/Release/bro.exe ../broworkshop/demos/example    # bro-headless.exe alongside
```

Linux/macOS (Ninja, single-config, so use a separate build dir per config):
```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
./build-release/bro ../broworkshop/demos/example
```
`scripts/package-release.sh` on Linux/macOS needs `--build-dir build-release` (its `--config` default is the Windows-style selector, a no-op for Ninja).

Headless: `bro-headless <appdir> test.js` or `bro-headless <appdir> -e "expr"`; the script is compiled in-process by bronze and run against the engine. There is no REPL. `--no-gpu` = Skia on the CPU, no WebGL/3D scene. Submodules: `git submodule update --init`.

**Skia is pre-built.** Headers + Release lib auto-download at configure on Windows/Linux/arm64-macOS, pinned to one Skia commit (`chrome/m147`) so the lib always matches the headers; `-DBRO_FETCH_SKIA=OFF` disables. Hand-build only for Intel macOS, a Windows Debug lib, or a version change: `third_party/skia/build_skia_{linux,mac}.sh` / `build_skia_windows.ps1`, lib into `third_party/skia/lib/{Debug,Release}/`.

macOS: `tests/run_tests.sh` needs bash 4+ (`brew install bash`); system bash is 3.2.

## Architecture

Lightweight app runtime: HTML/CSS apps, GPU-accelerated. C++20 under `src/`. Stack: brokit + htmlayout + broaudio + bromesh + Jolt + Skia + SDL3 + Vulkan 1.3 Core (dynamic rendering, timeline semaphores). Vulkan is the only graphics API (MoltenVK on macOS): Skia (Ganesh-Vulkan), WebGL2 and the 3D scene all render on one device, GLSL compiles to SPIR-V in process with glslang, and headless renders offscreen with no X server.

Three executables, one `Engine` (via `EngineConfig.displayMode`): `bro` (windowed), `bro-headless` (the same Vulkan pipeline, offscreen), and `bro-server` (`bro-server <appdir>`, a dedicated game server running with `bro.net`/`bro.physics`/`bro.mesh`/`FastNoise`, no window or renderer). Headless functions: `screenshot()`, `advanceTime(ms)` (virtual time, for deterministic tests), `flush()`, `sleep()`, `assert()`; all standard DOM APIs work. Full reference: [docs/headless.md](docs/headless.md).

Module layering (each names only layers left of it):
```
util → platform (SDL3, event loop) → render (Renderer iface) → svg → layout (htmlayout adapters, DrawTraversal) → dom → canvas | webgl | scene | physics → engine (main loop)
```
`src/svg` is only the `<img src="*.svg">` rasterizer (SkSVGDOM into an RGBA buffer); *inline* `<svg>` is painted by `src/layout/svg_*` — a native traversal emitting `Renderer` primitives with cascaded SVG paint, so SVG children have real `getBoundingClientRect` geometry, falling back to SkSVGDOM only for text/filters/masks/patterns/markers.

`src/bronze_host/` exposes the engine to the [bronze](https://github.com/wlejon/bronze) JavaScript runtime and AOT compiler (backed by [brass](https://github.com/wlejon/brass)). An app is a folder carrying `app.dll`/`.so`/`.dylib` beside its `index.html`, which the stock `bro`/`bro-headless` load; a folder without one has its `<script>` tags compiled in-process by bronze at boot. bronze resolves as `../bronze` first, `third_party/bronze` (submodule) second, and brass resolves as `../brass` / `third_party/brass`. bro's own natives (scene, physics, net, time, window, settings, ...) live under `natives/<sub>/` (prototypes `native_<sub>_decl.h`, registrations `native_<sub>_register.cpp`, JS wrapper `<sub>.js`, `module.globals`) with bodies in `native_*.cpp`, all hand-maintained — adding a native means editing all of them together; every sibling library's JS API is the sibling's own `<name>_api` library, linked per feature flag in `src/bronze_host/CMakeLists.txt` and installed exactly once per realm by `installSiblingApis` (`host_sibling_apis.cpp`) — never call a sibling `install*()` anywhere else. See `src/bronze_host/README.md` and `tests/bronze_host/README.md`.

Key patterns:
- **Pipeline:** gumbo parses into a `bro::dom` tree; `htmlayout::css::Cascade` resolves style, `layoutTree()` lays out, `DrawTraversal` issues Skia calls. Mutations `markDirty()`; the loop re-layouts only when dirty. A geometry read lays the document out first — `Engine::flushLayoutForRead` — so an element appended and measured in one turn measures correctly rather than reporting the box it does not have yet. The flush re-arms the *paint* half of the dirty flag, because the frame still has to draw what was measured; `Document::layoutIsCurrent()` keeps a run of reads to one pass.
- **GPU rendering & presentation (Vulkan):** `VulkanContext` (device, queue), `VulkanSwapchain` (windowed) and `VulkanPresenter`, which composites every layer of a frame (UI, iframes, 2D canvases, WebGL, 3D scene) by sampling each layer's `VkImage` in place, placed/clipped by the typed layer handles `DrawTraversal` records. Skia draws on the GPU through `SkiaGpu` (Ganesh on the shared device, submissions routed through `VulkanQueue`; `BRO_SKIA_GPU=0` keeps it on the CPU for comparison). The 3D scene is a pass graph under `src/scene/vulkan/` (shadows, environment/sky, PBR mesh, post-fx); WebGL2 is `src/webgl/` (`WebGLVkContext`). Shaders are GLSL compiled with glslang (`render/glsl_compiler.h`): built-in scene shaders at build time, WebGL and custom shaders at run time; pipelines persist in a `VkPipelineCache` under the user cache dir. `--no-gpu` keeps the same record → replay → composite pipeline with CPU Skia surfaces.
- **GPU frame core (`src/render`):** `VulkanQueue` is the single owner of the graphics/present queue — every `vkQueueSubmit`/`vkQueuePresentKHR` goes through it, each submission returns a timeline *ticket*, and waits are on tickets (never `vkQueueWaitIdle`/`vkDeviceWaitIdle` outside teardown). `VulkanFrames` is the frames-in-flight ring (`context.frames()`): per-slot command buffers, upload arena, descriptor arena, `defer()` for destroying resources once the GPU is done, and frame-end hooks that submit work a consumer keeps open across calls; the engine calls `beginGpuFrame()` once per frame. The 3D scene and WebGL are built on it: anything the CPU writes per render (uniforms, instance data, descriptor sets) comes from the frame's arenas, uploads are copies recorded in the command stream (the scene's upload stream, WebGL's one command buffer), and only readbacks and client waits block — on their own ticket. `vulkan_util.h` has barrier/memory-type helpers; `pixel_convert.h` the RGBA/BGRA copies. Tests run with validation on where the layer exists, and an error fails the test unless its VUID is in `tests/vk_validation_known.txt`.
- **HiDPI:** CSS px are window coordinates everywhere (layout, hit testing, events). `DeviceScale` (`engine/device_scale.h`) carries the render scale (the window's pixel density: 2 on Retina, 1 on Windows/X11) that sizes layer surfaces, the compositor framebuffer and 3D scene targets, while `SkiaRenderer::setDeviceScale` maps CSS-space commands onto them; `devicePixelRatio` / `@media (resolution)` follow it on Apple and in headless (`setDeviceScaleFactor`). Canvas/WebGL backing stays `canvas.width`.
- **Threading policy: data plane lock-free, control plane may lock.** Per-frame handoffs and RT-audio rings use atomics/snapshots, never a lock on an RT audio thread. Cold control paths (service command queues, physics phase handshake, canvas sync RPC) use mutex+condvar.
- **Renderer abstraction:** `bro::render::Renderer` is a CSS-shaped 2D interface implemented by `SkiaRenderer` (GPU or CPU surfaces, `--no-gpu` included), `RasterRenderer` (pure CPU: `bro-server` and layout-thread text metrics), and `RecordingRenderer`. Native font backends (DirectWrite on Windows, FreeType+fontconfig elsewhere). The 3D scene, WebGL, and compositing bypass it.
- **Text shaping:** all text goes through HarfBuzz behind a byte-domain `ShapedRun` (`render/shaped_run.h`), recorded as an `SkTextBlob`; bidi levels resolve via Skia's UAX#9 subset (`render/bidi.h`) and runs reorder into visual order. The shaper's cluster map is what answers htmlayout's caret/selection queries, so carets snap to clusters. HarfBuzz and the ICU bidi subset compile from the Skia source bundle (`third_party/skia/skia_modules.cmake`), and `BRO_WITH_TEXT_SHAPING` defaults ON in *every* profile, minimal included, so there is one text path rather than two.
- **Events:** SDL feeds `EventLoop`, which calls `Engine::handle*`, which runs `hitTest()` and then `dom::dispatchEvent()`: full three-phase dispatch with shadow retargeting.
- **Settings:** three-layer (engine < app < user), persisted to `.bro_settings.json`. See [docs/settings.md](docs/settings.md).

## Third-party dependencies (third_party/)

Every repo in the ecosystem (these, the desktop substrate libraries, the apps) is indexed in [docs/ecosystem.md](docs/ecosystem.md); `scripts/repos.txt` is the machine-readable list tooling reads. bro-* siblings build from `../<name>` working trees when present, else submodules ([docs/multi-repo-workflow.md](docs/multi-repo-workflow.md)). ML siblings depend on brotensor (plus broimage for preprocessing). Every bro-* library below except bromath, htmlayout and the terminal ones (bropty, brosearch, brothemes, bromux) also builds `<name>_api` from its `src/api/` (public header `include/<name>/api.h`; brokit and broflora differ), its bronze JS binding; those siblings depend on bronze + brass with no submodule fallback, so a standalone sibling build needs `../bronze` and `../brass` checked out.

| Library | Target | What |
|---------|--------|------|
| bromath | `bromath` | header-only math: Vec/Quat/Mat, Color, AABB, easing |
| brokit | `brokit` | web/system APIs: fetch, streams, storage, fs, crypto, child_process |
| htmlayout | `htmlayout` | HTML5 parsing (gumbo), CSS cascade/selectors, layout |
| broaudio | `broaudio` | real-time audio engine (synthesis, effects, spatial, MIDI) |
| bromesh | `bromesh` | mesh generation/manipulation/analysis/IO |
| broflora | `broflora` | ecosystem simulation (plants, foliage, blooms) |
| brotensor | `brotensor` | unified Tensor + device-neutral ops including the full training surface; CPU always, CUDA/Metal/Vulkan opt-in |
| brogameagent | `brogameagent` | game AI: navmesh, pathfinding, steering, perception |
| brolm | `brolm` | text-model inference: tokenizers, CLIP/T5 encoders, LLMs |
| brodiffusion | `brodiffusion` | diffusion text-to-image: U-Net/VAE, schedulers, LoRA |
| broimage | `broimage::broimage` | image decode/encode + CPU kernels + ML preprocessing (no WebGL; `bro.image.gpu` is bro-side JS) |
| brosoundml | `brosoundml` | audio-ML inference: TTS/STT/diarization/codec/wake |
| brovisionml | `brovisionml::brovisionml` | vision-ML inference: SAM, depth, normals, matting, ControlNet annotators |
| bropty | `bropty` | VT emulator + PTY/ConPTY behind `<terminal>` (`BRO_WITH_TERMINAL`; `src/terminal/`) |
| brosearch | `brosearch` | regex scrollback search (bropty's dependency) |
| brothemes | `brothemes` | colour schemes + WCAG/APCA contrast (the terminal's minimum contrast) |
| bromux | `bromux` | terminal multiplexer: the server behind persistent `<terminal>` sessions (optional; off when absent) |
| broremote | `broremote` | remote sessions behind `bro.remote`: frames to a viewer (VA-API video), its input back (`BRO_WITH_REMOTE`; `../broremote` only, no submodule; off when absent) |
| brass | `brass` | JIT / AOT native code generator backend for bronze |
| bronze | `bronze` / `bronze-cli` / `bronze::runtime_shared` | JavaScript compiler + shared runtime (mandatory) |
| Jolt Physics | `Jolt::Jolt` | rigid-body physics |
| SDL3 | `SDL3::SDL3` | windowing, input (static) |
| Skia | `skia` (imported) | pre-built 2D rasterization |
| Vulkan SDK | `Vulkan::Vulkan` | Vulkan 1.3 Core graphics & compute loader |
| stb_image / FastNoise2 | `stb_image` / `FastNoise` | image IO / SIMD noise |

## JS API Documentation (docs/)

Annotated `.js` files with JSDoc + examples. Read the file before using or changing an API; don't invent shapes from this table. ML namespaces (`bro.lm/stt/tts/diar/rave/vision/diffusion/tensor/triposplat/motion`) are GPU-by-default (Vulkan, CUDA or Metal); gate big loads on `bro.gpu`.

| File | Surface |
|------|---------|
| `audio-api.js` | `AudioContext` Web-Audio half: nodes, params, buffers, listener, decoding, `getUserMedia`; how it maps onto broaudio |
| `audio-engine-api.js` | `AudioContext` engine half: clips, playbacks, streams, voices, buses + effects, presets; VoiceAllocator, ModMatrix, MidiInput, Sequence |
| `audio-synth-graph-api.js` | `SynthGraph`: procedural voices from a plain-object graph (osc/FM/noise/filter/env/sweep/shaper/resonator/comb/layers), seeded per-trigger jitter, `render()` offline to an AudioBuffer bit-identical to live, `ctx.playSynth` / `releaseSynth`; JIT kernel per shape |
| `ear-api.js` | `bro.ear`: judge a finished clip offline, deterministic: `measure` (timing, LUFS, centroid/flatness, tonality, ringing partials), `compare` to a reference, labelled `spectrogram` images/PNGs; `loadClap` prompt scorer in ML builds |
| `mesh-api.js` | `bro.mesh`: the `Mesh` container, primitives, CSG, simplify/subdivide/smooth, analysis, `MeshBVH` |
| `mesh-io-api.js` | `bro.mesh` IO half: loaders/savers, Draco, splat clouds, isosurface + voxel statics, `PolyMesh`, `SDFGraph` |
| `mesh-plants-api.js` | `bro.mesh` procedural half: sweeps, leaf/flower cards, branch trees, scattering, `CapsuleField`, `LSystem` |
| `rigging-api.js` | `bro.rigging`: skins, skeletons, poses, clips, IK, `Rig.autoRig`, glTF rigged assets |
| `flora-api.js` | `bro.flora` ecosystem sim: prototypes, step, mesh/foliage/bloom emit |
| `math-api.js` | `bro.math`, bromath types in JS (`SpatialHash3D`) |
| `noise-api.js` | global `FastNoise`, FastNoise2 SIMD noise |
| `image-api.js` | `bro.image` typed-array kernels (CPU, broimage): decode/EXIF, encode/KTX2, geometry, color, preproc, presets |
| `image-gpu-api.js` | `bro.image.gpu.*` WebGL2 renderer (bro-side JS): `colormap`, `fbm2D`, ranging, `viewRect` |
| `imagebitmap-api.js` | `ImageBitmap` / `createImageBitmap`: drawImage + texImage2D source, Blob decode, Worker transfer |
| `file-api.js` | `Blob`/`File`, `FileReader`, `URL.createObjectURL` (resolves in `<img>` and `fetch`), dropped-file `dataTransfer` |
| `scene-api.js` | `bro.scene`: `SceneGraph` + `SceneNode` core: hierarchy, transforms, cameras, render settings, raycast/project, capture, `attachTo` |
| `scene-nodes-api.js` | `bro.scene` node types: create* options (mesh/skinned/instanced, splats, shapes, sprites, HTML, lights, particles, decals, probes), `bro.impostor`, per-type SceneNode members (shaders, LOD, instances, skeletal playback) |
| `animation-api.js` | `scene.createAnimationPlayer()`: data-driven keyframe clips for node properties, plus skeletal blend spaces, layered blending, and an authored state machine |
| `lighting-api.js` | PBR lighting: LightNode, materials, tonemap, ambient |
| `net-api.js` | `bro.net`: game networking (host/connect/send) via GNS |
| `net-sync-api.js` | `bro.net.sync`: high-level replication + RPCs (pure JS module, host-star) |
| `gamepad-api.js` | Gamepad API: W3C snapshots, rumble, settings action bindings, headless injection |
| `pointer-api.js` | Pointer + Touch Events: capture, compat mouse synthesis, headless touch injection |
| `web-animations-api.js` | `element.animate()`, a WAAPI subset on the CSS-transition interpolator |
| `matchmedia-api.js` | `window.matchMedia()`: MediaQueryList, live matches + change events, per-realm |
| `window-api.js` | `bro.window`: borderless, always-on-top, size limits, position, minimize/maximize/restore, displays; plus `window.screen`, `window.open`, `navigator.getBattery` |
| `wake-api.js` | `bro.wake`: streaming wake-word detection |
| `kws-api.js` | `bro.kws`: open-vocabulary streaming keyword spotting |
| `mic-api.js` | `bro.mic`: live mic chunks, peak/RMS, resample + AGC |
| `sense-api.js` | `bro.sense`: model-free acoustic sensors (VAD/onset/tonality), poll-only |
| `gesture-api.js` | `bro.gesture`: non-speech gesture matching (rhythm/tone); needs `bro.sense` |
| `listen-api.js` | `bro.listen`: N concurrent unmixed streams (mic / system loopback / per-app) with sense/kws/wake/gesture attach |
| `worker-api.js` | `Worker`, web worker threads |
| `ai-api.js` | `bro.ai`: index of the game-AI surface — which of the files below covers what |
| `ai-game-api.js` | `bro.ai.game` navigation: NavGrid, HexNav, NavMesh bake/load, pathfinding |
| `ai-game-planning.js` | `bro.ai.game`: Agent, World, Unit, steering, perception, AgentBinding |
| `ai-game-learning.js` | `bro.ai.game`: MCTS family, layered planners, team belief, simulation, replay |
| `ai-nn-api.js` | `bro.ai.game.nn`: circuits, nets, ops, `WeightsHandle` (forward/backward/sgdStep/adamStep) |
| `ai-learn-api.js` | `bro.ai.game.learn`: replay buffers, trainers, inference |
| `ai-game-tools.js` | `bro.ai.game.grid`: observation windows, tapes, `GridTrainer` |
| `gpu-api.js` | `bro.gpu`: runtime backend probe (`available`/`backend`/`devices`/`compiledBackends`) |
| `paths-api.js` | `bro.appDir` / `bro.resolvePath`: real filesystem paths, for sidecar binaries and external tools |
| `tensor-api.js` | `bro.tensor` core: `GpuTensor`, RNG, safetensors, dense/elementwise, norms, matmul, RoPE, reductions, optimisers |
| `tensor-nn-api.js` | `bro.tensor` part 2: the attention family, conv2d/3d + NCHW spatial, diffusion sampler steps, INT8/k-quant, audio/codec ops |
| `diffusion-api.js` | `bro.diffusion`: `loadModel`, `generate`, the step-wise `PipelineState` API, LoRA, schedulers |
| `diffusion-control-api.js` | `bro.diffusion` steering: ControlNet, control vectors, Sana identity anchor, krea2 hooks, `VAE`, attention trace |
| `lm-api.js` | `bro.lm` text generation: Qwen3/Mistral (GGUF), Qwen3.5 (safetensors); streaming `generate` + cancel |
| `laya-api.js` | `bro.lm.loadLaya`: Laya decision model (choice/score/noul, calibrated), request scheduler across GPUs, `predictAsync`, stats; what it is good and bad at |
| `stt-api.js` | `bro.stt` speech-to-text: Whisper, Parakeet-TDT (timestamps), Qwen3-ASR (+streaming encoder) |
| `diar-api.js` | `bro.diar` diarization: streaming Sortformer (4 speakers) + ClusterDiarizer (similar voices, discovers count) |
| `tts-api.js` | `bro.tts` text-to-speech: Kokoro (phoneme), Qwen3-TTS (text), OmniVoice (masked-diffusion, prompt clone / instruct, codes seam), to 24 kHz PCM |
| `rave-api.js` | `bro.rave` RAVE neural audio autoencoder: encode, edit latents, decode |
| `vision-api.js` | `bro.vision`: SAM, Depth-Anything-V2, DSINE normals, BiRefNet matting, ControlNet annotators |
| `triposplat-api.js` | `bro.triposplat`: single image to a 3D Gaussian splat for `createGaussianSplat` |
| `motion-api.js` | `bro.motion` text-to-motion (G1 skeleton, 25 fps); sync/blocking, so run it in a Worker |
| `brokit-api.js` | brokit runtime: Node modules (fs, path, os, child_process) + web globals |
| `physics-api.js` | `Physics`: Jolt bodies, shapes, raycasts, contacts, characters, vehicles (wheeled/tracked/motorcycle) |
| `terrain-api.js` | `scene.createTerrain`: chunked height-field terrain (one height per column, not voxels): noise, chunk streaming, edits, raycast |
| `clipmap-api.js` | `scene.createClipmapTerrain`: camera-centred clipmap, fixed ring geometry, GPU displacement from a streamed height pyramid |
| `tile-api.js` | `scene.createTileWorld`: tile-grid meshing, square + hex, elevation/cliffs/AO |
| `dialogs-api.js` | native file/folder dialogs (blocking, so never trigger them in tests) |
| `menu-api.js` | `bro.menu`: native menu bar |
| `time-api.js` | `bro.time`: global pause + timescale over one engine-owned scaled clock |
| `profiler-api.js` | `bro.profiler`: bronze's sampling profiler from script: `start({hz, threads})` / `stop({callers, report})` → per-function self/total + tier (interpreter / tier 1 / tier 2 / aot / native), caller edges, text; zero cost while stopped |
| `gizmo-api.js` | `bro.gizmo`: 3D transform handles |
| `video-api.js` | `<video>` playback (HTMLMediaElement subset, WebM/VP9+Opus) incl. `stepFrame`/`frameRate`, `bro.media` waveform + filmstrip analysis, `VideoEncoder` (WebM/VP9) / `GifEncoder`: RGBA in, file out |
| `iframe-api.js` | `<iframe src=dir>`: isolated sub-document (own realm/DOM/timers), input routed in |
| `terminal-api.js` | `<terminal>` (HTMLTerminalElement) + `bro.terminal`: native terminal element over bropty; spawn/write/feed/kill, screen/scrollback/frame text, key/IME/paste routing; mouse selection + reporting, scrollback view, search, links, OSC events (title/cwd/bell/notification/progress/OSC 52/OSC 133 commands), options + theme (CSS `--terminal-*`), own compositor layer |
| `conf-api.js` | `bro.conf`: layered desktop settings store with schemas, defaults, validation, and watcher handles |
| `themes-api.js` | `bro.themes`: colour schemes + WCAG/APCA contrast, color spaces, theme import/export |
| `keys-api.js` | `bro.keys`: keybinding engine with chords, sequences, when-clause evaluation, and VS Code json |
| `search-api.js` | `bro.search`: fzf-compatible fuzzy matching, ripgrep-style directory walk and regex grep |
| `apps-api.js` | `bro.apps`: desktop app catalog, execution, FreeDesktop icons, MIME associations, and recent files |
| `vfs-api.js` | `bro.vfs`: async file ops (copy/move/remove), XDG trash + undo/redo, watchers, and directory models |
| `thumb-api.js` | `bro.thumb`: FreeDesktop thumbnailer cache and generator with dimensions and format sniffing |
| `seat-api.js` | `bro.seat`: session state, VT switching, lock screen events, and idle inhibition |
| `cred-api.js` | `bro.cred`: lock screen authentication, PAM, biometrics (fprintd), Secret Service, and polkit agent |
| `sys-api.js` | `bro.sys`: power/battery, PipeWire audio, NetworkManager Wi-Fi/Ethernet, Bluetooth, notifications, and StatusNotifierItem tray |
| `displays-api.js` | `bro.displays`: display snapshot, test-then-revert configuration, night light, and backlight brightness |
| `portal-api.js` | `bro.portal`: XDG Desktop Portal backends for file chooser, screenshot, screencast, and open URI |
| `compositor-api.js` | `bro.compositor`: window-management policy, workspace switching, tiling/floating layout modes, and event hooks |
| `wl-api.js` | `bro.wl`: Wayland client protocols: wlr-layer-shell panels/docks, foreign-toplevel, session lock, and screencopy |
| `a11y-api.js` | `bro.a11y`: accessibility tree inspection, node query/mutation, live-region announcements, and custom widget roles |
| `remote-api.js` | `bro.remote`: host this screen for `broremote-view` (local or over ssh): `host({socket, codecs, bitrateKbps, fps})` / `stop` / `status` / attach + detach events; frames from the presenter (KMS dmabuf under DRM), viewer input routed as local input |

Other docs: `docs/headless.md` (headless reference including input/IME injection and the WebGL2 support matrix), `docs/settings.md`, `docs/inspect.md` (DOM inspector, great in headless), `docs/agent-control.md` (`bro-ctl`: the local control socket — screenshots, vblank-stamped recordings, input, eval, DOM, the frame-pacing flight recorder, perf profiles; on by default under `--drm`), `docs/system-panels.md`, `docs/embedding.md` (linking bro_engine into your own executable: media backends, the headless driver), `docs/hot-reload.md` (the edit loop: source watcher, F5, `BRO_JIT_TIER` override), `docs/code-cache.md` (on-disk compiled-IL cache: key, location, `BRO_CODE_CACHE*`), `docs/compile-progress.md` (window stays live during a compile; `bro-compiling` / `--bro-compile-progress` on `<html>`), `docs/multi-repo-workflow.md`, `docs/coverage.md` (Windows-only line coverage), `docs/perf-ratchet.md` (end-to-end perf ratchet: `bench/ratchet.sh`, CPU/GC/page-compile/tats-startup goldens, load guard, `--update`).

## Namespace

All code under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::platform`, `bro::engine`, `bro::layout`, `bro::canvas`, `bro::webgl`, `bro::scene`, `bro::physics`, `bro::svg`.
