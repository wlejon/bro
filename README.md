# Bro

Build desktop apps and games in **HTML/CSS/JS** — with 3D, physics, audio, and on-device AI wired into the same JavaScript, in one native process. QuickJS, a custom layout engine, Skia, and SDL_GPU underneath. Windows, Mac, and Linux.

** this is pre-release, pre-alpha **

## What can I compare it to?

Two familiar yardsticks, plus one that doesn't have a name yet:

- **Rendering parity with Chromium** (HTML/CSS) — the app layer should look right.
- **Feature parity with Godot's engine** (no editor) — 3D, physics, audio, the game runtime.
- **On-device generation and perception as a native engine subsystem** — every modality the engine renders, it can also generate and understand, locally and in real time. There isn't an existing engine to point at for this one.

It's not there yet on any of the three, but that's the goal.

## Broworkshop

[broworkshop](https://github.com/wlejon/broworkshop) houses the apps built with bro used to demonstrate the functionality.

>bro broworkshop/bro.json

![launcher](docs/launcher.png)

The launcher and a curated set of starter apps live in the sibling [broworkshop](https://github.com/wlejon/broworkshop) repo — games, tools, demos, and AI/research apps you can clone and reshape for your own thing. bro is the runtime; the workshop is where the patterns are worked out.

## Why

Coding agents build web UIs really well. While they can build native UIs, in my experience, there's a lot more debugging happening to get something working. Electron solves that, but only for what the web platform natively supports.

In bro, HTML/CSS/JS is the UI/app layer like Electron but the rendering pipeline is ours, which means we can plug in whatever we want underneath. And we have: a 3D scene graph, Jolt physics, a real-time audio engine, mesh generation and CSG, navmesh pathfinding, game networking over GNS, native file dialogs, menu bars. All exposed to JS, all running in one process, no IPC to a Chromium renderer.

None of this is new, it's just a reconfiguration that works better for coding agents.

## Hello world

```html
<!-- hello/index.html -->
<h1 id="msg">Hello</h1>
<button id="btn">Click me</button>
<script>
  document.getElementById('btn').addEventListener('click', () => {
    document.getElementById('msg').textContent = 'Hello, bro';
  });
</script>
```

```bash
bro path/to/hello
```

See [broworkshop](https://github.com/wlejon/broworkshop) for example applications. 

## What you get

**The app layer** — write the UI the way you already do. HTML5, CSS (flexbox, grid, gradients, border radius, overflow/scroll), SVG, Canvas 2D, WebGL 2.0, Web Components with Shadow DOM, Web Workers, Fetch, localStorage, form controls with real text editing. three.js and jQuery just work — your web knowledge (and your coding agent's) transfers.

**Beyond the web** — reachable from the same JS, one process, no IPC:

| Reach for | to |
|---|---|
| `bro.scene` | 3D scene graph — meshes, terrain, sprites, particles, PBR lighting |
| `Physics` / physics nodes | Jolt rigid bodies, contact events, constraints, raycasts |
| `AudioContext` (broaudio) | real-time audio — synthesis, effects, spatial, MIDI |
| `bro.mesh` · `bro.ai.game` · `bro.net` | mesh generation + CSG · navmesh + A* pathfinding · game networking (GNS) |
| native dialogs · menu bars · gizmos · crosshair | OS-native app chrome |

**On-device AI** — every modality above, the engine can also **generate and perceive** — locally, on the GPU, in the frame loop. No API key, no network, runs offline; `bro.gpu` probes the live CUDA/Metal/CPU backend.

| Reach for | to |
|---|---|
| `bro.lm` | generate & understand **text** (local LLMs — Qwen3, Mistral) |
| `bro.diffusion` · `bro.vision` | generate **images** (U-Net/VAE, LoRA) · perceive them (SAM, depth, normals, ControlNet) |
| `bro.triposplat` | generate **3D** from a single image |
| `bro.tts` · `bro.stt` · `bro.wake` / `bro.mic` / `bro.sense` | **speak** (Kokoro, Qwen3-TTS) · **hear** (Whisper, Parakeet) · **listen** (wake-word, mic, sensors) |
| `bro.tensor` · `bro.image` | the tensor + image-kernel substrate underneath them all |

The left-hand `bro.*` names are the whole surface — each has an annotated JSDoc reference in [`docs/`](docs/) (read the `.js` files, not the source).

**For development** — Headless mode runs the full pipeline (GPU, real fonts, WebGL) without a window, driven by JS with virtual time for deterministic testing. See [docs/headless.md](docs/headless.md). Line-coverage reports via OpenCppCoverage are wired up for bro and every sibling — `pwsh scripts/coverage.ps1` in any repo. See [docs/coverage.md](docs/coverage.md).

## Architecture

- **QuickJS** — JavaScript engine (ES2020+).
- **bromath** — Header-only C++20 math: Vec/Quat/Mat, Color, AABB, easing curves. Used transitively by most siblings. See [bromath](https://github.com/wlejon/bromath).
- **qjsbind** — Header-only C++20 binding library for exposing C++ classes/functions to QuickJS with automatic type conversion. See [qjsbind](https://github.com/wlejon/qjsbind).
- **brokit** — Web-standard and system APIs (fetch, streams, storage, fs, crypto, events, and more). See [brokit](https://github.com/wlejon/brokit).
- **htmlayout** — HTML5 parsing (gumbo), CSS parsing, selector matching, style cascade, and block/inline/flex layout. See [htmlayout](https://github.com/wlejon/htmlayout).
- **broaudio** — Real-time audio engine. See [broaudio](https://github.com/wlejon/broaudio).
- **bromesh** — Mesh generation, manipulation, analysis, and I/O. See [bromesh](https://github.com/wlejon/bromesh).
- **broflora** — Ecosystem simulation (Makowski et al. "Synthetic Silviculture"): plants, foliage, blooms. See [broflora](https://github.com/wlejon/broflora).
- **brotensor** — Tensor type and device-neutral ops; CPU backend always built, CUDA/Metal additive and opt-in. Underpins brogameagent, brolm, brodiffusion, and brosoundml. See [brotensor](https://github.com/wlejon/brotensor).
- **brogameagent** — Game AI: navmesh, A* pathfinding, steering, perception. See [brogameagent](https://github.com/wlejon/brogameagent).
- **brolm** — Language/text-model inference: BPE + Unigram tokenizers, transformer text encoders (CLIP, T5), CLIP vision encoder + scorer. The text frontend brodiffusion builds on. See [brolm](https://github.com/wlejon/brolm).
- **brodiffusion** — Diffusion-model text-to-image inference: U-Net + VAE, DDIM/LCM schedulers, LoRA, INT8. See [brodiffusion](https://github.com/wlejon/brodiffusion).
- **brosoundml** — Audio-ML model inference (TTS / STT / neural codec) built on brotensor's FP32 audio op family. See [brosoundml](https://github.com/wlejon/brosoundml).
- **brovisionml** — Vision-model inference: SAM segmentation, Depth-Anything-V2 depth, DSINE surface normals, and the ControlNet conditioning annotators (HED, lineart, MLSD, OpenPose, SegFormer). See [brovisionml](https://github.com/wlejon/brovisionml).
- **broimage** — Image decode/encode (stb) plus composable kernels (reduce/map/combine/lookup/stencil/resample/gradient), geometric ops, alpha-correct compositing, color/HSV/sRGB, normalization presets, and NHWC↔NCHW preproc. Backs `bro.image` and host-side preprocessing in brolm/brodiffusion. See [broimage](https://github.com/wlejon/broimage).
- **Jolt Physics** — Rigid body physics with contact listeners, integrated into the scene graph.
- **Skia** — 2D rasterization (text, paths, images, gradients). HTML/CSS is rasterized to a texture via Skia's Ganesh GL backend, with a CPU raster fallback for `--no-gpu` headless runs.
- **SDL3** — Windowing, input events, and GPU display compositing via SDL_GPU (D3D12 on Windows). The Skia-rasterized UI texture and the 3D scene layer are composited together through SDL_GPU pipelines.

Also uses **GameNetworkingSockets** (Valve's GNS, via vcpkg), **glad** (OpenGL 3.3 Core loader), and **FastNoise2** (via brokit).

C++20. `bro` (windowed) and `bro-headless` (headless JS scripting and testing). See [docs/multi-repo-workflow.md](docs/multi-repo-workflow.md) for development across the sibling repos.

## Building

See [BUILDING.md](BUILDING.md) for prerequisites, Skia setup, and build commands across Windows, Linux, and macOS.

## Usage

Double-clicking `bro` (or running it with no arguments) opens the **project manager**: a built-in home screen for creating new projects from skeletons, opening existing project folders, and drag-and-drop importing folders or .zip files. Project paths are persisted to a per-user registry (`%APPDATA%/bro/projects.json` on Windows, `~/Library/Application Support/bro/projects.json` on macOS, `~/.local/share/bro/projects.json` on Linux). See [docs/projects.md](docs/projects.md).

To run a specific app directly, pass its path:

### Windowed mode

```bash
# Windows
./build/Debug/bro.exe ../broworkshop/bro.json

# Linux / macOS
./build/bro ../broworkshop/bro.json
```

Loads the app's `index.html`, applies stylesheets, executes scripts, and opens a window.

### Headless mode

```bash
bro-headless ../broworkshop/demos/example                                   # interactive REPL
bro-headless ../broworkshop/demos/example test.js                           # script file
bro-headless ../broworkshop/demos/example -e "document.querySelector('#btn').click()"
bro-headless --no-gpu ../broworkshop/demos/example                          # CPU-only (CI)
```

On Linux without a display server, use `--no-gpu`. 

Headless globals: `screenshot(path)`, `advanceTime(ms)`, `flush()`, `sleep(ms)`, `assert(cond, msg?)`.

See [docs/headless.md](docs/headless.md) for full documentation.

## App structure

An app is a directory containing at minimum an `index.html`:

```
myapp/
  index.html      # required
  style.css       # linked via <link rel="stylesheet">
  app.js          # loaded via <script src="...">
```

For more elaborate setups — multiple apps under a project root with shared `lib/` and `system/` directories — see [broworkshop](../broworkshop) for the layout pattern (project-level `bro.json` with `default_app`, `lib`, `system`).

## JS API reference

Annotated `.js` files in [docs/](docs/) — load them in your editor for JSDoc on every binding:

`audio-api.js`, `mesh-api.js`, `flora-api.js`, `math-api.js`, `scene-api.js`, `lighting-api.js`, `physics-api.js`, `terrain-api.js`, `ai-game-api.js`, `gpu-api.js`, `tensor-api.js`, `lm-api.js`, `diffusion-api.js`, `tts-api.js`, `stt-api.js`, `wake-api.js`, `mic-api.js`, `vision-api.js`, `net-api.js`, `noise-api.js`, `worker-api.js`, `image-api.js`, `imagebitmap-api.js`, `video-api.js`, `dialogs-api.js`, `menu-api.js`, `gizmo-api.js`, `crosshair-api.js`, `brokit-api.js`.

Plus [settings.md](docs/settings.md) (settings + action binding) and [inspect.md](docs/inspect.md) (DOM inspector, very useful in headless).

## Warning

while you technically could easily wire this up to be an actual web browser, it was not built for that. i have not paid mind to security at all. this exposes a _lot_ more of your system to javascript than a browser does. it'd be better if we didn't run random internet code in this unsecured sandbox.

## why are there so many repos?

splitting the codebase exploration into chunks makes coding agents work better for my workflow. i'll try to keep setup reasonable but i expect the submodule list will continue to grow.

## License

[MIT](LICENSE)

Third-party dependencies are under their own permissive licenses (MIT, BSD-3-Clause, zlib, Apache-2.0). See each library's LICENSE file in `third_party/` or its respective repository.
