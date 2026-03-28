# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure (debug)
cmake --preset debug

# Build (debug)
cmake --build build-debug

# Configure + build (release)
cmake --preset default
cmake --build build

# Run windowed app
./build-debug/bro apps/hello

# Run headless (interactive)
./build-debug/bro-headless apps/hello

# Run headless (piped)
echo -e "click #btn\ndump #counter\nquit" | ./build-debug/bro-headless apps/hello 2>/dev/null

# Run headless (script file)
./build-debug/bro-headless apps/hello test.txt
```

Build uses Ninja + MSVC. Do not use MinGW (GCC is broken, clang+MinGW has pthread issues).

Submodules must be initialized: `git submodule update --init`

## Architecture

Bro is a lightweight app runtime: HTML/CSS/JS apps rendered with GPU acceleration. ~6K LOC of C++20.

**Stack:** QuickJS (JS engine) + LiteHTML (HTML/CSS layout) + Skia (GPU rendering via Vulkan) + SDL3 (windowing)

**Two executables:**
- `bro` — windowed app runner
- `bro-headless` — headless testing tool (no GPU/window, text commands via stdin)

### Module dependency graph

```
util  (logging, string helpers — standalone)
  ↑
platform  (SDL3 window, Vulkan context, event loop)
  ↑
render  (abstract Renderer interface, SkiaRenderer or SDLRenderer)
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
- **Renderer abstraction:** `bro::render::Renderer` is a pure virtual interface. Two implementations: `SkiaRenderer` (GPU, Vulkan, requires `BRO_USE_SKIA=ON` + pre-built Skia) and `SDLRenderer` (2D fallback, default).
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
| SDL3 | `SDL3::SDL3-static` | Static build, Vulkan enabled |
| Vulkan-Headers | `Vulkan::Headers` | Falls back to bundled if SDK not found |
| Skia | `skia` (imported) | Pre-built binaries, optional (`BRO_USE_SKIA`) |

## Namespace

All code is under `bro::` with sub-namespaces matching module directories: `bro::render`, `bro::dom`, `bro::js`, `bro::platform`, `bro::engine`, `bro::layout`.
