# Bro

A lightweight desktop application runtime that runs HTML/CSS/JS apps as native windows. Bro combines a JavaScript engine, an HTML/CSS layout engine, and GPU-accelerated rendering into a single executable — no browser required.

## Architecture

```
┌─────────────────────────────────────────────┐
│                bro <app-dir>                │
├──────────┬──────────┬──────────┬────────────┤
│  QuickJS │ LiteHTML │   Skia   │    SDL3    │
│   (JS)   │ (Layout) │ (Render) │  (Window)  │
└──────────┴──────────┴──────────┴────────────┘
```

- **QuickJS** — ES2023 JavaScript engine with DOM API bindings.
- **LiteHTML** — HTML/CSS parser and layout engine. Handles box model, selectors, text flow, flexbox, and CSS styling.
- **Skia** — High-quality 2D rasterization (text, paths, images, gradients). Renders to a CPU surface, uploaded to GPU each frame.
- **SDL3** — Windowing, input events, and OpenGL-based display compositing.

~6K lines of C++20 glue code. Two executables: `bro` (windowed) and `bro-headless` (automated testing).

## Features

- HTML/CSS parsing, layout, and GPU-accelerated rendering
- JavaScript DOM API: `querySelector`, `createElement`, `appendChild`, `addEventListener`, `textContent`, `innerHTML`, `style`, attributes, classList
- Event system with bubbling: mouse (click, mousedown, mouseup), keyboard (keydown, keyup, text input)
- CSS gradients (linear, radial, conic), background images, border radius
- SVG rendering (basic shapes, paths, transforms)
- Canvas 2D API
- WebGL 2.0
- Audio playback
- Form controls (`<input>` with text editing, cursor, focus management)
- Vue 3 and jQuery compatibility
- Headless mode for deterministic testing with virtual time

## Building

### Prerequisites

- **MSVC** (Visual Studio 2022+) — MinGW is not supported
- **CMake** 3.28+
- **Skia** pre-built libraries in `third_party/skia/`
- Git submodules: QuickJS, LiteHTML, SDL3

### Setup

```bash
git clone --recursive <repo-url>
cd bro
```

### Build

```bash
# Configure (uses Visual Studio generator)
cmake -B build

# Debug build
cmake --build build --config Debug

# Release build
cmake --build build --config Release
```

Vcpkg at `D:/vcpkg` is auto-detected if present. Skia is required — place pre-built libraries in `third_party/skia/lib/` and headers in `third_party/skia/include/`.

## Usage

### Windowed mode

```bash
./build/src/Debug/bro.exe apps/hello
```

Loads `apps/hello/index.html`, applies stylesheets, executes scripts, and opens a window.

### Headless mode

```bash
./build/src/headless/Debug/bro-headless.exe apps/hello              # interactive
./build/src/headless/Debug/bro-headless.exe apps/hello test.txt     # run script file
echo -e "click #btn\ndump\nquit" | ./build/src/headless/Debug/bro-headless.exe apps/hello 2>/dev/null
```

Commands: `dump [selector]`, `click <selector>`, `eval <js>`, `wait <ms>`, `diff`, `quit`.

See [docs/headless.md](docs/headless.md) for full documentation.

## App structure

An app is a directory containing at minimum an `index.html`:

```
apps/myapp/
  index.html      # required
  style.css       # linked via <link rel="stylesheet">
  app.js          # loaded via <script src="...">
```

## Project layout

```
src/
  engine/             # App loading, main loop, event dispatch
  dom/                # DOM tree: Document, Element, TextNode, Event, StyleProxy
  js/                 # QuickJS bindings: DOM, Console, Timers, Canvas, WebGL, Audio
  layout/             # LiteHTML container, font management, replaced elements (input, SVG)
  render/             # Skia renderer, OpenGL context, GPU compositing
  canvas/             # Canvas 2D implementation
  svg/                # SVG parser and renderer
  webgl/              # WebGL 2.0 implementation
  audio/              # Audio engine
  platform/           # SDL3 window and event loop
  headless/           # Headless testing tool
  util/               # Logging, string utilities
third_party/
  quickjs/            # JS engine (git submodule, MIT)
  litehtml/           # HTML/CSS layout (git submodule, BSD-3-Clause)
  SDL/                # Windowing + input (git submodule, zlib)
  skia/               # 2D rendering (pre-built, BSD-3-Clause)
  glad/               # OpenGL loader (WTFPL/CC0 + Apache-2.0)
  stb/                # Image loading (MIT/Public Domain)
apps/
  hello/              # Minimal example
  vue-test/           # Vue 3 feature test
  jquery-test/        # jQuery compatibility test
  ...                 # Additional example apps
docs/
  headless.md         # Headless mode documentation
```

## License

[MIT](LICENSE)

Third-party dependencies are under their own permissive licenses (MIT, BSD-3-Clause, zlib, Apache-2.0). See each library's LICENSE file in `third_party/`.
