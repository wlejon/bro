# Bro(wser)

A lightweight desktop application runtime that runs HTML/CSS/JS apps as native windows. Bro combines a JavaScript engine, an HTML/CSS layout engine, and GPU-accelerated rendering into a single executable — no browser required.

Note from the co-pilot: i am a career programmer but this and its sister repositories are vibe coded. use at your own risk. 

## Architecture

- **QuickJS** — JavaScript engine (ES2020+).
- **qjsbind** — Header-only C++20 binding library for exposing C++ classes/functions to QuickJS with automatic type conversion. See [qjsbind](https://github.com/wlejon/qjsbind).
- **brokit** — Web-standard and system APIs (fetch, streams, storage, fs, crypto, events, and more). See [brokit](https://github.com/wlejon/brokit).
- **htmlayout** — HTML5 parsing (gumbo), CSS parsing, selector matching, style cascade, and block/inline/flex layout. See [htmlayout](https://github.com/wlejon/htmlayout).
- **broaudio** — Real-time audio engine: synthesis (oscillators, wavetable, noise), effects (filters, delay, reverb), spatial audio, MIDI input, and mixing bus architecture. See [broaudio](https://github.com/wlejon/broaudio).
- **bromesh** — Mesh generation (primitives, isosurface, voxel), manipulation (subdivision, simplification, CSG), analysis (raycasting, collision), baking, optimization, and I/O (OBJ, glTF, STL, PLY, FBX, VOX). See [bromesh](https://github.com/wlejon/bromesh).
- **brogameagent** — Game AI: navmesh generation, A* pathfinding, steering behaviors, and perception. Exposed to JS as `bro.ai.game.*`. See [brogameagent](https://github.com/wlejon/brogameagent).
- **Jolt Physics** — Rigid body physics with contact listeners, integrated into the scene graph.
- **Skia** — 2D rasterization (text, paths, images, gradients). HTML/CSS is rasterized to a texture via Skia's Ganesh GL backend, with a CPU raster fallback for `--no-gpu` headless runs.
- **SDL3** — Windowing, input events, and GPU display compositing via SDL_GPU (D3D12 on Windows). The Skia-rasterized UI texture and the 3D scene layer are composited together through SDL_GPU pipelines.

Also uses **GameNetworkingSockets** (Valve's GNS, via vcpkg — powers the `bro.net` game networking API), **glad** (OpenGL 3.3 Core loader), **stb_image** (image loading/writing), and **FastNoise2** (SIMD noise generation, via brokit).

C++20. Two executables: `bro` (windowed) and `bro-headless` (headless JS scripting and testing). See [docs/multi-repo-workflow.md](docs/multi-repo-workflow.md) for development across the sibling repos.

## Features

- HTML/CSS parsing, layout, and GPU-accelerated rendering
- JavaScript DOM API (`querySelector`, `createElement`, `appendChild`, `addEventListener`, `textContent`, `innerHTML`, `style`, attributes, `classList`)
- Event system with bubbling (click, mousedown/up, keydown/up, text input)
- CSS features: gradients (linear, radial, conic), background images, border radius, flexbox, overflow/scroll
- SVG rendering (basic shapes, paths, transforms)
- Canvas 2D API
- WebGL 2.0
- Web Audio API with synthesis (oscillators, wavetable, noise), effects, spatial audio, and MIDI input (broaudio)
- 3D scene graph with mesh rendering, cameras (perspective/orthographic), transforms, and terrain
- Mesh generation and manipulation: primitives, CSG, isosurface extraction, simplification, raycasting (bromesh)
- Rigid body physics with contact detection (Jolt)
- Game AI: navmesh, pathfinding, steering, perception (brogameagent, via `bro.ai.game`)
- Game networking: host/connect/send/broadcast over GNS (`bro.net`)
- Procedural noise generation (FastNoise2)
- Web Workers
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
- **vcpkg** (for GameNetworkingSockets — e.g. `D:/vcpkg`, with `VCPKG_ROOT` set or passed via `-DCMAKE_TOOLCHAIN_FILE`)
- **Skia** pre-built libraries in `third_party/skia/`
- Sibling repos cloned next to `bro/` (see [docs/multi-repo-workflow.md](docs/multi-repo-workflow.md)) — `qjsbind`, `bromesh`, and `brogameagent` have no submodule fallback and must be present at `../<name>`

**Linux (Debian/Ubuntu):**
- **GCC 12+** or **Clang 15+**
- **CMake** 3.24+
- System packages: `build-essential cmake libfreetype-dev libfontconfig-dev libgl-dev libjpeg-dev libpng-dev libwebp-dev`
- **vcpkg** (for GameNetworkingSockets)
- **Skia** pre-built library (see below)
- Sibling repos as above

**macOS (12+, arm64 or x86_64):**
- **Xcode Command Line Tools** (`xcode-select --install`) — Apple clang 17+
- **CMake** 3.24+, **Ninja** (`brew install cmake ninja`)
- **bash 4+** for `tests/run_tests.sh` (`brew install bash`) — the system bash 3.2 lacks `mapfile`
- **vcpkg** (for GameNetworkingSockets)
- **Skia** pre-built library (see below)
- Sibling repos as above

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

On macOS, an equivalent script is provided — Skia is built with CoreText as
the font backend (freetype/fontconfig disabled):

```bash
cd third_party/skia
./build_skia_mac.sh             # Release only
./build_skia_mac.sh Debug       # Debug only
./build_skia_mac.sh all         # Both
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

On Windows this uses the Visual Studio multi-config generator. On Linux and macOS it defaults to a single-config generator; set `CMAKE_BUILD_TYPE` at configure time or pass `-G Ninja`.

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

Headless mode runs the full engine pipeline (GPU rendering, real fonts, WebGL) without a visible window, driven entirely by JavaScript.

```bash
# Interactive JS REPL
bro-headless apps/example

# Run a JS script file
bro-headless apps/example test.js

# Inline JS expressions
bro-headless apps/example -e "document.querySelector('#btn').click()" -e "screenshot('out.png')"

# CPU-only mode (no GPU/WebGL — for CI without a GPU)
bro-headless --no-gpu apps/example
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

## Warning

while you technically could easily wire this up to be an actual web browser, it was not built for that. i have not paid mind to security at all. this exposes a _lot_ more of your system to javascript than a browser does. it'd be better if we didn't run random internet code in this unsecured sandbox.

## why are there so many repos?

splitting the codebase exploration into chunks makes coding agents work better for my workflow. i'll try to keep setup reasonable but i expect the submodule list will continue to grow.

## License

[MIT](LICENSE)

Third-party dependencies are under their own permissive licenses (MIT, BSD-3-Clause, zlib, Apache-2.0). See each library's LICENSE file in `third_party/` or its respective repository.
