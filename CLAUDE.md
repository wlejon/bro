# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure
cmake -B build

# Build (debug)
cmake --build build --config Debug

# Build (release)
cmake --build build --config Release
```

**Windows** (Visual Studio multi-config generator, do not use MinGW):
```bash
./build/src/Debug/bro.exe apps/example
./build/src/headless/Debug/bro-headless.exe apps/example
```

**Linux** (single-config, executables have no .exe suffix or config subdirectory):
```bash
./build/src/bro apps/example
./build/src/headless/bro-headless apps/example
```

**Common headless invocations** (paths differ per platform as above):
```bash
# Interactive JS REPL (GPU — default)
bro-headless apps/example

# JS script file
bro-headless apps/example test.js

# Inline JS expression
bro-headless apps/example -e "document.querySelector('#btn').click()" -e "screenshot('out.png')"

# CPU-only, no GPU/WebGL
bro-headless --no-gpu apps/example
```

Submodules must be initialized: `git submodule update --init`

Skia is a pre-built dependency. On Linux, run `third_party/skia/build_skia_linux.sh` to build and install it. On Windows, build Skia separately and place `skia.lib` in `third_party/skia/lib/{Debug,Release}/`.

## Architecture

Bro is a lightweight app runtime: HTML/CSS/JS apps rendered with GPU acceleration. ~31K LOC of C++20.

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
| brokit | `brokit` | Web-standard + system APIs (fetch, streams, storage, fs, crypto, events) (standalone or submodule) |
| htmlayout | `htmlayout` | HTML5 parsing (gumbo), CSS parsing, selector matching, style cascade, layout (standalone or submodule) |
| broaudio | `broaudio` | Real-time audio engine (synthesis, effects, spatial, MIDI, mixing) (standalone or submodule) |
| bromesh | `bromesh` | Mesh generation, manipulation, analysis, and I/O (standalone or submodule) |
| brogameagent | `brogameagent` | Game AI: navmesh, pathfinding, steering, perception (standalone or submodule) |
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
| `docs/noise-api.js` | `bro.noise` — FastNoise2 SIMD noise generation |
| `docs/scene-api.js` | `bro.scene` — 3D scene graph, shapes, sprites, meshes, physics nodes |
| `docs/net-api.js` | `bro.net` — game networking (host/connect/send/broadcast) via GNS |
| `docs/crosshair-api.js` | `bro.crosshair` — engine-level crosshair overlay (cross/dot/circle/crossdot) |
| `docs/worker-api.js` | `Worker` — web worker threads |
| `docs/ai-game-api.js` | `bro.ai.game` — navmesh, pathfinding, steering, perception, capabilities, AgentBinding |
| `docs/brokit-api.js` | brokit runtime — Node modules (fs, path, os, child_process) + web globals (fetch, crypto, WebSocket, streams, storage, etc.) |

Other docs: `docs/headless.md` (headless mode), `docs/settings.md` (settings system), `docs/inspect.md` (DOM inspector that's very useful in headless), `docs/moba-demo.md` (MOBA architecture).

## Namespace

All code is under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::js`, `bro::platform`, `bro::engine`, `bro::layout`, `bro::canvas`, `bro::webgl`, `bro::scene`, `bro::physics`, `bro::svg`.
