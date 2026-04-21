# Bro

An HTML/CSS/JS runtime for desktop apps and games — QuickJS, a custom layout engine, Skia, and SDL_GPU, with 3D, physics, audio, and game networking wired straight into the DOM. Windows, Mac, and Linux are currently supported. 

Note from the co-pilot: i am a career programmer but this and its sister repositories are vibe coded. use at your own risk.

## Launcher

>bro apps/launcher

![launcher](docs/launcher.png)

## Why

Coding agents build web UIs really well. While they can build native UIs, in my experience, there's a lot more debugging happening to get something working. Electron/Tauri/etc solve that, but only for what the web platform natively supports.

Bro is the middle path. HTML/CSS/JS is the UI layer — so agents stay productive — but the rendering pipeline is ours, which means we can plug in whatever we want underneath. And we have: a 3D scene graph, Jolt physics, a real-time audio engine, mesh generation and CSG, navmesh pathfinding, game networking over GNS, native file dialogs, menu bars. All exposed to JS, all running in one process, no IPC to a Chromium renderer.

## Hello world

```html
<!-- apps/hello/index.html -->
<h1 id="msg">Hello</h1>
<button id="btn">Click me</button>
<script>
  document.getElementById('btn').addEventListener('click', () => {
    document.getElementById('msg').textContent = 'Hello, bro';
  });
</script>
```

```bash
bro apps/hello
```

see apps/ for more examples 

## What you get

**Web platform** — HTML5 parsing, CSS (flexbox, gradients, border radius, overflow/scroll), SVG, Canvas 2D, WebGL 2.0, Web Components with Shadow DOM, Web Workers, Fetch, localStorage, form controls with real text editing. threejs, jQuery, work.

**Beyond the web** — 3D scene graph with mesh rendering and terrain (`bro.scene`), Jolt rigid-body physics with contact events, Web Audio with synthesis / effects / spatial / MIDI (broaudio), mesh generation and CSG (bromesh), navmesh and A* pathfinding (`bro.ai.game`), game networking over GameNetworkingSockets (`bro.net`), native file dialogs, native menu bars, 3D transform gizmos, native crosshair.

**For development** — Headless mode runs the full pipeline (GPU, real fonts, WebGL) without a window, driven by JS with virtual time for deterministic testing. See [docs/headless.md](docs/headless.md).

## Example apps

See `apps/` for runnable examples:

- `launcher` — app picker, opens the others
- `tetris` — keyboard input, canvas, audio
- `synth` — wavetable synth with MIDI, keyboard, and effects chain
- `mesh-viewer` — 3D mesh import (OBJ / glTF / STL / PLY / FBX / VOX)
- `terrain` — voxel terrain with streaming chunks and raycast editing
- `fps` — first-person demo: scene, physics, input, audio
- `ai-arena` — navmesh + MCTS agents
- `scene-editor`, `spatial-audio`, `example`

## Architecture

- **QuickJS** — JavaScript engine (ES2020+).
- **qjsbind** — Header-only C++20 binding library for exposing C++ classes/functions to QuickJS with automatic type conversion. See [qjsbind](https://github.com/wlejon/qjsbind).
- **brokit** — Web-standard and system APIs (fetch, streams, storage, fs, crypto, events, and more). See [brokit](https://github.com/wlejon/brokit).
- **htmlayout** — HTML5 parsing (gumbo), CSS parsing, selector matching, style cascade, and block/inline/flex layout. See [htmlayout](https://github.com/wlejon/htmlayout).
- **broaudio** — Real-time audio engine. See [broaudio](https://github.com/wlejon/broaudio).
- **bromesh** — Mesh generation, manipulation, analysis, and I/O. See [bromesh](https://github.com/wlejon/bromesh).
- **brogameagent** — Game AI: navmesh, A* pathfinding, steering, perception. See [brogameagent](https://github.com/wlejon/brogameagent).
- **Jolt Physics** — Rigid body physics with contact listeners, integrated into the scene graph.
- **Skia** — 2D rasterization (text, paths, images, gradients). HTML/CSS is rasterized to a texture via Skia's Ganesh GL backend, with a CPU raster fallback for `--no-gpu` headless runs.
- **SDL3** — Windowing, input events, and GPU display compositing via SDL_GPU (D3D12 on Windows). The Skia-rasterized UI texture and the 3D scene layer are composited together through SDL_GPU pipelines.

Also uses **GameNetworkingSockets** (Valve's GNS, via vcpkg), **glad** (OpenGL 3.3 Core loader), **stb_image**, and **FastNoise2** (via brokit).

C++20. `bro` (windowed) and `bro-headless` (headless JS scripting and testing). See [docs/multi-repo-workflow.md](docs/multi-repo-workflow.md) for development across the sibling repos.

## Building

See [BUILDING.md](BUILDING.md) for prerequisites, Skia setup, and build commands across Windows, Linux, and macOS.

## Usage

### Windowed mode

```bash
# Windows
./build/src/Debug/bro.exe apps/example

# Linux / macOS
./build/src/bro apps/example
```

Loads the app's `index.html`, applies stylesheets, executes scripts, and opens a window.

### Headless mode

```bash
bro-headless apps/example                                   # interactive REPL
bro-headless apps/example test.js                           # script file
bro-headless apps/example -e "document.querySelector('#btn').click()"
bro-headless --no-gpu apps/example                          # CPU-only (CI)
```

On Linux without a display server, use `--no-gpu`. 

Headless globals: `screenshot(path)`, `advanceTime(ms)`, `flush()`, `sleep(ms)`, `assert(cond, msg?)`.

See [docs/headless.md](docs/headless.md) for full documentation.

## App structure

An app is a directory containing at minimum an `index.html`:

```
apps/myapp/
  index.html      # required
  style.css       # linked via <link rel="stylesheet">
  app.js          # loaded via <script src="...">
```

## JS API reference

Annotated `.js` files in [docs/](docs/) — load them in your editor for JSDoc on every binding:

`audio-api.js`, `mesh-api.js`, `scene-api.js`, `physics-api.js`, `terrain-api.js`, `ai-game-api.js`, `net-api.js`, `noise-api.js`, `worker-api.js`, `dialogs-api.js`, `menu-api.js`, `gizmo-api.js`, `crosshair-api.js`, `brokit-api.js`.

Plus [settings.md](docs/settings.md) (settings + action binding) and [inspect.md](docs/inspect.md) (DOM inspector, very useful in headless).

## Warning

while you technically could easily wire this up to be an actual web browser, it was not built for that. i have not paid mind to security at all. this exposes a _lot_ more of your system to javascript than a browser does. it'd be better if we didn't run random internet code in this unsecured sandbox.

## why are there so many repos?

splitting the codebase exploration into chunks makes coding agents work better for my workflow. i'll try to keep setup reasonable but i expect the submodule list will continue to grow.

## License

[MIT](LICENSE)

Third-party dependencies are under their own permissive licenses (MIT, BSD-3-Clause, zlib, Apache-2.0). See each library's LICENSE file in `third_party/` or its respective repository.
