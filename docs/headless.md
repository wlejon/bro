# bro-headless

Headless mode for bro — runs the full engine pipeline (GPU rendering, real fonts, WebGL) without a visible window. Captures pixel-perfect screenshots, manipulates the DOM, and drives apps via text commands. Built for automated testing, visual regression, and scripted automation.

## Usage

```
bro-headless [--no-gpu] <app-directory> [script.txt]
```

- No script argument = **interactive mode** (reads from stdin)
- With script argument = **script mode** (runs commands from file, then exits)
- Piping works: `echo "dump #btn" | bro-headless apps/hello`

### Flags

| Flag | Description |
|------|-------------|
| `--no-gpu` | Disable GPU rendering. Uses CPU-only Skia rasterizer (no WebGL, no scene layer compositing). For CI environments without a GPU. |

By default, headless uses a hidden SDL window with a full OpenGL context — the same rendering pipeline as windowed mode, including GPU-accelerated Skia, WebGL2, and Canvas 2D scene layers.

## Commands

| Command | Description |
|---------|-------------|
| `dump` | Print full DOM as HTML |
| `dump <selector>` | Print a single element's outer HTML |
| `diff` | Show line-by-line diff since last `dump` |
| `click <selector>` | Simulate a click event on the element. Prints `[changed]` if the DOM was modified |
| `eval <js>` | Evaluate JavaScript and print the result |
| `wait <ms>` | Advance virtual time by N milliseconds (fires timers, rAF callbacks, and pending JS jobs) |
| `screenshot <path>` | Render the current frame to a PNG file. Composites scene layers (WebGL, Canvas 2D) with the HTML/CSS UI overlay. |
| `rect <selector>` | Print the element's layout box (`x`, `y`, `w`, `h`) |
| `system` | Toggle the system overlay (performance panel) |
| `system perf <metrics>` | Push performance data to the system overlay |
| `help` | Print command reference |
| `quit` / `exit` | Exit |
| `# comment` | Ignored (for script files) |

Selectors: `#id` for getElementById, or any CSS selector supported by htmlayout.

## Examples

### Interactive

```
> bro-headless apps/hello

bro headless> dump #counter
<div id="counter">Count: 0</div>

bro headless> click #btn
[changed]

bro headless> dump #counter
<div id="counter">Count: 1</div>

bro headless> eval document.getElementById("counter").textContent
Count: 1

bro headless> screenshot hello.png
[headless] saved screenshot to hello.png

bro headless> rect #btn
x=20 y=109.84 w=61.3359 h=19.2
```

### Piped

```
echo -e "dump #counter\nclick #btn\ndump #counter\nquit" | bro-headless apps/hello 2>/dev/null
```

Output:
```
<div id="counter">Count: 0</div>
[changed]
<div id="counter">Count: 1</div>
```

### Script file

`test.txt`:
```
# Initial state
dump #counter
screenshot before.png

# Click the button 3 times
click #btn
click #btn
click #btn

# Check result
dump #counter
screenshot after.png
diff
quit
```

```
bro-headless apps/hello test.txt
```

### Visual regression with WebGL

```
# Load a Three.js app, capture initial state
wait 500
screenshot baseline.png

# Change geometry and capture
click #btn-sphere
wait 200
screenshot sphere.png

# Change material color
click #btn-color-red
wait 200
screenshot red-sphere.png
quit
```

### Diff

`diff` compares the current DOM to the state at the last `dump` call:

```
bro headless> dump
bro headless> click #btn
bro headless> click #btn
bro headless> diff
- ...<p id="message">Click the button to get started.</p>...<div id="counter">Count: 0</div>...
+ ...<p id="message">You clicked 2 times!</p>...<div id="counter">Count: 2</div>...
```

## Architecture

Headless mode shares the same `Engine` class as windowed mode, configured via `EngineConfig` with `DisplayMode::Headless`. The `HeadlessController` is a thin command layer that drives the engine.

### GPU mode (default)

- Creates a hidden SDL window (`SDL_WINDOW_HIDDEN`) for a real OpenGL 3.3 context
- Uses `SkiaRenderer` — same GPU-accelerated Skia backend as windowed mode
- Full WebGL2 support — Three.js, raw WebGL, and other GL frameworks work
- Canvas 2D uses GL scene layers, composited in the screenshot pipeline
- Screenshots replicate the windowed compositing pass: scene layer rendered to an offscreen FBO, UI overlay composited on top with premultiplied alpha, then read back via `glReadPixels`
- Text metrics use Skia with DirectWrite — pixel-identical to windowed rendering

### CPU mode (`--no-gpu`)

- No window, no OpenGL context
- Uses `RasterRenderer` — CPU-only Skia with real DirectWrite fonts
- Canvas 2D rendered via software command replay
- No WebGL support (apps fall back gracefully)
- Screenshots captured directly from the Skia raster surface

### Virtual time

Time does not advance automatically in headless mode. Use `wait <ms>` to advance the virtual clock, which:

- Advances in 16ms steps (matching ~60fps frame cadence)
- Ticks `setTimeout` / `setInterval` callbacks
- Fires `requestAnimationFrame` callbacks (with WebGL canvas FBO bound when applicable)
- Runs pending JS microtasks (promises)
- Re-layouts the DOM if dirty
- Runs periodic GC

Virtual time starts from the wall clock at initialization, so timers created during script execution work correctly.

## Notes

- `[INFO]` and `[console.log]` lines go to stderr; command output goes to stdout. Separate them with `2>/dev/null`.
- Screenshots are PNG format (typically 30-50KB for 1024x768).
- The default viewport is 1024x768.
- Audio bindings are installed but non-functional (AudioContext constructor throws; apps should catch and degrade gracefully).
