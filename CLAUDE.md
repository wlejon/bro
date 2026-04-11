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
./build/src/Debug/bro.exe apps/dashboard
./build/src/headless/Debug/bro-headless.exe apps/dashboard
```

**Linux** (single-config, executables have no .exe suffix or config subdirectory):
```bash
./build/src/bro apps/dashboard
./build/src/headless/bro-headless apps/dashboard
```

**Common headless invocations** (paths differ per platform as above):
```bash
# Interactive JS REPL (GPU — default)
bro-headless apps/dashboard

# JS script file
bro-headless apps/dashboard test.js

# Inline JS expression
bro-headless apps/dashboard -e "document.querySelector('#btn').click()" -e "screenshot('out.png')"

# CPU-only, no GPU/WebGL — for CI without GPU (or use SDL_VIDEODRIVER=dummy on Linux)
bro-headless --no-gpu apps/dashboard
```

Submodules must be initialized: `git submodule update --init`

Skia is a pre-built dependency. On Linux, run `third_party/skia/build_skia_linux.sh` to build and install it. On Windows, build Skia separately and place `skia.lib` in `third_party/skia/lib/{Debug,Release}/`.

## Architecture

Bro is a lightweight app runtime: HTML/CSS/JS apps rendered with GPU acceleration. ~31K LOC of C++20.

**Stack:** QuickJS (JS engine) + brokit (web/system APIs) + htmlayout (HTML parsing + CSS + layout) + broaudio (audio engine) + Skia (raster rendering) + SDL3 (windowing + GPU display)

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
layout  (htmlayout adapters, draw traversal, replaced elements)
  ↑
dom  (Document/Element/TextNode tree, events, style proxy)
  ↑
js  (QuickJS wrapper, console, timers, DOM bindings)
  ↑
engine  (orchestrates all subsystems, main loop)
```

### Key design patterns

- **Single DOM:** HTML is parsed with gumbo into a `bro::dom` tree. CSS is resolved by `htmlayout::css::Cascade`, layout by `htmlayout::layout::layoutTree()`, and rendering by `DrawTraversal` which walks the tree and issues Skia draw calls.
- **GPU rendering:** `GPUContext` owns the `SDL_GPUDevice`, shader pipelines (color + texture), and manages the D3D12 render passes. `SkiaRenderer` rasterizes HTML/CSS to a Skia surface, uploads to a `SDL_GPUTexture` via transfer buffers, and composites as a fullscreen textured quad. Canvas 2D commands are batched into vertex buffers and drawn via the color pipeline.
- **Renderer abstraction:** `bro::render::Renderer` is a pure virtual interface for 2D rasterization. `SkiaRenderer` (GPU-accelerated, windowed) and `RasterRenderer` (CPU Skia with real fonts, headless) both implement it. Both use Skia with platform-native font backends (DirectWrite on Windows, FreeType/fontconfig on Linux) for accurate text metrics.
- **Event flow:** SDL event → `EventLoop` → `Engine::handleMouse*/Key*()` → hit test via `htmlayout::layout::hitTest()` → create `MouseEvent`/`KeyboardEvent` → `dispatchEvent()` with manual bubbling → JS listeners.
- **Dirty tracking:** DOM mutations call `document_->markDirty()`. Main loop only re-layouts when `isDirty()` is true.
- **Virtual time in headless:** `advanceTime(ms)` manually ticks timers without real delays, enabling deterministic testing.
- **JS lifetime:** QuickJS context must outlive all DOM elements (they hold JS function references).

### Headless JS interface

Headless mode is driven by JavaScript — the same language apps are written in. Three invocation modes: JS REPL (interactive), script file (`test.js`), or inline expressions (`-e "expr"`). Headless-specific globals: `screenshot(path)`, `advanceTime(ms)`, `flush()`, `sleep(ms)`, `assert(cond, msg?)`. All standard DOM APIs work (`querySelector`, `.click()`, `.textContent`, `getBoundingClientRect()`, etc.). See `docs/headless.md` for full reference.

## Third-party dependencies (all in third_party/ as git submodules)

| Library | Target | Notes |
|---------|--------|-------|
| QuickJS | `qjs` | JS engine, built as library |
| brokit | `brokit` | Web-standard + system APIs (fetch, streams, storage, fs, crypto, events) |
| htmlayout | `htmlayout` | HTML5 parsing (gumbo), CSS parsing, selector matching, style cascade, layout |
| broaudio | `broaudio` | Real-time audio engine (synthesis, effects, spatial, MIDI, mixing) |
| SDL3 | `SDL3::SDL3` | Built from submodule (static) |
| Skia | `skia` (imported) | Pre-built binaries, auto-detected |

## Namespace

All code is under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::js`, `bro::platform`, `bro::engine`, `bro::layout`.
