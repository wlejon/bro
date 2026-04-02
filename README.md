# Bro(wser)

A lightweight desktop application runtime that runs HTML/CSS/JS apps as native windows. Bro combines a JavaScript engine, an HTML/CSS layout engine, and GPU-accelerated rendering into a single executable — no browser required.

## Architecture

```
┌─────────────────────────────────────────────┐
│                bro <app-dir>                │
├──────────┬──────────┬──────────┬────────────┤
│  QuickJS │ htmlayout│   Skia   │    SDL3    │
│   (JS)   │ (Layout) │ (Render) │  (Window)  │
└──────────┴──────────┴──────────┴────────────┘
```

- **QuickJS** — ES2023 JavaScript engine with DOM API bindings.
- **htmlayout** — CSS parsing, selector matching, style cascade, and block/inline/flex layout.
- **Skia** — 2D rasterization (text, paths, images, gradients). Renders to a CPU surface, uploaded to GPU each frame.
- **SDL3** — Windowing, input events, and GPU display compositing via SDL_GPU.

C++20. Two executables: `bro` (windowed) and `bro-headless` (automated testing).

## Features

- HTML/CSS parsing, layout, and GPU-accelerated rendering
- JavaScript DOM API (`querySelector`, `createElement`, `appendChild`, `addEventListener`, `textContent`, `innerHTML`, `style`, attributes, `classList`)
- Event system with bubbling (click, mousedown/up, keydown/up, text input)
- CSS features: gradients (linear, radial, conic), background images, border radius, flexbox, overflow/scroll
- SVG rendering (basic shapes, paths, transforms)
- Canvas 2D API
- WebGL 2.0
- Audio playback (SDL3)
- Form controls (`<input>`, `<textarea>`, `<select>` with text editing, cursor, focus, tab navigation)
- Web Components with Shadow DOM (custom elements, slots, style encapsulation)
- Fetch API, localStorage/sessionStorage
- jQuery and Vue 3 compatibility
- Headless mode for deterministic testing with virtual time

## Building

### Prerequisites

- **MSVC** (Visual Studio 2022+) — MinGW is not supported
- **CMake** 3.28+
- **Skia** pre-built libraries in `third_party/skia/`

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


## Usage

### Windowed mode

```bash
./build/src/Debug/bro.exe apps/hello
```

Loads `apps/hello/index.html`, applies stylesheets, executes scripts, and opens a window.

### Headless mode

Headless mode runs the full engine pipeline (GPU rendering, real fonts, WebGL) without a visible window, driven entirely by JavaScript.

```bash
# Interactive JS REPL
./build/src/headless/Debug/bro-headless.exe apps/hello

# Run a JS script file
./build/src/headless/Debug/bro-headless.exe apps/hello test.js

# Inline JS expressions
./build/src/headless/Debug/bro-headless.exe apps/hello -e "document.querySelector('#btn').click()" -e "screenshot('out.png')"

# CPU-only mode (no GPU/WebGL — for CI without a GPU)
./build/src/headless/Debug/bro-headless.exe --no-gpu apps/hello
```

Headless globals: `screenshot(path)`, `advanceTime(ms)`, `flush()`, `sleep(ms)`, `assert(cond, msg?)`. All standard DOM APIs work.

See [docs/headless.md](docs/headless.md) for full documentation.

## App structure

An app is a directory containing at minimum an `index.html`:

```
apps/myapp/
  index.html      # required
  style.css       # linked via <link rel="stylesheet">
  app.js          # loaded via <script src="...">
```

## License

[MIT](LICENSE)

Third-party dependencies are under their own permissive licenses (MIT, BSD-3-Clause, zlib, Apache-2.0). See each library's LICENSE file in `third_party/`.
