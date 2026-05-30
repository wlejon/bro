# bro-headless

Headless mode for bro — runs the full engine pipeline (GPU rendering, real fonts, WebGL) without a visible window. Driven entirely by JavaScript: write test scripts, automate screenshots, and manipulate the DOM using the same language your apps are written in.

## Usage

```
bro-headless [--no-gpu] [--width N] [--height N] <app-directory> [script.js | -e "expr" ...]
```

- No arguments after app directory = **interactive JS REPL**
- With `.js` file = **script mode** (runs file, then exits)
- With `-e` flags = **inline mode** (evaluates expressions, then exits)

### Flags

| Flag | Description |
|------|-------------|
| `--no-gpu` | Disable GPU rendering. Uses CPU-only Skia rasterizer (no WebGL, no scene layer compositing). For CI environments without a GPU. |
| `--fetch` | Download the app's declared models (its `bro.json` `"models"` array) via `bro.models` into the shared cache, print per-file progress, and exit. Runs GPU-free; no UI. See `docs/models-api.js`. |
| `--width N` | Viewport width in pixels (default: 1920) |
| `--height N` | Viewport height in pixels (default: 1080) |

By default, headless uses a hidden SDL window with a full OpenGL context — the same rendering pipeline as windowed mode, including GPU-accelerated Skia, WebGL2, and Canvas 2D scene layers.

## Headless globals

These functions are available in addition to all standard DOM APIs:

### Core

| Function | Description |
|----------|-------------|
| `advanceTime(ms)` | Advance virtual time by N milliseconds (fires timers, rAF callbacks, and pending JS jobs) |
| `sleep(ms)` | Alias for `advanceTime` |
| `flush()` | Force layout recalculation (called automatically after `advanceTime`) |
| `assert(condition, message?)` | Throw if condition is falsy. Failed assertions produce a nonzero exit code. |

### Screenshots

| Function | Description |
|----------|-------------|
| `screenshot(path)` | Render the current frame to a PNG file. Composites scene layers (WebGL, Canvas 2D) with the HTML/CSS UI overlay. Throws on failure. |
| `screenshot(path, selector)` | Render the current frame and crop to the element's bounding box before saving. Bounding box uses viewport-relative coords (matches `getBoundingClientRect`). Transparent canvas pixels flatten to opaque black; for alpha-preserving canvas exports use `screenshotCanvas`. |
| `screenshotCanvas(path, selector)` | Snapshot a `<canvas>` element's underlying Skia surface directly to PNG, preserving alpha. Selector must point to a 2D canvas (not WebGL or scene). |
| `getPixel(x, y)` | Return `{r, g, b, a}` for the pixel at viewport coordinates. Renders the full composited frame (HTML, Canvas, WebGL, crosshair) and reads back the pixel. |

### Input simulation

All input functions go through the full engine pipeline (hit testing, focus management, event dispatch, bubbling). After each call, `flush()` is called automatically to process pending JS jobs.

Mouse coordinates are **viewport-relative**, matching `getBoundingClientRect()` / `clientX` / `clientY`. The engine reserves a top inset for the menu bar (~28px); these helpers add it internally so `click(rect.x, rect.y)` Just Works without offset math.

| Function | Description |
|----------|-------------|
| `click(x, y [, button])` | Simulate a mouse click at viewport coordinates (mousedown + mouseup) |
| `mouseDown(x, y [, button])` | Simulate a mouse button press |
| `mouseUp(x, y [, button])` | Simulate a mouse button release |
| `mouseMove(x, y)` | Simulate mouse movement (triggers hover, mousemove events) |
| `wheel(x, y, deltaY [, deltaX])` | Simulate a mouse wheel event (deltaY in scroll lines) |
| `keyDown(keycode [, scancode, mod, repeat])` | Simulate a key press (SDL keycodes) |
| `keyUp(keycode [, scancode, mod])` | Simulate a key release |
| `textInput(text)` | Simulate text input (for typing into focused input/textarea) |
| `paste(text)` | Simulate paste on focused element with the given text (dispatches paste event, inserts into input/textarea) |
| `copy()` | Simulate copy on focused element (dispatches copy event, returns copied text) |
| `cut()` | Simulate cut on focused element (dispatches cut event, clears field, returns cut text) |
| `dropFiles(x, y, paths)` | Simulate file drop at coordinates. `paths` is an array of file path strings. Dispatches dragenter → dragover → drop. |
| `dropText(x, y, text)` | Simulate text drop at coordinates. Dispatches dragenter → dragover → drop with text data. |
| `resize(w, h)` | Resize the virtual viewport |

Note: `el.click()` (DOM method) dispatches a click event directly on the element without hit testing. `click(x, y)` (headless global) goes through the full engine input pipeline with hit testing, focus, and bubbling — use this when testing user interactions.

### Settings

The `bro.settings` API is available in headless mode for reading and writing persistent engine settings (graphics, audio, input, action bindings). See [settings.md](settings.md) for the full API reference.

### CSS/Layout inspection

| Function | Description |
|----------|-------------|
| `inspect(selector [, verbose])` | Return formatted box model, position, computed styles, and DOM info. Pass `true` for verbose (all styles). See [inspect.md](inspect.md). |
| `inspectTree(selector [, depth])` | Return a tree view of element layout (sizes + positions). Default depth 3. Traverses shadow DOM. |
| `computedStyle(selector [, property])` | Return a specific computed style value (string), or all styles as a JS object. |
| `elements(selector)` | Return a summary of all matching elements with sizes and positions. |

## Examples

### Interactive REPL

```
> bro-headless ../broworkshop/demos/example

bro> document.querySelector('#sidebar').className
sidebar hidden-left

bro> inspect('body')
<BODY>
  Box Model:
    content:  1920 x 1080
    ...

bro> screenshot('dashboard.png')

bro> quit
```

### Inline expressions

```bash
bro-headless ../broworkshop/demos/example -e "advanceTime(2000)" -e "screenshot('out.png')"
```

Multiple `-e` flags are concatenated and evaluated together.

### Script file

`test.js`:
```js
advanceTime(2000);
assert(document.querySelector('#sidebar') !== null, 'sidebar should exist');
screenshot('after.png');
```

```bash
bro-headless ../broworkshop/demos/example test.js
```

Exit code is 0 on success, 1 if any assertion fails or an uncaught exception occurs.

### Top-level await

Scripts support top-level `await` (detected automatically when code contains the `await` keyword):

```js
let resp = await fetch('data.json');
let data = await resp.json();
assert(data.items.length > 0, 'data loaded');
screenshot('loaded.png');
```

## Integration tests

The `tests/` directory contains integration tests that exercise the engine via `bro-headless --no-gpu`. Run them with:

```bash
bash tests/run_tests.sh          # run all tests
bash tests/run_tests.sh events   # filter by substring
```

Each test is a self-contained JS file that manipulates the DOM and uses `assert()` to verify behavior. The runner discovers all `tests/*/test_*.js` files, runs each against the minimal `tests/test_app/` HTML page, and reports pass/fail with a summary.

### Test categories

| Directory | What it tests |
|-----------|---------------|
| `tests/dom/` | createElement, appendChild/removeChild, innerHTML, textContent, querySelector, attributes, classList, dataset, cloneNode |
| `tests/events/` | click dispatch, event bubbling, stopPropagation, preventDefault, addEventListener/removeEventListener |
| `tests/style/` | Inline style get/set, getComputedStyle |
| `tests/layout/` | getBoundingClientRect, offsetWidth/Height/Left/Top, clientWidth/Height |
| `tests/timers/` | setTimeout, setInterval, requestAnimationFrame (all with virtual time via advanceTime) |
| `tests/shadow_dom/` | attachShadow, shadow root querySelector, slot distribution |
| `tests/custom_elements/` | customElements.define, lifecycle callbacks (connected/disconnected), observedAttributes + attributeChangedCallback |
| `tests/gc/` | Orphan element cleanup after innerHTML removal, rapid create/remove cycles |

### Writing tests

Tests run against `tests/test_app/` which provides a minimal HTML page with a `<div id="root">` and a reset stylesheet (`* { margin: 0; box-sizing: border-box }`). Tests create DOM elements dynamically, use `flush()` to trigger layout, and `assert()` to verify:

```js
// tests/dom/test_example.js
const root = document.getElementById('root');
root.innerHTML = '<div id="box" style="width:100px;height:50px;">hello</div>';
flush();

const box = document.getElementById('box');
assert(box !== null, 'element exists');
assert(box.textContent === 'hello', 'text content matches');

const rect = box.getBoundingClientRect();
assert(Math.abs(rect.width - 100) < 1, 'width is ~100');

// Cleanup so state doesn't leak to other tests
root.innerHTML = '';
```

For input testing, use coordinate-based functions (`click`, `mouseDown`, etc.) which go through the full hit-testing pipeline:

```js
root.innerHTML = '<div id="btn" style="width:100px;height:50px;">Click me</div>';
flush();

let clicked = false;
document.getElementById('btn').addEventListener('click', () => { clicked = true; });
click(50, 25);
assert(clicked, 'click handler fired');
```

For timer testing, use `advanceTime()` to deterministically advance the virtual clock:

```js
let fired = false;
setTimeout(() => { fired = true; }, 100);
advanceTime(150);
assert(fired, 'timer fired after advancing past deadline');
```

## Architecture

Headless mode shares the same `Engine` class as windowed mode, configured via `EngineConfig` with `DisplayMode::Headless`. The JS runtime is the same QuickJS instance that runs app code — headless globals (`screenshot`, `advanceTime`, etc.) are installed as additional bindings after `engine.run()` returns (which performs initial layout and returns immediately in headless mode).

### GPU mode (default)

- Creates a hidden SDL window (`SDL_WINDOW_HIDDEN`) for a real OpenGL 3.3 context
- Uses `SkiaRenderer` — same GPU-accelerated Skia backend as windowed mode
- Full WebGL2 support — Three.js, raw WebGL, and other GL frameworks work
- Canvas 2D uses GL scene layers, composited in the screenshot pipeline
- Screenshots replicate the windowed compositing pass: scene layers rendered to an offscreen FBO, UI overlay composited on top with premultiplied alpha, then read back via `glReadPixels`
- Text metrics use Skia with platform-native fonts (DirectWrite on Windows, FreeType/fontconfig on Linux) — pixel-identical to windowed rendering

### CPU mode (`--no-gpu`)

- No window, no SDL video subsystem, no OpenGL context
- Uses `RasterRenderer` — CPU-only Skia with real platform-native fonts
- Canvas 2D rendered via software command replay
- No WebGL support (apps fall back gracefully)
- Screenshots captured directly from the Skia raster surface
- Input simulation (click, mouseDown, etc.) works fully — hit testing, event dispatch, focus management all function without a GPU

### Virtual time

Time does not advance automatically in headless mode. Use `advanceTime(ms)` to advance the virtual clock, which:

- Advances in 16ms steps (matching ~60fps frame cadence)
- Ticks `setTimeout` / `setInterval` callbacks
- Fires `requestAnimationFrame` callbacks (with WebGL canvas FBO bound when applicable)
- Runs pending JS microtasks (promises)
- Pumps fetch requests (brokit HTTP)
- Ticks worker threads
- Re-layouts the DOM if dirty
- Runs periodic GC (~every 1s of virtual time)
- Pumps the audio DSP pipeline (headless audio frames matching the time step)

Virtual time starts from the wall clock at engine initialization. The timer subsystem is seeded with this time at startup so that `setTimeout`/`setInterval` registered during script execution fire correctly relative to `advanceTime()` calls.

## Notes

- `[INFO]` and `[console.log]` lines go to stderr; REPL output and `-e` print results go to stdout. Separate them with `2>/dev/null`.
- Screenshots are PNG format.
- The default viewport is 1920x1080. Override with `--width` and `--height`.
- Audio engine runs in headless mode (no audio device output). `advanceTime()` pumps the audio DSP pipeline, so voices, effects, sequencer, metering, recording, and FFT analysis all work. Use `getBusPeakL/R()`, `getBusRmsL/R()`, `getSpectrum()`, and `stopRecording()` to inspect audio output numerically.
- In the REPL, the prompt (`bro>`) is printed to stderr so it doesn't contaminate piped output.
- The REPL supports `quit` and `exit` to terminate.
