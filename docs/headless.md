# bro-headless

Headless mode for bro — runs the full engine pipeline (GPU rendering, real fonts, WebGL) without a visible window. Driven entirely by JavaScript: write test scripts, automate screenshots, and manipulate the DOM using the same language your apps are written in.

## Usage

```
bro-headless [--no-gpu] <app-directory> [script.js | -e "expr" ...]
```

- No arguments after app directory = **interactive JS REPL**
- With `.js` file = **script mode** (runs file, then exits)
- With `-e` flags = **inline mode** (evaluates expressions, then exits)

### Flags

| Flag | Description |
|------|-------------|
| `--no-gpu` | Disable GPU rendering. Uses CPU-only Skia rasterizer (no WebGL, no scene layer compositing). For CI environments without a GPU. |

By default, headless uses a hidden SDL window with a full OpenGL context — the same rendering pipeline as windowed mode, including GPU-accelerated Skia, WebGL2, and Canvas 2D scene layers.

## Headless globals

These functions are available in addition to all standard DOM APIs:

| Function | Description |
|----------|-------------|
| `advanceTime(ms)` | Advance virtual time by N milliseconds (fires timers, rAF callbacks, and pending JS jobs) |
| `sleep(ms)` | Alias for `advanceTime` |
| `screenshot(path)` | Render the current frame to a PNG file. Composites scene layers (WebGL, Canvas 2D) with the HTML/CSS UI overlay. Throws on failure. |
| `screenshot(path, selector)` | Render the current frame and crop to the element's bounding box before saving. |
| `flush()` | Force layout recalculation (called automatically after `advanceTime`) |
| `assert(condition, message?)` | Throw if condition is falsy. Failed assertions produce a nonzero exit code. |
| `click(x, y [, button])` | Simulate a mouse click at screen coordinates (mousedown + mouseup through full engine pipeline with hit testing) |
| `mouseDown(x, y [, button])` | Simulate a mouse button press at screen coordinates |
| `mouseUp(x, y [, button])` | Simulate a mouse button release at screen coordinates |
| `mouseMove(x, y)` | Simulate mouse movement to screen coordinates (triggers hover, mousemove events) |
| `wheel(x, y, deltaY [, deltaX])` | Simulate a mouse wheel event at screen coordinates (deltaY in scroll lines) |
| `keyDown(keycode [, scancode, mod, repeat])` | Simulate a key press (SDL keycodes) |
| `keyUp(keycode [, scancode, mod])` | Simulate a key release (SDL keycodes) |
| `textInput(text)` | Simulate text input (for typing into focused input/textarea) |
| `resize(w, h)` | Resize the virtual viewport |
| `inspect(selector [, verbose])` | Return formatted box model, position, computed styles, and DOM info for an element. Pass `true` for verbose (all styles). See [inspect.md](inspect.md). |
| `inspectTree(selector [, depth])` | Return a tree view of element layout (sizes + positions). Default depth 3. Traverses shadow DOM. |
| `computedStyle(selector [, property])` | Return a specific computed style value (string), or all styles as a JS object if no property given. |
| `elements(selector)` | Return a summary of all matching elements with sizes and positions. |

All standard DOM APIs work as expected: `document.querySelector()`, `el.click()`, `el.textContent`, `el.innerHTML`, `el.outerHTML`, `el.getBoundingClientRect()`, `el.style`, event dispatch, etc. Note that `el.click()` dispatches a click event directly on the element (no hit testing), while `click(x, y)` goes through the full engine input pipeline with hit testing, focus management, and event bubbling.

## Examples

### Interactive REPL

```
> bro-headless apps/hello

bro> document.querySelector('#counter').textContent
Count: 0

bro> document.querySelector('#btn').click()

bro> document.querySelector('#counter').textContent
Count: 1

bro> screenshot('hello.png')

bro> document.querySelector('#btn').getBoundingClientRect()
[object Object]

bro> quit
```

### Inline expressions

```bash
bro-headless apps/hello -e "document.querySelector('#btn').click()" -e "screenshot('out.png')"
```

Multiple `-e` flags are concatenated and evaluated together.

### Script file

`test.js`:
```js
const btn = document.querySelector('#btn');
const counter = document.querySelector('#counter');

screenshot('before.png');

for (let i = 0; i < 3; i++) {
    btn.click();
    advanceTime(16);
}

assert(counter.textContent === 'Count: 3', 'counter should be 3 after 3 clicks');
screenshot('after.png');
```

```bash
bro-headless apps/hello test.js
```

Exit code is 0 on success, 1 if any assertion fails or an uncaught exception occurs.

### Visual regression with WebGL

```js
// Load a Three.js app, let it render
advanceTime(500);
screenshot('baseline.png');

// Change geometry
document.querySelector('#btn-sphere').click();
advanceTime(200);
screenshot('sphere.png');

// Change material color
document.querySelector('#btn-color-red').click();
advanceTime(200);
screenshot('red-sphere.png');
```

### Top-level await

Scripts support top-level `await`:

```js
await advanceTime(1000);
screenshot('after-delay.png');
```

## Architecture

Headless mode shares the same `Engine` class as windowed mode, configured via `EngineConfig` with `DisplayMode::Headless`. The JS runtime is the same QuickJS instance that runs app code — headless globals (`screenshot`, `advanceTime`, etc.) are installed as additional bindings.

### GPU mode (default)

- Creates a hidden SDL window (`SDL_WINDOW_HIDDEN`) for a real OpenGL 3.3 context
- Uses `SkiaRenderer` — same GPU-accelerated Skia backend as windowed mode
- Full WebGL2 support — Three.js, raw WebGL, and other GL frameworks work
- Canvas 2D uses GL scene layers, composited in the screenshot pipeline
- Screenshots replicate the windowed compositing pass: scene layer rendered to an offscreen FBO, UI overlay composited on top with premultiplied alpha, then read back via `glReadPixels`
- Text metrics use Skia with platform-native fonts (DirectWrite on Windows, FreeType/fontconfig on Linux) — pixel-identical to windowed rendering

### CPU mode (`--no-gpu`)

- No window, no OpenGL context
- Uses `RasterRenderer` — CPU-only Skia with real platform-native fonts
- Canvas 2D rendered via software command replay
- No WebGL support (apps fall back gracefully)
- Screenshots captured directly from the Skia raster surface

### Virtual time

Time does not advance automatically in headless mode. Use `advanceTime(ms)` to advance the virtual clock, which:

- Advances in 16ms steps (matching ~60fps frame cadence)
- Ticks `setTimeout` / `setInterval` callbacks
- Fires `requestAnimationFrame` callbacks (with WebGL canvas FBO bound when applicable)
- Runs pending JS microtasks (promises)
- Re-layouts the DOM if dirty
- Runs periodic GC

Virtual time starts from the wall clock at initialization, so timers created during script execution work correctly.

## Notes

- `[INFO]` and `[console.log]` lines go to stderr; REPL output goes to stdout. Separate them with `2>/dev/null`.
- Screenshots are PNG format (typically 30-50KB for 1024x768).
- The default viewport is 1024x768.
- Audio engine runs in headless mode (no audio device output). `advanceTime()` pumps the audio DSP pipeline, so voices, effects, sequencer, metering, recording, and FFT analysis all work. Use `getBusPeakL/R()`, `getBusRmsL/R()`, `getSpectrum()`, and `stopRecording()` to inspect audio output numerically.
- In the REPL, the prompt (`bro>`) is printed to stderr so it doesn't contaminate piped output.
