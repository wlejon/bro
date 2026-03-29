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

# Run windowed app
./build/src/Debug/bro.exe apps/hello

# Run headless (interactive)
./build/src/headless/Debug/bro-headless.exe apps/hello

# Run headless (piped)
echo -e "click #btn\ndump #counter\nquit" | ./build/src/headless/Debug/bro-headless.exe apps/hello 2>/dev/null

# Run headless (script file)
./build/src/headless/Debug/bro-headless.exe apps/hello test.txt
```

Uses the Visual Studio generator (multi-config). Vcpkg at D:/vcpkg is auto-detected. Do not use MinGW.

Submodules must be initialized: `git submodule update --init`

## Architecture

Bro is a lightweight app runtime: HTML/CSS/JS apps rendered with GPU acceleration. ~6K LOC of C++20.

**Stack:** QuickJS (JS engine) + LiteHTML (HTML/CSS layout) + Skia (raster rendering) + SDL3 (windowing + GPU display)

**Two executables:**
- `bro` — windowed app runner
- `bro-headless` — headless testing tool (no GPU/window, text commands via stdin)

### Module dependency graph

```
util  (logging, string helpers — standalone)
  ↑
platform  (SDL3 window, event loop)
  ↑
render  (abstract Renderer interface, SkiaRenderer)
  ↑
layout  (LiteHTML document_container bridge → Renderer)
  ↑
dom  (Document/Element/TextNode tree, events, style proxy)
  ↑
js  (QuickJS wrapper, console, timers, DOM bindings)
  ↑
engine  (orchestrates all subsystems, main loop)
```

### Key design patterns

- **Dual DOM:** LiteHTML parses HTML and owns layout. A parallel `bro::dom` tree is built from it for JS interaction. Both are kept in sync via dirty tracking.
- **Renderer abstraction:** `bro::render::Renderer` is a pure virtual interface. `SkiaRenderer` renders to a CPU-side Skia surface, uploads to an SDL texture for GPU display. SDL picks the best GPU backend (D3D11/12, Metal, etc.).
- **Event flow:** SDL event → `EventLoop` → `Engine::handleMouse*/Key*()` → hit test via LiteHTML → create `MouseEvent`/`KeyboardEvent` → `dispatchEvent()` with manual bubbling → JS listeners.
- **Dirty tracking:** DOM mutations call `document_->markDirty()`. Main loop only re-layouts when `isDirty()` is true.
- **Virtual time in headless:** `advanceTime(ms)` manually ticks timers without real delays, enabling deterministic testing.
- **JS lifetime:** QuickJS context must outlive all DOM elements (they hold JS function references).

### Headless testing commands

`dump [selector]`, `click <selector>`, `eval <js>`, `wait <ms>`, `diff`, `quit`. See `docs/headless.md` for full reference. Headless uses approximate text measurement (no real fonts).

## Third-party dependencies (all in third_party/ as git submodules)

| Library | Target | Notes |
|---------|--------|-------|
| QuickJS | `qjs` | JS engine, built as library |
| LiteHTML | `litehtml` | HTML/CSS layout |
| SDL3 | `SDL3::SDL3` | From vcpkg (shared) or submodule (static) |
| Skia | `skia` (imported) | Pre-built binaries, auto-detected |

## Namespace

All code is under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::js`, `bro::platform`, `bro::engine`, `bro::layout`.
