# Bro

A lightweight desktop application framework that runs HTML/CSS/JS apps as native windows. Bro combines a JavaScript engine, an HTML/CSS layout engine, and a GPU (or software) renderer into a single executable -- no browser required.

## Architecture

```
┌─────────────────────────────────────────────┐
│                 bro <app-dir>               │
├─────────┬──────────┬──────────┬─────────────┤
│  QuickJS │ LiteHTML │  Skia*   │    SDL3     │
│  (JS)    │ (Layout) │ (Render) │  (Window)   │
└─────────┴──────────┴──────────┴─────────────┘
```

- **QuickJS** -- ES2023 JavaScript engine. Runs app scripts with a DOM API (`document.getElementById`, `addEventListener`, `textContent`, `style`, etc.).
- **LiteHTML** -- HTML/CSS parser and layout engine. Handles box model, selectors, text flow, and CSS styling.
- **Skia** (optional) -- GPU-accelerated 2D rendering via Vulkan. When not available, falls back to an SDL3 2D renderer with Win32 GDI text.
- **SDL3** -- Cross-platform windowing, input events, and software rendering fallback.

## Status

Early prototype. The framework can load an HTML/CSS/JS app, render it in a window, and handle basic mouse/keyboard interaction with JS event handlers. Many features are stubs or incomplete (see below).

### What works

- Loading an app from a directory (`index.html` + CSS + JS)
- HTML/CSS parsing and layout via LiteHTML
- Rendering text, backgrounds, borders, solid fills
- JS `document.getElementById`, `querySelector`, `querySelectorAll`
- JS `element.textContent`, `innerHTML` (get), `style.*`, attributes
- JS `addEventListener` / `removeEventListener` with event bubbling
- JS `console.log/warn/error`, `setTimeout`, `setInterval`
- Mouse click events with hit testing
- Keyboard events (raw keycodes)
- Window resizing with re-layout
- Headless mode for automated testing (`bro-headless`)

### What doesn't work yet

- Image loading and rendering
- CSS gradients (filled with first color stop only)
- Border radius (drawn as straight lines)
- `innerHTML` setter (stores raw HTML as text, doesn't re-parse)
- `document.createElement` + `appendChild` (memory management issues)
- ES modules (`import`/`export`)
- Element-level `querySelector`/`querySelectorAll`
- Mouse coordinates and modifier keys in JS events
- Keyboard key names (sends raw keycodes, not `"Enter"`, `"KeyA"`, etc.)
- Cursor changes
- Anchor (`<a>`) navigation
- Frame rate limiting / vsync
- Non-Windows platforms (text rendering is Win32 GDI only in SDL mode)

## Building

### Prerequisites

- **MSVC** (Visual Studio 2022+) -- MinGW is not supported
- **CMake** 3.28+
- **Ninja** (recommended)
- Git submodules: QuickJS, LiteHTML, SDL3, Vulkan-Headers

### Setup

```bash
git clone --recursive <repo-url>
cd bro
```

### Build (Debug)

```bash
cmake --preset debug
cmake --build build-debug
```

### Build (Release)

```bash
cmake --preset default
cmake --build build
```

The default build uses `BRO_NO_SKIA` (SDL software renderer). To enable Skia+Vulkan rendering, place pre-built Skia libraries in `third_party/skia/lib/` and headers in `third_party/skia/include/`, then configure with `-DBRO_USE_SKIA=ON`.

## Usage

### Windowed mode

```bash
bro apps/hello
```

Loads `apps/hello/index.html`, applies linked stylesheets, executes `<script>` tags, and opens a window.

### Headless mode

```bash
bro-headless apps/hello              # interactive
bro-headless apps/hello test.txt     # run script
echo "dump #btn" | bro-headless apps/hello  # piped
```

Commands: `dump`, `dump <selector>`, `diff`, `click <selector>`, `eval <js>`, `wait <ms>`, `quit`.

See [docs/headless.md](docs/headless.md) for full documentation.

## App structure

An app is a directory containing at minimum an `index.html`:

```
apps/myapp/
  index.html      # required
  style.css       # linked via <link rel="stylesheet">
  app.js          # loaded via <script src="...">
```

Bro extracts `<link>` and `<script>` references from `index.html` and loads them in order.

## Project layout

```
src/
  main.cpp              # Entry point (windowed)
  engine/               # App loading, main loop, event dispatch
  dom/                  # DOM tree: Document, Element, TextNode, Event, StyleProxy
  js/                   # QuickJS bindings: Runtime, Console, Timers, DomBindings
  layout/               # LiteHTML container implementation, font management
  render/               # Renderer interface + Skia/SDL backends
  platform/             # SDL window, Vulkan context, event loop
  headless/             # Headless testing tool
  util/                 # Logging, string utilities
third_party/
  quickjs/              # JS engine (git submodule)
  litehtml/             # HTML/CSS layout (git submodule)
  SDL/                  # Windowing + input (git submodule)
  Vulkan-Headers/       # Vulkan type definitions (git submodule)
  skia/                 # Pre-built Skia (not included, optional)
apps/
  hello/                # Example app
docs/
  headless.md           # Headless mode documentation
```

## License

TBD
