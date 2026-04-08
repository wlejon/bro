# Bro(wser)

A lightweight desktop application runtime that runs HTML/CSS/JS apps as native windows. Bro combines a JavaScript engine, an HTML/CSS layout engine, and GPU-accelerated rendering into a single executable — no browser required.

Note from the co-pilot: i am a career programmer but this and its sister repositories are vibe coded. use at your own risk. 

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         bro <app-dir>                            │
├──────────┬──────────┬──────────┬──────────┬──────────┬───────────┤
│  QuickJS │  brokit  │htmlayout │ broaudio │   Skia   │   SDL3    │
│   (JS)   │ (APIs)   │ (Layout) │ (Audio)  │ (Render) │ (Window)  │
└──────────┴──────────┴──────────┴──────────┴──────────┴───────────┘
```

- **QuickJS** — ES2023 JavaScript engine with DOM API bindings.
- **brokit** — Web-standard and system APIs (fetch, streams, storage, fs, crypto, events, and more). See [brokit](https://github.com/wlejon/brokit).
- **htmlayout** — HTML5 parsing (gumbo), CSS parsing, selector matching, style cascade, and block/inline/flex layout. See [htmlayout](https://github.com/wlejon/htmlayout).
- **broaudio** — Real-time audio engine: synthesis (oscillators, wavetable, noise), effects (filters, delay, reverb), spatial audio, MIDI input, and mixing bus architecture. See [broaudio](https://github.com/wlejon/broaudio).
- **Skia** — 2D rasterization (text, paths, images, gradients). Renders to a CPU surface, uploaded to GPU each frame.
- **SDL3** — Windowing, input events, and GPU display compositing via SDL_GPU.

C++20. Two executables: `bro` (windowed) and `bro-headless` (automated testing). See [docs/multi-repo-workflow.md](docs/multi-repo-workflow.md) for development across the four repos.

## Features

- HTML/CSS parsing, layout, and GPU-accelerated rendering
- JavaScript DOM API (`querySelector`, `createElement`, `appendChild`, `addEventListener`, `textContent`, `innerHTML`, `style`, attributes, `classList`)
- Event system with bubbling (click, mousedown/up, keydown/up, text input)
- CSS features: gradients (linear, radial, conic), background images, border radius, flexbox, overflow/scroll
- SVG rendering (basic shapes, paths, transforms)
- Canvas 2D API
- WebGL 2.0
- Web Audio API with synthesis (oscillators, wavetable, noise), effects, spatial audio, and MIDI input (broaudio)
- Form controls (`<input>`, `<textarea>`, `<select>` with text editing, cursor, focus, tab navigation)
- Web Components with Shadow DOM (custom elements, slots, style encapsulation)
- Fetch API, localStorage/sessionStorage
- jQuery and Vue 3 compatibility
- Headless mode for deterministic testing with virtual time

## Building

### Prerequisites

**Windows:**
- **MSVC** (Visual Studio 2022+) — MinGW is not supported
- **CMake** 3.24+
- **Skia** pre-built libraries in `third_party/skia/`

**Linux (Debian/Ubuntu):**
- **GCC 12+** or **Clang 15+**
- **CMake** 3.24+
- System packages: `build-essential cmake libfreetype-dev libfontconfig-dev libgl-dev libjpeg-dev libpng-dev libwebp-dev`
- **Skia** pre-built library (see below)

### Setup

```bash
git clone --recursive https://github.com/wlejon/bro
cd bro
```

### Building Skia

Skia is a pre-built dependency — it is not built automatically by CMake. Place the built library in `third_party/skia/lib/{Debug,Release}/`.

On Linux, a build script is provided:

```bash
# Install prerequisites (Debian/Ubuntu)
sudo apt install build-essential clang python3 ninja-build \
                 libfreetype-dev libfontconfig-dev libgl-dev \
                 libjpeg-dev libpng-dev libwebp-dev

# Build Skia (clones source, syncs deps, builds, and installs libskia.a)
cd third_party/skia
./build_skia_linux.sh           # Release only
./build_skia_linux.sh Debug     # Debug only
./build_skia_linux.sh all       # Both
```

On Windows, build Skia separately with `gn`/`ninja` and place `skia.lib` in the same location.

### Build

```bash
# Configure
cmake -B build

# Debug build
cmake --build build --config Debug

# Release build
cmake --build build --config Release
```

On Windows this uses the Visual Studio multi-config generator. On Linux it defaults to Unix Makefiles (single-config; set `CMAKE_BUILD_TYPE` at configure time or use `-G Ninja`).

## Usage

### Windowed mode

```bash
# Windows
./build/src/Debug/bro.exe apps/hello

# Linux
./build/src/bro apps/hello
```

Loads `apps/hello/index.html`, applies stylesheets, executes scripts, and opens a window.

### Headless mode

Headless mode runs the full engine pipeline (GPU rendering, real fonts, WebGL) without a visible window, driven entirely by JavaScript.

```bash
# Interactive JS REPL
bro-headless apps/hello

# Run a JS script file
bro-headless apps/hello test.js

# Inline JS expressions
bro-headless apps/hello -e "document.querySelector('#btn').click()" -e "screenshot('out.png')"

# CPU-only mode (no GPU/WebGL — for CI without a GPU)
bro-headless --no-gpu apps/hello
```

On Linux without a display server, use `--no-gpu` or set `SDL_VIDEODRIVER=dummy`.

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
