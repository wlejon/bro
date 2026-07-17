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
| `touchDown(id, x, y [, pressure])` | Simulate a finger landing. `id` is a caller-chosen contact id (reuse it for the move/up/cancel of the same finger; distinct concurrent ids are distinct fingers). Dispatches pointerdown (pointerType `"touch"`, unique pointerId ≥ 2) then touchstart. See [pointer-api.js](pointer-api.js). |
| `touchMove(id, x, y [, pressure])` | Move a live contact. Dispatches pointermove then touchmove. Travelling past the ~10px tap slop makes the contact a drag (no compat click on lift). |
| `touchUp(id, x, y)` | Lift a contact. Dispatches pointerup then touchend; a clean primary-finger tap then synthesizes the compat mousedown → mouseup → click. |
| `touchCancel(id, x, y)` | Abort a contact (the OS-cancelled-gesture path). Dispatches pointercancel then touchcancel, releases any pointer capture, never synthesizes compat mouse events. |
| `keyDown(keycode [, scancode, mod, repeat])` | Simulate a key press (SDL keycodes) |
| `keyUp(keycode [, scancode, mod])` | Simulate a key release |
| `textInput(text)` | Simulate text input (for typing into focused input/textarea) |
| `imeCompose(text [, cursorPos])` | Simulate an IME composition (preedit) update on the focused input/textarea — the same engine path as `SDL_EVENT_TEXT_EDITING`. The preedit shows inline in `.value` as underlined provisional text; `cursorPos` is the composition cursor in characters within `text` (default: end). Fires `compositionstart` (first call) / `compositionupdate` and `input` with `inputType: "insertCompositionText"`. |
| `imeCommit(text)` | Commit the composition with `text` (the same path as a real IME's `SDL_EVENT_TEXT_INPUT`): replaces the preedit, fires the final `compositionupdate` → `input` → `compositionend`, and records ONE undo entry for the whole composition. Without an active composition it behaves like `textInput(text)`. |
| `imeCancel()` | Cancel the composition (an empty editing event): removes the preedit, restores the pre-composition value/selection, fires `compositionupdate("")` → `input` → `compositionend("")`, leaves no undo entry. |
| `paste(text)` | Simulate paste on focused element with the given text (dispatches paste event, inserts into input/textarea) |
| `copy()` | Simulate copy on focused element (dispatches copy event, returns the selected text — a collapsed caret copies nothing) |
| `cut()` | Simulate cut on focused element (dispatches cut event, removes the selected range, returns the cut text) |
| `dropFiles(x, y, paths)` | Simulate file drop at coordinates. `paths` is an array of file path strings. Dispatches dragenter → dragover → drop. |
| `dropText(x, y, text)` | Simulate text drop at coordinates. Dispatches dragenter → dragover → drop with text data. |
| `resize(w, h)` | Resize the virtual viewport |
| `gamepadConnect([id])` | Connect a virtual gamepad; returns its slot index. Fires `gamepadconnected` on window, appears in `navigator.getGamepads()`. |
| `gamepadDisconnect(index)` | Disconnect a virtual gamepad. Fires `gamepaddisconnected`. |
| `gamepadButton(index, button, pressed [, value])` | Set a virtual pad's button. `button` is a W3C index (0-16) or name (`"south"`, `"start"`, `"lefttrigger"`, ...). `value` gives triggers an analog level (defaults to pressed ? 1 : 0). Press/release edges dispatch bound `"action"` events. |
| `gamepadAxis(index, axis, value)` | Set a virtual pad's stick axis. `axis` is 0-3 or `"leftx"`/`"lefty"`/`"rightx"`/`"righty"`; value -1..1. |

Note: `el.click()` (DOM method) dispatches a click event directly on the element without hit testing. `click(x, y)` (headless global) goes through the full engine input pipeline with hit testing, focus, and bubbling — use this when testing user interactions.

### Text editing undo/redo

Every `<input>` (text-like types) and `<textarea>` keeps its own undo/redo
history for user edits, matching standard editor behavior:

- **Keys**: Ctrl+Z undoes; Ctrl+Y and Ctrl+Shift+Z both redo (Cmd variants on
  macOS). The keys act on the focused editing element only. Undo on an empty
  stack, or redo after a fresh edit (which clears the redo tail), is a no-op.
- **Granularity**: a run of consecutive typed characters at the caret is one
  undo step, as is a run of backspaces or forward-deletes. A run is broken by
  any caret/selection move (arrows, mouse, `setSelectionRange`), focus loss,
  or a >1 s typing pause. Paste, cut, typing over a selection, Enter in a
  textarea, and number-spinner steps are each their own step.
- **Selection**: undo restores the text *and* the caret/selection exactly as
  they were before the edit; redo restores the post-edit selection.
- **Events**: undo/redo fire the normal `input` event with
  `inputType: "historyUndo"` / `"historyRedo"`. A paste inserted through the
  engine reports `inputType: "insertFromPaste"`.
- **Programmatic writes clear history**: setting `.value =` from JS drops that
  element's undo and redo stacks (browser behavior) — Ctrl+Z cannot cross a
  script's rewrite of the field.
- **Caps**: ~200 entries or ~1 MB of edit text per element; oldest entries
  drop first.

In headless scripts, drive it with `textInput(...)`, `keyDown/keyUp` (e.g.
`keyDown(122 /* z */, 0, 0x0040 /* LCTRL */)`), `paste(...)`, and `cut()`.

### IME composition

CJK (and dead-key) input composes through `imeCompose`/`imeCommit`/`imeCancel`
(table above). Semantics match browsers: the preedit is visible in `.value`
during composition (rendered with an underline and the composition-cursor
caret), a commit is one discrete undo entry from the pre-composition state, a
cancel restores it and leaves no entry, and anything that moves the caret or
focus mid-composition (mouse press, arrow/command keys, Tab, `.blur()`)
**commits** the current preedit rather than stranding it. Composition offsets
follow the controls' byte-offset convention (`selectionStart`/`selectionEnd`
are UTF-8 byte offsets).

### Settings

The `bro.settings` API is available in headless mode for reading and writing persistent engine settings (graphics, audio, input, action bindings). See [settings.md](settings.md) for the full API reference.

### CSS/Layout inspection

| Function | Description |
|----------|-------------|
| `inspect(selector [, verbose])` | Return formatted box model, position, computed styles, and DOM info. Pass `true` for verbose (all styles). See [inspect.md](inspect.md). |
| `inspectTree(selector [, depth])` | Return a tree view of element layout (sizes + positions). Default depth 3. Traverses shadow DOM. |
| `computedStyle(selector [, property])` | Return a specific computed style value (string), or all styles as a JS object. |
| `elements(selector)` | Return a summary of all matching elements with sizes and positions. |

### Performance

`perf` measures what a change costs the style and layout passes — the two that a
DOM update actually pays for, and the two a screenshot can't show you.

| Function | Description |
|----------|-------------|
| `perf.now()` | Real wall-clock milliseconds. **Use this, not `performance.now()`** — that one rides virtual time (see below) and reports 0ms for work that took a second. |
| `perf.reset()` | Zero the counters. |
| `perf.stats()` | The counters since the last reset. |

`perf.stats()` returns, accumulated over every style/layout pass since the reset:

| Field | Meaning |
|-------|---------|
| `styleMs` | `resolveStyles()` — selector matching + the computed-style diff |
| `buildMs` | rebuilding the whole layout tree from the DOM |
| `invalidateMs` | carrying element dirt into the layout tree, including per-element subtree rebuilds |
| `layoutMs` | `layoutTree()` itself |
| `syncMs` | writing the resulting boxes back onto elements |
| `totalMs` | the sum of the above |
| `passes` | how many layout passes ran |
| `elementsStyled` | elements whose computed style was re-resolved |
| `nodesLaidOut` | layout nodes that ran a formatting context |
| `nodesReused` | layout nodes handed back from cache untouched |
| `measureCalls` | text measurements (shaping) requested |
| `styleLookups` | string-keyed style map lookups inside `layoutTree()` |
| `reuseFailDirty` | nodes re-laid because they (or a descendant) really changed |
| `reuseFailAvailW` | nodes re-laid because their available width differed from cache |
| `reuseFailAvailH` | nodes re-laid because their available height differed from cache |
| `reuseFailOverride` | nodes re-laid because their flex width override differed |
| `treeRebuilds` | layout subtrees rebuilt from the DOM |
| `scene` | 3D frustum-culling counters from the most recent frame, summed across all scene graphs: `{mesh,instanced,splat,particles,billboards,shadow}{Drawn,Culled}` (shadow counts are per caster × atlas tile). Per-graph numbers: `scene.cullStats()`; escape hatch: `scene.setFrustumCulling(false)`. See `docs/scene-api.js`. |

**The counts matter more than the milliseconds.** Layout and style are both
incremental: a change is supposed to cost time proportional to what it changed,
not to the size of the document. `nodesLaidOut` is what tells you whether that
actually happened. A change to one element that comes back having laid out
thousands of nodes has an *invalidation* bug — something marked more dirty than
it had to — and no amount of making layout faster will fix it. Likewise a large
`measureCalls` says the cost is text shaping, not boxes, and `styleLookups`
far out of proportion to `nodeVisits` says some pass is re-deriving style
across whole subtrees instead of touching only what changed. When
`nodesLaidOut` is high, the `reuseFail*` counters say why: `reuseFailDirty`
is real invalidation (look upstream at what got marked), while the other
three mean clean subtrees are being offered different layout inputs than
they were cached under — a cache-defeat inside layout itself.

```js
// What does one slider drag event cost?
const slider = document.querySelector('#gain');
perf.reset();
const t0 = perf.now();
for (let i = 0; i < 20; i++) {
  slider.value = String(i / 20);
  slider.dispatchEvent(new Event('input'));
  flush();                       // run the style + layout passes
}
const wall = (perf.now() - t0) / 20;
const p = perf.stats();
console.log(`${wall.toFixed(1)}ms/event, ${p.nodesLaidOut / 20} nodes laid out, ` +
            `${p.nodesReused / 20} reused`);
// 0.9ms/event, 54 nodes laid out, 1 reused   <- good: it only touched what changed
// 138ms/event, 3440 nodes laid out, 0 reused <- bad: it relaid out the document
```

Note `flush()` is what runs the passes, so a benchmark must call it inside the
loop; mutating the DOM ten times and flushing once measures one pass, not ten.

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

The `tests/` directory contains integration tests that exercise the engine via `bro-headless` on its default GPU path — the same renderer, WebGL, and layer compositing the shipping runtime uses. (`--no-gpu` selects the CPU raster fallback, which is a different code path; running the suite there would leave the real one untested.) Run them with:

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
- WebGL2 support — Three.js, raw WebGL, and other GL frameworks work (see the support matrix below for the exact API surface)
- Canvas 2D uses GL scene layers, composited in the screenshot pipeline
- Screenshots replicate the windowed compositing pass: scene layers rendered to an offscreen FBO, UI overlay composited on top with premultiplied alpha, then read back via `glReadPixels`
- Text metrics use Skia with platform-native fonts (DirectWrite on Windows, FreeType/fontconfig on Linux) — pixel-identical to windowed rendering

### WebGL2 support matrix

The `webgl2` context (src/webgl/ + src/js/webgl2_bindings*) maps WebGL2 onto
raw OpenGL 3.3 core. Behavioral tests live in `tests/webgl/`.

**Implemented:** context state + `getParameter`/`getError` (including
WebGL-only pixel-store pnames and synthetic errors); buffers with all
`bufferData`/`bufferSubData`/`getBufferSubData` signatures (element-unit
`srcOffset`/`length`), `copyBufferSubData`, all WebGL2 binding points and
`bindBufferBase`/`bindBufferRange`; VAOs, `vertexAttribIPointer`,
`vertexAttribDivisor` + instanced draws, `drawRangeElements`, all three index
types; shaders/programs with info logs, active attrib/uniform metadata,
uniform blocks (UBO), `getFragDataLocation`, and every uniform setter shape
(scalars, typed arrays, plain JS arrays, square + non-square matrices);
textures 2D / 3D / 2D-array / cube map, sized internal formats,
`texStorage2D/3D`, mipmaps, `UNPACK_ALIGNMENT`, `UNPACK_FLIP_Y_WEBGL`,
`UNPACK_PREMULTIPLY_ALPHA_WEBGL`; framebuffers/renderbuffers including
multisample + `blitFramebuffer` resolve, MRT via `drawBuffers` +
`readBuffer`, `readPixels` (with destination-size validation), float
color buffers (`EXT_color_buffer_float`).

**Not implemented (absent API families):** sampler objects, sync objects
(`fenceSync`/`clientWaitSync`), query objects, transform feedback objects,
PBO-offset variants of `readPixels`/texture uploads, `compressedTexImage*`,
`copyTexImage2D`/`copyTexSubImage2D`, `framebufferTextureLayer`,
`invalidateFramebuffer`/`invalidateSubFramebuffer`, unsigned-int uniform
setters (`uniform*ui*`), constant vertex attributes (`vertexAttrib[1-4]f*`,
`vertexAttribI4*`), and the introspection getters `getUniform`,
`getVertexAttrib`, `getTexParameter`, `getBufferParameter`,
`getRenderbufferParameter`, `getFramebufferAttachmentParameter`,
`getActiveUniformBlockParameter`/`Name`, `getUniformIndices`,
`getInternalformatParameter`, `validateProgram`, and the `is*` object
predicates.

**Known deviations:** `texStorage2D` is emulated with mutable storage
(`texImage2D` per level) so three.js's placeholder-then-allocate flow works —
immutability is not enforced and `TEXTURE_IMMUTABLE_FORMAT` reports as
mutable; `getParameter` object-binding queries (`CURRENT_PROGRAM`,
`ARRAY_BUFFER_BINDING`, `TEXTURE_BINDING_2D`, ...) return `null` rather than
the wrapper objects; `getShaderPrecisionFormat` returns fixed highp values.

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

### Waiting in scripts: pump, don't await

A script file with a top-level `await` is evaluated as an ES module, and while
its evaluation promise is pending the runner drains **microtasks only** — no
timers, no frame pumps. Anything delivered per-frame (`setTimeout`,
`bro.net` callbacks, worker messages, Steam events) can never fire during a
bare top-level `await`, so `await new Promise(r => setTimeout(r, ...))` hangs
forever. Wait with a synchronous pump loop instead:

```js
// advanceTime() drains the event queues and fires callbacks;
// wallSleep() gives real threads (network, child process, mic) wall-clock
// time to produce work. Neither alone is enough.
function pumpUntil(desc, fn, iters) {
    for (let i = 0; i < iters; i++) {
        advanceTime(16);
        wallSleep(16);
        if (fn()) return;
    }
    throw new Error('timeout waiting for ' + desc);
}
```

A long-lived headless server (e.g. a `bro.net.host` process) should end with
`for (;;) { advanceTime(16); wallSleep(16); }` and exit via `process.exit()`
from a callback. Promises resolved directly by async C++ APIs (model loaders,
inference calls) are the exception: those settle through the microtask queue,
so plain `await` works for them.

### location.reload() and the app realm

`location.reload()` works in headless mode, but its two contexts commit at
different points:

- **Inside an `<iframe>` sub-document** it queues a rebuild of that iframe
  (same deferred path as the host calling `frame.reload()`), and the queue is
  drained at the engine's safe point — which `flush()` reaches. A driving
  script can therefore observe it in-process: `advanceTime()` until the
  sub-doc calls reload, `flush()`, and the host sees another `load` event.

- **In the top-level document** it tears down the whole app document and its
  JS realm, then re-parses and re-runs the app in the same engine. In headless
  mode the driving script (REPL line, `-e` expression, or script file) runs
  *inside* that realm, so the reload can never commit while the script is
  still on the stack. It is drained **between evaluation units**: after engine
  construction (an app that reloads itself during its first run), after each
  `-e` expression, after a script file finishes, and between REPL lines. A
  single script file cannot observe its own top-level reload — the realm that
  would do the asserting is the one being replaced. To test it, let the app
  reload itself and pass a *second* script that runs in the fresh realm, or
  spawn a child `bro-headless` (see `tests/engine/test_location_reload_toplevel.js`).

Both contexts share the web semantics: the call is deferred (the calling
script runs to completion), multiple requests in one frame coalesce, scripts
re-execute fresh in a new realm, and the old realm's timers and listeners do
not survive the swap.

## Notes

- `[INFO]` and `[console.log]` lines go to stderr; REPL output and `-e` print results go to stdout. Separate them with `2>/dev/null`.
- Screenshots are PNG format.
- The default viewport is 1920x1080. Override with `--width` and `--height`.
- Audio engine runs in headless mode (no audio device output). `advanceTime()` pumps the audio DSP pipeline, so voices, effects, sequencer, metering, recording, and FFT analysis all work. Use `getBusPeakL/R()`, `getBusRmsL/R()`, `getSpectrum()`, and `stopRecording()` to inspect audio output numerically.
- In the REPL, the prompt (`bro>`) is printed to stderr so it doesn't contaminate piped output.
- The REPL supports `quit` and `exit` to terminate.
