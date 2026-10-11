# Bro

[![CI](https://github.com/wlejon/bro/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/bro/actions/workflows/ci.yml)
[![CodeQL](https://github.com/wlejon/bro/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/bro/actions/workflows/codeql.yml)
[![Nightly](https://github.com/wlejon/bro/actions/workflows/nightly.yml/badge.svg)](https://github.com/wlejon/bro/actions/workflows/nightly.yml)
[![Download nightly](https://img.shields.io/github/v/release/wlejon/bro?label=download%20nightly)](https://github.com/wlejon/bro/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Build desktop apps and games in **HTML/CSS/JS** with 3D, physics, audio, and on-device AI, in one native process. Custom layout engine, Skia, and Vulkan 1.3 Core. Windows, Mac, and Linux.

** this is pre-alpha **

Each nightly zip also carries **[bronze](https://github.com/wlejon/bronze)** under `bronze/`, the AOT JavaScript compiler, and the binaries in the same zip load what it emits: `bro-headless myapp --print-host-globals > web_host.globals` writes the manifest of globals bro supplies, and `bronze build app.js -o myapp/app.dll --emit-shared --host-globals web_host.globals` turns an app's JavaScript into a native module that `bro myapp` runs.

## What can I compare it to?

Two familiar yardsticks, plus one that doesn't have a name yet:

- **Rendering parity with Chromium** (HTML/CSS). The app layer should look right.
- **Feature parity with Godot's engine** (no editor). 3D, physics, audio, the game runtime.
- **On-device generation and perception as a native engine subsystem.** Every modality the engine renders, it can also generate and understand, locally.

## Broworkshop

[broworkshop](https://github.com/wlejon/broworkshop) houses the apps built with bro used to demonstrate the functionality.

>bro broworkshop/bro.json

![launcher](docs/launcher.png)

The launcher and a curated set of starter apps live in the sibling [broworkshop](https://github.com/wlejon/broworkshop) repo: games, tools, demos, and AI/research apps you can clone and reshape for your own thing. bro is the runtime; the workshop is where the patterns are worked out.

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

**The app layer.** Write the UI the way you already do. HTML5, CSS (flexbox, grid, gradients, border radius, overflow/scroll), SVG, Canvas 2D, WebGL 2.0, Web Components with Shadow DOM, Web Workers, Fetch, localStorage, form controls with real text editing. Text is shaped by HarfBuzz with bidi reordering, so ligatures, cursive joining, and RTL come out right. three.js and jQuery just work, and your web knowledge (and your coding agent's) transfers.

**Beyond the web.** Reachable from the same JS, one process, no IPC:

| Reach for | to |
|---|---|
| `bro.scene` | 3D scene graph: meshes, sprites, particles, PBR lighting, skinned characters |
| `scene.createTerrain` · `createClipmapTerrain` · `createTileWorld` | chunked height-field terrain, camera-centred clipmaps, square/hex tile grids |
| `Physics` / physics nodes | Jolt rigid bodies, contact events, constraints, raycasts, characters, vehicles |
| `AudioContext` (broaudio) | real-time audio: synthesis, effects, spatial, MIDI |
| `bro.mesh` · `bro.ai.game` · `bro.net` | mesh generation + CSG · navmesh + A* pathfinding · game networking (GNS) |
| `<video>` · `VideoEncoder` · `GifEncoder` | WebM/VP9 playback, and encoding RGBA frames back out to a file |
| native dialogs · menu bars · gizmos · multi-window · `bro.steam` | OS-native app chrome and Steamworks integration |

**On-device AI.** Every modality above, the engine can also **generate and perceive**, locally, on the GPU, in the frame loop. No API key, no network, runs offline. `bro.gpu` probes the live CUDA/Vulkan/Metal/CPU backend.

| Reach for | to |
|---|---|
| `bro.lm` | generate & understand **text** (local LLMs: Qwen3, Qwen3.5, Mistral) |
| `bro.diffusion` · `bro.vision` | generate **images** (U-Net/VAE, LoRA) · perceive them (SAM, depth, normals, matting, ControlNet) |
| `bro.triposplat` · `bro.motion` | generate **3D** from a single image · generate **animation** from text |
| `bro.tts` · `bro.stt` · `bro.diar` | **speak** (Kokoro, Qwen3-TTS) · **hear** (Whisper, Parakeet, Qwen3-ASR) · **separate speakers** |
| `bro.wake` · `bro.kws` · `bro.mic` · `bro.sense` · `bro.gesture` · `bro.listen` | wake words, keyword spotting, live mic, model-free acoustic sensors, gestures, concurrent streams |
| `bro.tensor` · `bro.image` · `bro.rave` | the tensor + image-kernel substrate underneath them all, plus neural audio encode/decode |

The left-hand `bro.*` names are the whole surface. Each has an annotated JSDoc reference in [`docs/`](docs/) (read the `.js` files, not the source).

**For development.** Headless mode runs the full pipeline (GPU, real fonts, WebGL) without a window, driven by JS with virtual time for deterministic testing. See [docs/headless.md](docs/headless.md). Line-coverage reports via OpenCppCoverage are wired up for bro and every sibling, `pwsh scripts/coverage.ps1` in any repo. See [docs/coverage.md](docs/coverage.md).

## Architecture

C++20 on Skia, Vulkan 1.3 (MoltenVK on macOS), SDL3 and Jolt, with JavaScript compiled by [bronze](https://github.com/wlejon/bronze) on [brass](https://github.com/wlejon/brass): in-process at boot (interpreter plus tiered JIT), or ahead of time into an `app.dll`/`.so`/`.dylib` the app folder carries. Three executables over one `Engine`: `bro` (windowed), `bro-headless` (scripting and testing), `bro-server` (dedicated game server, no window or renderer).

The rest lives in sibling libraries; [docs/ecosystem.md](docs/ecosystem.md) lists every repo, and [docs/multi-repo-workflow.md](docs/multi-repo-workflow.md) covers working across them.

## Building

See [BUILDING.md](BUILDING.md) for prerequisites, Skia setup, and build commands across Windows, Linux, and macOS. The build is modular: `-DBRO_PROFILE=minimal|app|full` picks a feature tier and individual `-DBRO_WITH_*` flags override it, with compiled-out features installing `{ available: false }` JS stubs. See [docs/build-options.md](docs/build-options.md).

## Usage

Running `bro` with no arguments opens the **project manager**: create projects from skeletons, open folders, or drop in folders and .zip files. See [docs/projects.md](docs/projects.md).

To run a specific app directly, pass its path:

### Windowed mode

```bash
# Windows
./build/Release/bro.exe ../broworkshop/bro.json

# Linux / macOS
./build-release/bro ../broworkshop/bro.json
```

Loads the app's `index.html`, applies stylesheets, executes scripts, and opens a window.

### Headless mode

```bash
bro-headless ../broworkshop/demos/dom-lab test.js                           # script file
bro-headless ../broworkshop/demos/dom-lab -e "document.querySelector('#btn').click()"
bro-headless --no-gpu ../broworkshop/demos/dom-lab                          # CPU-only (CI)
```

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

For more elaborate setups, such as multiple apps under a project root with shared `lib/` and `system/` directories, see [broworkshop](https://github.com/wlejon/broworkshop) for the layout pattern (project-level `bro.json` with `default_app`, `lib`, `system`).

## JS API reference

Every binding is documented in an annotated `.js` file in [docs/](docs/) (JSDoc plus examples; load them in your editor), alongside the guides (`headless.md`, `settings.md`, `apps.md`, ...). `docs/reference.html` indexes them all.

## Warning

while you technically could easily wire this up to be an actual web browser, it was not built for that. i have not paid mind to security at all. this exposes a _lot_ more of your system to javascript than a browser does. it'd be better if we didn't run random internet code in this unsecured sandbox.

## why are there so many repos?

splitting the codebase exploration into chunks makes coding agents work better for my workflow. i'll try to keep setup reasonable but i expect the list of sibling repos will continue to grow. [docs/ecosystem.md](docs/ecosystem.md) is the map: every repo, what it's for, and how they depend on each other, including the desktop-environment libraries.

## License

[MIT](LICENSE)

Third-party dependencies are under their own permissive licenses (MIT, BSD-3-Clause, zlib, Apache-2.0). See each library's LICENSE file in `third_party/` or its respective repository.
