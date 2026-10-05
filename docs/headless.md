# bro-headless

Headless mode for bro runs the full engine pipeline (GPU rendering, real fonts, WebGL) without a visible window. Driven entirely by JavaScript: write test scripts, automate screenshots, and manipulate the DOM using the same language your apps are written in.

## Usage

```
bro-headless [--no-gpu] [--width N] [--height N] <app-directory> [script.js | -e "expr" ...]
```

- With `.js` file = **script mode** (runs file, then exits)
- With `-e` flags = **inline mode** (evaluates expressions, then exits)
- No script and no `-e` = the app boots, runs its own scripts, and exits

Scripts and `-e` expressions are compiled in-process by bronze and run against the live engine; there is no interpreter and no interactive REPL.

### Flags

| Flag | Description |
|------|-------------|
| `--no-gpu` | Disable GPU rendering. Uses CPU-only Skia rasterizer (no WebGL, no scene layer compositing). For CI environments without a GPU. |
| `--audio` | Open the real SDL audio device and mic. Off by default: headless runs the DSP graph with no device attached, so a test never claims the machine's sound hardware. |
| `--width N` | Viewport width in pixels (default: 1920) |
| `--height N` | Viewport height in pixels (default: 1080) |
| `--device-scale-factor S` | Render as a HiDPI display with `S` device px per CSS px (default 1): `devicePixelRatio`, `@media (resolution)` and every captured frame follow, layout and input stay in CSS px. Same as calling `setDeviceScaleFactor(S)` first. Needs the GPU path; `--no-gpu` reports the ratio but rasterizes 1:1. |
| `--splash` | Show the startup splash. Off by default in headless: its canvas animation leaks into early screenshots; opt in only to exercise the splash lifecycle. |
| `--no-splash` | Explicitly disable the splash (the default). |
| `--print-host-globals` | Install the host globals exactly as a run would, print bronze's host-global registry to stdout one name per line, and exit: the `--host-globals` manifest for `bronze build` of an app that will run on this binary. |
| `--print-native-manifest <path>` | The other half of the same contract: write the native registry — the `__bro_native.*` entry points and signatures behind `bro.time`, `bro.settings`, `bro.window` and the panels' `__bro.*` — as the JSON `--native-manifest` for the same compile, and exit. Combines with `--print-host-globals` in one run; `tests/bronze_host/lib.sh` asks for both that way. |

By default, headless uses headless offscreen Vulkan rendering directly on the GPU without requiring Xvfb on Linux, running the same rendering pipeline as windowed mode, including GPU-accelerated Skia, WebGL2, and 3D scene layers.

## Headless globals

These functions are available in addition to all standard DOM APIs:

### Core

| Function | Description |
|----------|-------------|
| `advanceTime(ms)` | Advance virtual time by N milliseconds (fires timers, rAF callbacks, and pending JS jobs) |
| `sleep(ms)` | Alias for `advanceTime` |
| `wallSleep(ms)` | Block for N milliseconds of *real* wall-clock time without advancing virtual time. Gives real threads (network, child process, mic) time to produce work; pair it with `advanceTime()` to deliver that work into JS (see "Waiting in scripts" below). |
| `flush()` | Force layout recalculation (called automatically after `advanceTime`) |
| `assert(condition, message?)` | Throw if condition is falsy. Failed assertions produce a nonzero exit code. |
| `skipTest(reason)` | This environment cannot test the script's subject (weights absent, feature compiled out): the run exits 77, which `tests/run_tests.sh` reports as SKIP — never as a pass. Something that also failed still fails the run. The script keeps running, so do nothing further after it. |
| `missingGpuContext(kind)` | Call when `getContext('webgl2')` / `getContext('scene')` returned null. On a run with no GPU device (`--no-gpu`) or with the feature compiled out it skips like `skipTest`; on a GPU run a null context is a bug, and it fails the run. |
| `writeFile(path, data)` | Write `data` to `path` and return the byte count. A string is written as UTF-8; an `ArrayBuffer` or TypedArray is written verbatim (a view writes only its own range, not the whole buffer). Missing parent directories are created, like `screenshot()`. Throws on anything else object-shaped, and on an open/write failure, naming the path. Headless-only, so a bake/export tool can drop an asset next to its test; a shipped app gets no filesystem write from this. |

### Screenshots

| Function | Description |
|----------|-------------|
| `screenshot(path)` | Render the current frame to a PNG file. Composites scene layers (WebGL, Canvas 2D) with the HTML/CSS UI overlay. Missing parent directories are created, so a test can dump into `tests/out/` without anything having made it first. Throws on failure, naming the path. |
| `screenshot(path, selector)` | Render the current frame and crop to the element's bounding box before saving. Bounding box uses viewport-relative coords (matches `getBoundingClientRect`); at a device scale above 1 the crop, like the whole frame, is in device px. Transparent canvas pixels flatten to opaque black; for alpha-preserving canvas exports use `screenshotCanvas`. |
| `screenshotCanvas(path, selector)` | Snapshot a `<canvas>` element's underlying Skia surface directly to PNG, preserving alpha. Selector must point to a 2D canvas (not WebGL or scene). |
| `getPixel(x, y)` | Return `{r, g, b, a}` for the pixel at **document** coordinates, the same space `getBoundingClientRect()` reports in, so a probe can be compared against a measured rect with no inset arithmetic. Renders the full composited frame (HTML, Canvas, WebGL, scene) and reads back the pixel. Engine chrome (menu bar, docked inspector) insets the document within the frame; `getPixel` folds that in for you. Out-of-document coordinates return all zeroes. |
| `getPixels(x, y, w, h)` | A `w`×`h` block of `getPixel` probes from **one** composite, as `{width, height, data}` with `data` a `Uint8ClampedArray` of RGBA (ImageData-shaped). Same document coordinates and sampling as `getPixel`; texels outside the document are zeroes. Use it for any scan: each `getPixel` call composites and reads back the whole window, so a loop of a few thousand probes costs a few thousand frames (seconds on a GPU, minutes on software GL). |
| `getFramePixel(x, y)` | Same readback in **frame** coordinates: the whole composited window including engine chrome. Only needed when asserting something about the chrome itself (e.g. that the menu bar occupies the top strip); app content is easier to probe with `getPixel`. |

### Input simulation

All input functions go through the full engine pipeline (hit testing, focus management, event dispatch, bubbling). After each call, `flush()` is called automatically to process pending JS jobs.

Mouse coordinates are **viewport-relative**, matching `getBoundingClientRect()` / `clientX` / `clientY`. The engine reserves a top inset for the menu bar (~28px); these helpers add it internally so `click(rect.x, rect.y)` Just Works without offset math.

**Targeting a secondary window.** Every input helper below takes an optional
trailing `windowId`. Omitted (or `0`) means the main window, so existing
scripts are unaffected. Pass a `bro.window.open()` handle's `.id` to route the
event into THAT window's document instead:

```js
const win = bro.window.open('palette', { width: 300, height: 200 });
flush();
click(50, 30, 0, win.id);       // clicks inside the palette window
textInput('hi', win.id);        // types into its focused control
wheel(250, 40, 120, 0, win.id); // scrolls its overflow box
currentCursor(win.id);          // that window's resolved cursor
```

A secondary window has no menu-bar inset and no engine viewport scroll, so its
coordinates are plain window coordinates (which are also its document
coordinates), the top-inset adjustment is skipped for them. Realms are
isolated, so the child's side of an interaction is observed through
`win.capture()`. An unknown or already-closed id is a silent no-op; it never
falls back to the main window. v1 keeps pointer lock, touch, and the
gamepad/`"action"` stream on the main window (see
[window-api.js](window-api.js)).

| Function | Description |
|----------|-------------|
| `click(x, y [, button, windowId])` | Simulate a mouse click at viewport coordinates (mousedown + mouseup) |
| `mouseDown(x, y [, button, windowId])` | Simulate a mouse button press |
| `mouseUp(x, y [, button, windowId])` | Simulate a mouse button release |
| `mouseMove(x, y [, windowId])` | Simulate mouse movement (triggers hover, mousemove events) |
| `currentCursor([windowId])` | Resolved OS cursor shape name for that window's current hover target (`"default"`, `"pointer"`, `"text"`, `"move"`, `"crosshair"`, `"wait"`, `"progress"`, `"not-allowed"`, `"ew-resize"`, `"ns-resize"`, `"nesw-resize"`, `"nwse-resize"`, `"none"`). Updated by every `mouseMove()` from the hovered element's computed CSS `cursor`; in windowed mode the same shape drives that window's real OS cursor. Each window resolves independently, a `cursor: pointer` element in a secondary window never changes what `currentCursor()` reports for the main one. |
| `wheel(x, y, deltaY [, deltaX, windowId])` | Simulate a mouse wheel event (deltaY in scroll lines) |
| `touchDown(id, x, y [, pressure])` | Simulate a finger landing. `id` is a caller-chosen contact id (reuse it for the move/up/cancel of the same finger; distinct concurrent ids are distinct fingers). Dispatches pointerdown (pointerType `"touch"`, unique pointerId ≥ 2) then touchstart. See [pointer-api.js](pointer-api.js). |
| `touchMove(id, x, y [, pressure])` | Move a live contact. Dispatches pointermove then touchmove. Travelling past the ~10px tap slop makes the contact a drag (no compat click on lift). |
| `touchUp(id, x, y)` | Lift a contact. Dispatches pointerup then touchend; a clean primary-finger tap then synthesizes the compat mousedown → mouseup → click. |
| `touchCancel(id, x, y)` | Abort a contact (the OS-cancelled-gesture path). Dispatches pointercancel then touchcancel, releases any pointer capture, never synthesizes compat mouse events. |
| `keyDown(keycode [, scancode, mod, repeat, windowId])` | Simulate a key press (SDL keycodes) |
| `keyUp(keycode [, scancode, mod, windowId])` | Simulate a key release |
| `textInput(text [, windowId])` | Simulate text input (for typing into focused input/textarea) |
| `imeCompose(text [, cursorPos, windowId])` | Simulate an IME composition (preedit) update on the focused input/textarea, or on a contenteditable host when the DOM Selection caret sits inside one, the same engine path as `SDL_EVENT_TEXT_EDITING`. The preedit shows inline in `.value` as underlined provisional text; `cursorPos` is the composition cursor in characters within `text` (default: end). Fires `compositionstart` (first call) / `compositionupdate` and `input` with `inputType: "insertCompositionText"`. |
| `imeCommit(text [, windowId])` | Commit the composition with `text` (the same path as a real IME's `SDL_EVENT_TEXT_INPUT`): replaces the preedit, fires the final `compositionupdate` → `input` → `compositionend`, and records ONE undo entry for the whole composition. Without an active composition it behaves like `textInput(text)`. |
| `imeCancel([windowId])` | Cancel the composition (an empty editing event): removes the preedit, restores the pre-composition value/selection, fires `compositionupdate("")` → `input` → `compositionend("")`, leaves no undo entry. |
| `paste(text)` | Simulate paste on focused element with the given text (dispatches paste event, inserts into input/textarea) |
| `copy()` | Simulate copy on focused element (dispatches copy event, returns the selected text, a collapsed caret copies nothing) |
| `cut()` | Simulate cut on focused element (dispatches cut event, removes the selected range, returns the cut text) |
| `dropFiles(x, y, paths [, windowId])` | Simulate file drop at coordinates. `paths` is an array of file path strings (a bare string is accepted for one file). Dispatches dragenter → dragover → drop **once** for the whole gesture, with a real `File` for every path in `dataTransfer.files` (see docs/file-api.js) — matching a real multi-file drop, which SDL delivers as one `DROP_BEGIN`…`DROP_COMPLETE` group. |
| `dropText(x, y, text [, windowId])` | Simulate text drop at coordinates. Dispatches dragenter → dragover → drop with text data. |
| `setPickedFiles(paths)` | What the next `<input type=file>` click picks (a path string or an array of them). There is no native picker with no user present, so queue the choice and click the input as a user would; the click consumes it, and a click with nothing queued is a cancelled pick. See docs/file-api.js. |
| `lastDownload()` | Absolute path of the file the most recent `<a download>` click saved, or `null` if none has. Lets a test assert on an app's export without knowing the user's Downloads folder. See docs/file-api.js. |
| `setDialogAnswer(accept)` | What `alert`/`confirm`/`prompt` do with no user to ask. Headless never blocks on them: the message is logged and the call returns at once, accepting by default (`confirm` → `true`, `prompt` → its default value). Pass `false` to take the cancel branch instead (`confirm` → `false`, `prompt` → `null`) until set back. See docs/dialogs-api.js. |
| `resize(w, h)` | Resize the virtual viewport. Each side is clamped to [1, 16384], the largest surface one GL texture can back. |
| `setDeviceScaleFactor(s)` | Simulate a display with `s` device px per CSS px (a Retina Mac is 2), the way a windowed engine follows its window's backing scale. `devicePixelRatio` and `@media (resolution)` change, `resize` and matchMedia `change` fire, and the layer surfaces, the 3D scene and every captured frame render at `s`× (a `screenshot()` is `s`× the viewport). Layout, `getBoundingClientRect`, `click()` and `getPixel()` stay in CSS px. |
| `gamepadConnect([id])` | Connect a virtual gamepad; returns its slot index. Fires `gamepadconnected` on window, appears in `navigator.getGamepads()`. |
| `gamepadDisconnect(index)` | Disconnect a virtual gamepad. Fires `gamepaddisconnected`. |
| `gamepadButton(index, button, pressed [, value])` | Set a virtual pad's button. `button` is a W3C index (0-16) or name (`"south"`, `"start"`, `"lefttrigger"`, ...). `value` gives triggers an analog level (defaults to pressed ? 1 : 0). Press/release edges dispatch bound `"action"` events. |
| `gamepadAxis(index, axis, value)` | Set a virtual pad's stick axis. `axis` is 0-3 or `"leftx"`/`"lefty"`/`"rightx"`/`"righty"`; value -1..1. |

Note: `el.click()` (DOM method) dispatches a click event directly on the element without hit testing. `click(x, y)` (headless global) goes through the full engine input pipeline with hit testing, focus, and bubbling. Use this when testing user interactions.

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
  element's undo and redo stacks (browser behavior). Ctrl+Z cannot cross a
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
**commits** the current preedit rather than stranding it. The controls store
byte offsets internally, but `selectionStart`/`selectionEnd` at the JS
boundary are UTF-16 code units per spec.

**Contenteditable** composes too: with the DOM Selection caret inside a
`contenteditable` host (click into it first), the preedit is spliced
provisionally into the text node at the caret, `textContent` shows it,
`compositionstart`/`compositionupdate`/`compositionend` and
`input(insertCompositionText)` target the host element in the same order as
the controls, the preedit renders with the same thin underline plus the
composition-cursor caret, and the never-strand commits apply identically. If
the caret sits between elements (or in an empty host) the text node is
created by the same insertion rule regular contenteditable typing uses.
Undo matches the controls: a committed composition records one discrete
entry, so a single Ctrl+Z removes the whole committed run, and `imeCancel()`
records none, it removes the preedit and, when the composition had replaced
a non-collapsed selection at `compositionstart`, resurrects that selection
and its DOM. Engine-wide deviations shared with the controls: events dispatch after the
mutation, `beforeinput` is not fired for composition, and
`compositionstart`'s `preventDefault` is not honored.

### Editing commands (`document.execCommand`)

`document.execCommand(name [, showUI, value])` runs an editing command against
the current Selection, and `queryCommandSupported(name)` /
`queryCommandEnabled(name)` report whether a command exists in this build and
whether it would do anything right now. Names match case-insensitively.

Every supported command runs the *same* engine primitive as the key press it
names, one implementation, not two. So a scripted edit and a typed one
produce identical DOM, identical `beforeinput`/`input` events (a canceled
`beforeinput` blocks the command) and identical undo entries in one shared
history. `execCommand("undo")` and Ctrl+Z step the same stack.

| Command | Equivalent | Notes |
|---------|-----------|-------|
| `insertText` | typing | `value` is the text. Empty `value` is a no-op that still returns true and fires no events. Replaces a non-collapsed selection. |
| `insertLineBreak`, `insertParagraph` | Enter | Both insert a `<br>`: contenteditable is plaintext-v1, so there is no block splitting to tell them apart yet. |
| `delete` | Backspace | Deletes the selected range, else one character backward. |
| `forwardDelete` | Delete | One character forward. |
| `undo`, `redo` | Ctrl+Z / Ctrl+Y | False when the host's history has nothing in that direction. |
| `selectAll` | Ctrl+A | Selects the containing host's children, or the body's: the one command that works outside an editable. |
| `copy`, `cut`, `paste` | Ctrl+C/X/V | Real system clipboard, not the `copy()`/`paste()` headless hooks. |

Returns false for an unsupported command, and for a supported one with
nothing to act on (no editable selection, empty history, collapsed selection
for copy/cut). Formatting commands, `bold`, `italic`, `underline`,
`foreColor`, `createLink`, `formatBlock`, are **not** supported: they need an
inline-formatting model plaintext-v1 doesn't have, and they report
`queryCommandSupported === false` rather than silently doing nothing, so
callers can feature-detect instead of discovering it from a no-op.

Deliberate divergence from browsers: `paste` (and `cut`/`copy`) work from
script. Browsers refuse them because a web page reading the user's clipboard
without a gesture is a privilege escalation; bro is an app runtime whose app
is the trusted party, and `navigator.clipboard` is already available to it
unconditionally. Refusing here would buy no safety and would only make the
keyboard and scripted paths disagree.

### Settings

The `bro.settings` API is available in headless mode for reading and writing persistent engine settings (graphics, audio, input, action bindings). See [settings.md](settings.md) for the full API reference.

### Window management, screen, and battery

`bro.window`, `window.screen`, `window.open`, and `navigator.getBattery` all install in headless mode, pinned for determinism ([window-api.js](window-api.js)): flag/limit setters (borderless, alwaysOnTop, min/max size) round-trip against the hidden window; state-affecting ops (minimize/maximize/restore, setPosition, moveToDisplay) no-op; `getDisplays()` enumerates the machine's real displays (assert shapes, not values); `screen.*` pins to the hidden window's size; `window.open` never shells out for an external URL (it returns null) but opens an app-relative src as a hidden bro window and returns its handle, as `bro.window.open` does; `getBattery()` always resolves the no-battery shape `{charging: true, chargingTime: 0, dischargingTime: Infinity, level: 1}`.

### CSS/Layout inspection

| Function | Description |
|----------|-------------|
| `inspect(selector [, verbose])` | Return formatted box model, position, computed styles, and DOM info. Pass `true` for verbose (all styles). See [inspect.md](inspect.md). |
| `inspectTree(selector [, depth])` | Return a tree view of element layout (sizes + positions). Default depth 3. Traverses shadow DOM. |
| `computedStyle(selector [, property])` | Return a specific computed style value (string), or all styles as a JS object. |
| `elements(selector)` | Return a summary of all matching elements with sizes and positions. |
| `inspectOverlay(panel, selector [, verbose])` | Same output as `inspect()`, but resolves `selector` inside a **system panel's** document (`"perf"`, `"menu"`, `"nav"`, `"settings/graphics"`, ...) instead of the app's. Throws if nothing matches. |
| `inspectOverlayTree(panel, selector [, depth])` | Same output as `inspectTree()`, rooted at a system panel's element. Default depth 3. |
| `overlayPanels()` | Array of the loaded system panels' names: the values `inspectOverlay`/`inspectOverlayTree` accept. |

### Text shaping (`bro.text`)

A diagnostic view of the shaper's cluster map and the bidi resolver, not an
app-facing text API, but installed unconditionally in **both** headless and
windowed mode, which makes it the way to assert shaping and bidi behavior from
a script. All offsets are **byte** offsets into the UTF-8 string (the engine
works in bytes here; `selectionStart`/`selectionEnd` on the DOM controls are
UTF-16 per spec). Everything that needs a renderer returns `null` when there
isn't one.

The options bag is shared by the first four functions:
`{ family: 'Arial', size: 16, weight: 400, italic: false, letterSpacing: 0, wordSpacing: 0 }`:
every key optional, defaults as shown.

| Function | Description |
|----------|-------------|
| `bro.text.shape(text, opts)` | Shape `text` → `{text, glyphCount, width, clusters}`. Each cluster is `{start, end, x, advance, glyphs, rtl}`, its byte span, x offset and advance within the run, how many glyphs it produced, and whether it resolved RTL. |
| `bro.text.byteOffsetToX(text, opts, byteOffset)` | Caret position for a byte offset → `{x, isLeadingEdge}`, plus a `secondary` caret of the same shape at a directional boundary. |
| `bro.text.xToByteOffset(text, opts, x)` | The inverse: nearest byte offset for an x position within the run. |
| `bro.text.clusterRange(text, opts, byteOffset)` | `{start, end}`: the byte span of the cluster containing that offset (grapheme-safe caret movement). |
| `bro.text.cacheStats()` | `{hits, misses}` for the shaping cache. Useful for asserting that a re-render reused shaped runs instead of re-shaping. |
| `bro.text.bidi(text [, base, override])` | Run UAX #9 over `text` → `{paragraphLevel, uniform, levels, runs}`. `base` is `"auto"` (default, P2/P3), `"ltr"` or `"rtl"`; a truthy `override` applies a directional override. `levels` has one entry **per codepoint**; `runs` is `[{start, end, level}]` in byte offsets. |
| `bro.text.bidiReorder(levels)` | Rule L2 applied to an array of levels → the logical index for each visual slot. |
| `bro.text.bidiAvailable` | Boolean, not a function: whether a bidi resolver is compiled in. |

### Performance

`perf` measures what a change costs the style and layout passes, the two that a
DOM update actually pays for, and the two a screenshot can't show you.

| Function | Description |
|----------|-------------|
| `perf.now()` | Real wall-clock milliseconds. **Use this, not `performance.now()`**: in headless that one rides virtual time (see below), so it is frozen between `advanceTime()` calls and reports 0ms for work that took a second. (Windowed and server runs interpolate it from wall time instead, so an app profiling its own render frame there gets a real number; headless stays frozen on purpose, so a test measuring across N virtual milliseconds gets exactly N back.) |
| `perf.reset()` | Zero the counters. |
| `perf.stats()` | The counters since the last reset (CPU style/layout only). |
| `perf.gpuFrameMs()` | **Real GPU milliseconds** for the last `flush()`'s 3D scene render, from a native `GL_TIME_ELAPSED` query wrapped around the scene draw. Blocking: it reads `GL_QUERY_RESULT`, which forces that frame's GPU work to finish, so each call returns an isolated per-frame GPU cost. This is the number to trust for 3D/render perf — wall-clock around `flush()` returns *before* the GPU runs the draws and so measures nothing. Returns `-1` for a 2D-only page, under `--no-gpu`, or before the first scene flush. |

Measuring GPU frame cost — drive the camera directly (suppress the app's rAF so it can't clobber it), `flush()`, and read `perf.gpuFrameMs()` each frame; each read serializes on that frame's query, so summing gives a clean average:

```js
window.requestAnimationFrame = () => 0;        // stop the app driving the camera
const cam = { mode:'perspective', fov:58, aspect, near:1, far:120000,
              position:[px,py,pz], target:[tx,ty,tz], up:[0,1,0] };
for (let i = 0; i < 40; i++) { scene.setCamera(cam); flush(); }  // warm/stream
perf.gpuFrameMs();                              // drain the pending query
let sum = 0;
for (let i = 0; i < 200; i++) { scene.setCamera(cam); flush(); sum += perf.gpuFrameMs(); }
console.log(`${(sum/200).toFixed(2)} ms/frame (${(200000/sum).toFixed(0)} fps)`);
```

Toggle a subsystem's scene nodes `.visible` on/off across two such runs and subtract to attribute GPU cost to it. The absolute numbers track real windowed framerate.

`perf.stats()` returns, accumulated over every style/layout pass since the reset:

| Field | Meaning |
|-------|---------|
| `styleMs` | `resolveStyles()`: selector matching + the computed-style diff |
| `cascadeMs` | of `styleMs`: time inside htmlayout's `Cascade::resolve()`, per element |
| `styleDiffMs` | of `styleMs`: diffing each element's old and new computed style |
| `styleManagersMs` | of `styleMs`: the transition/animation hooks and their overrides |
| `genContentMs` | of `styleMs`: `::before`/`::after` resolution for the restyled elements |
| `pseudoResolves` | `Cascade::resolvePseudo()` calls (two per restyled element when the sheet has any `::before`/`::after` rule) |
| `buildMs` | rebuilding the whole layout tree from the DOM |
| `invalidateMs` | carrying element dirt into the layout tree, including per-element subtree rebuilds |
| `layoutMs` | `layoutTree()` itself: the sum of the three sub-passes below |
| `layoutTreeMs` | in-flow layout, the only sub-pass that is incremental |
| `layoutAbsMs` | positioning absolute/fixed boxes |
| `layoutHitMs` | caching per-node subtree hit bounds |
| `syncMs` | writing the resulting boxes back onto elements |
| `totalMs` | the sum of the above |
| `passes` | how many layout passes ran |
| `elementsStyled` | elements whose computed style was re-resolved |
| `nodesLaidOut` | layout nodes that ran a formatting context |
| `nodeVisits` | every `layoutNodeInner()` call, including re-entrant ones: a flex or grid container lays an item out to measure it, then again to push the resolved size through. `nodeVisits` far above `nodesLaidOut` means the pass is dominated by re-measurement, not by what changed. |
| `nodeRevisitsSkipped` | same-pass re-visits answered from the box: a flex or grid container asked for an item's layout again with every input equal to the last time (available size, override, and the preset size it wrote in), so nothing was re-run. Nested flex re-visits each level from every visit of its parent; this is what keeps that from doubling per level. |
| `nodesReused` | layout nodes handed back from cache untouched |
| `measureCalls` | text measurements (shaping) requested |
| `styleLookups` | string-keyed style map lookups inside `layoutTree()` |
| `reuseFailDirty` | nodes re-laid because they (or a descendant) really changed |
| `reuseFailAvailW` | nodes re-laid because their available width differed from cache |
| `reuseFailAvailH` | nodes re-laid because their available height differed from cache |
| `reuseFailOverride` | nodes re-laid because their flex width override differed |
| `treeRebuilds` | layout subtrees rebuilt from the DOM |
| `scene` | 3D frustum-culling counters from the most recent frame, summed across all scene graphs: `{mesh,instanced,splat,particles,billboards,shadow}{Drawn,Culled}` (shadow counts are per caster × atlas tile), plus `shadowTilesTotal` / `shadowTilesRendered` / `shadowTilesCached`, how many shadow-atlas tiles existed, were re-rendered, and were served from cache this frame. Per-graph numbers: `scene.cullStats()`; escape hatch: `scene.setFrustumCulling(false)`. See `docs/scene-api.js`. |
| `dom` | The detached-tree sweep (`src/bronze_host/host_node_sweep.h`), current rather than since the reset: `nodes` the live documents own, `entries` / `live` / `wrapped` in the host's node registry (all entries, those with a node, those with a JS wrapper), the sweep's running totals `passes` (frames it had work on), `collections` (the ones it asked for itself), `treesFreed`, `groupsDied`, `groupsSurvived` (lived through a full collection), `groupsPromoted` (brought back by a touch), its per-frame slice `lastPassMs` and `maxPassMs` (the longest since the last `perf.stats()` read, which resets it), `lastCollectMs`, and what is outstanding: `demotedTrees` in `groups`, and `queued` candidates. A UI that rebuilds from templates should hold `nodes` and `entries` flat. `__host.domSweep()` does all the outstanding work now, collections included; `BRO_DOM_SWEEP=0` turns the sweep off. |
| `processBytes` | The process's private committed bytes (Windows; resident bytes on Linux). |

**The counts matter more than the milliseconds.** Layout and style are both
incremental: a change is supposed to cost time proportional to what it changed,
not to the size of the document. `nodesLaidOut` is what tells you whether that
actually happened. A change to one element that comes back having laid out
thousands of nodes has an *invalidation* bug, something marked more dirty than
it had to, and no amount of making layout faster will fix it. Likewise a large
`measureCalls` says the cost is text shaping, not boxes, and `styleLookups`
far out of proportion to `nodeVisits` says some pass is re-deriving style
across whole subtrees instead of touching only what changed. When
`nodesLaidOut` is high, the `reuseFail*` counters say why: `reuseFailDirty`
is real invalidation (look upstream at what got marked), while the other
three mean clean subtrees are being offered different layout inputs than
they were cached under, a cache-defeat inside layout itself.

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

Exit code is 0 on success, 1 if any assertion fails or an uncaught exception occurs, and 77 if the script called `skipTest()` (or `missingGpuContext()` without a GPU) and nothing failed.

### Await in scripts

Top-level `await` is not accepted by the compiler. Wrap asynchronous work in an async function and pump the engine until it settles (see "Waiting in scripts: pump, don't await" below):

```js
async function main() {
  let resp = await fetch('data.json');
  let data = await resp.json();
  assert(data.items.length > 0, 'data loaded');
  screenshot('loaded.png');
}
main();
```

## Integration tests

The `tests/` directory contains integration tests that exercise the engine via `bro-headless` on its default GPU path, the same renderer, WebGL, and layer compositing the shipping runtime uses. (`--no-gpu` selects the CPU raster fallback, which is a different code path; running the suite there would leave the real one untested.) Run them with:

```bash
bash tests/run_tests.sh          # run all tests
bash tests/run_tests.sh events   # filter by substring
```

Each test is a self-contained JS file that manipulates the DOM and uses `assert()` to verify behavior. The runner discovers all `tests/*/test_*.js` files, runs each against the minimal `tests/test_app/` HTML page, and reports pass/fail with a summary.

### Test categories

`tests/` holds 40+ directories, one per subsystem (`webgl/`, `scene/`, `physics/`,
`audio/`, `net/`, `window/`, `video/`, the ML suites, ...). A sample of the core
ones:

| Directory | What it tests |
|-----------|---------------|
| `tests/dom/` | createElement, appendChild/removeChild, innerHTML, textContent, querySelector, attributes, classList, dataset, cloneNode |
| `tests/events/` | click dispatch, event bubbling, stopPropagation, preventDefault, addEventListener/removeEventListener, `document.dispatchEvent` |
| `tests/style/` | Inline style get/set, getComputedStyle |
| `tests/layout/` | getBoundingClientRect, offsetWidth/Height/Left/Top, clientWidth/Height |
| `tests/timers/` | setTimeout, setInterval, requestAnimationFrame (all with virtual time via advanceTime) |
| `tests/shadow_dom/` | attachShadow, shadow root querySelector, slot distribution |
| `tests/custom_elements/` | customElements.define, lifecycle callbacks (connected/disconnected), observedAttributes + attributeChangedCallback |
| `tests/gc/` | Orphan element cleanup after innerHTML removal, rapid create/remove cycles |

### Writing tests

Tests run against `tests/test_app/` which provides a minimal HTML page with a `<div id="root">` and a reset stylesheet (`* { margin: 0; box-sizing: border-box }`). Tests create DOM elements dynamically, use `flush()` to trigger layout, and `assert()` to verify:

`flush()` is about the *frame* — painting, observers, events, sub-documents. Measurement does not need it: reading `getBoundingClientRect()`, `offsetWidth`, `getComputedStyle()` and the rest lays the document out first, so an element built and measured in the same turn measures correctly. Anything that waits on a frame — a screenshot, a hit test, a ResizeObserver callback — still needs the flush.

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

Headless mode shares the same `Engine` class as windowed mode, configured via `EngineConfig` with `DisplayMode::Headless`. Headless testing APIs (`screenshot`, `advanceTime`, etc.) operate directly against the engine after `engine.run()` initializes layout.

### GPU mode (default)

- Uses headless Vulkan 1.3 Core with Dynamic Rendering (`VK_KHR_dynamic_rendering`) directly via `VulkanContext` and `VulkanPresenter` without requiring an X11 server, window, or Xvfb on Linux
- Uses `SkiaRenderer`: same Skia rasterization backend as windowed mode
- WebGL2 support: Three.js, raw WebGL, and other GL frameworks work via native Vulkan translation (`WebGLVkContext`, `WebGLVkCanvas`)
- 3D scene graph runs Vulkan render passes; its shaders are compiled to SPIR-V by the in-process glslang (built-in ones at build time, custom shaders and WebGL programs at run time), and pipelines persist in the on-disk pipeline cache
- Screenshots replicate the windowed compositing pass: scene layers rendered to offscreen Vulkan render targets, UI overlay composited on top with zero-copy texture presentation, then read back directly via Vulkan transfer buffers
- Text metrics use Skia with platform-native fonts (DirectWrite on Windows, FreeType/fontconfig on Linux), pixel-identical to windowed rendering

### WebGL2 support matrix

The `webgl2` context maps WebGL 2 onto Vulkan (`WebGLVkContext`,
src/webgl/vulkan/). Behavioral tests live in `tests/webgl/`; each claim
below is exercised there with pixel readback. This is the state of the
backend, not of the WebGL 2 spec: what is missing is listed as missing.

**Command stream.** Every GPU operation (draws, clears, uploads, copies,
blits, layout changes) is recorded in API order into one command stream per
context and submitted at frame end, or earlier once the stream has used its
upload / descriptor budget (32 MB, 4096 sets), so a long burst of uploads in
one turn neither runs out of memory nor stalls. Drawing from top-level
script, timers or across frames reaches the composited frame; the app's
framebuffer binding persists across frames. Only readbacks (`readPixels`,
`getBufferSubData`, a screenshot) and client waits block, on the context's
own work. Several WebGL canvases composite independently.

**Shaders.** GLSL ES 3.00 and 1.00 are compiled in process by glslang to
SPIR-V, and the program interface comes from glslang's reflection, not from
parsing source text: attributes (placed by `layout(location)`, then
`bindAttribLocation`, then automatically; aliasing fails the link),
uniforms of every type including structs, arrays of structs, square,
non-square and transposed matrices, sampler arrays, `std140` uniform blocks
(including block arrays) and fragment outputs. ES 1.00 `attribute` /
`varying` / `gl_FragColor` / `gl_FragData` / `texture2D` work, and the
`OES_standard_derivatives`, `EXT_shader_texture_lod`, `EXT_frag_depth` and
`EXT_draw_buffers` directives are accepted. `gl_FragCoord` and
`gl_PointCoord` follow GL's window convention. A link fails when the program
needs more uniform vectors, samplers or uniform blocks than `getParameter`
reports; a sampler is visible only to the stages that read it, so the vertex
and fragment stages each have their own sampler budget (16 + 16 = 32
combined even on MoltenVK, whose per-stage limit is 16). Every precision is highp, and `getShaderPrecisionFormat` says so.

**Limits.** `getParameter` answers implementation limits from the Vulkan
device: texture, cube, 3D, array-layer, renderbuffer and viewport sizes,
`MAX_SAMPLES`, draw buffers / color attachments, vertex attributes, uniform
vectors and components, varyings, texture units, uniform-buffer bindings,
blocks, block size and offset alignment, `MAX_ELEMENT_INDEX`, texel offsets,
LOD bias, subpixel bits, the point-size range (1 without the device's
`largePoints`), the line-width range (1 without the device's `wideLines`, so
on MoltenVK) and the transform feedback limits. `RED_BITS` ...
`STENCIL_BITS`, `SAMPLES` / `SAMPLE_BUFFERS`, `DRAW_BUFFERi`, `READ_BUFFER`
and `IMPLEMENTATION_COLOR_READ_FORMAT/TYPE` describe the bound framebuffers.
`EXT_float_blend` and `OES_texture_float_linear` are offered only when the
device supports them.

**Framebuffers.** Color (up to `MAX_COLOR_ATTACHMENTS`), depth, stencil
and `DEPTH_STENCIL` attachments, from texture levels, cube faces,
array / 3D layers (`framebufferTextureLayer`) and renderbuffers; separate
`DRAW_FRAMEBUFFER` / `READ_FRAMEBUFFER` bindings; completeness with
`INCOMPLETE_MISSING_ATTACHMENT`, `INCOMPLETE_ATTACHMENT`,
`INCOMPLETE_MULTISAMPLE` and `UNSUPPORTED` (depth and stencil attached from
different images), and `INVALID_FRAMEBUFFER_OPERATION` for draws, clears,
reads and blits on an incomplete one; `drawBuffers` / `readBuffer` with the
ES 3.0 rules (`BACK` / `NONE` on the canvas, `COLOR_ATTACHMENTi` / `NONE` on
an FBO); `getFramebufferAttachmentParameter` and `getRenderbufferParameter`.
Sampling a texture attached to the draw framebuffer is `INVALID_OPERATION`.
The canvas has a `DEPTH24_STENCIL8` buffer. Renderbuffers take the
color-renderable and depth/stencil formats, multisampled at the counts the
device has (`getInternalformatParameter(RENDERBUFFER, fmt, SAMPLES)`;
integer formats are never multisampled), and start cleared (color 0,
depth 1, stencil 0).

**Clears, blits, reads.** `clear` and `clearBuffer{fv,iv,uiv,fi}`, typed per
buffer (`clear` of an integer buffer is `INVALID_OPERATION`), cut to the
scissor box. `blitFramebuffer` copies color, depth and stencil between the
canvas and FBOs in either direction, scaled (`NEAREST` / `LINEAR`), mirrored,
clipped to both framebuffers and the scissor box, and resolves
multisampled color and depth/stencil (sample zero) into single-sample
targets, with the ES 3.0 validation. `readPixels` reads the read buffer of the
canvas or an FBO as RGBA/`UNSIGNED_BYTE` (normalized buffers), RGBA/`FLOAT`
(float buffers) or `RGBA_INTEGER`/`INT` or `UNSIGNED_INT` (integer buffers),
honours `PACK_ALIGNMENT`, leaves pixels outside the framebuffer untouched,
and can target a `PIXEL_PACK_BUFFER`.

**Textures.** 2D, cube, 2D array and real 3D images in every ES 3.0
sized and unsized format (normalized, `SNORM`, integer, sRGB, packed,
16/32-bit float, depth and depth-stencil), stored in the Vulkan format each
names (`RGB` formats in their four-channel form, swizzled to alpha 1);
`texStorage*` is immutable (`TEXTURE_IMMUTABLE_FORMAT` / `_LEVELS`); depth
textures take uploads. Uploads honour every unpack parameter
(`UNPACK_ALIGNMENT`, `_ROW_LENGTH`, `_IMAGE_HEIGHT`, `_SKIP_*`,
`UNPACK_FLIP_Y_WEBGL`, `UNPACK_PREMULTIPLY_ALPHA_WEBGL`), from client memory
or a `PIXEL_UNPACK_BUFFER`; `readPixels` honours the pack ones.
`copyTexImage2D` / `copyTexSubImage*` and `generateMipmap` run on the GPU.
Compressed textures (S3TC, S3TC sRGB, RGTC, BPTC, ETC2/EAC, ETC1, ASTC LDR,
including `compressedTexImage3D` and the PBO overloads) are stored as they
come, and their extension is offered only where the device samples the
format natively. `EXT_texture_filter_anisotropic` is offered where the device
has `samplerAnisotropy`. Depth comparison (`TEXTURE_COMPARE_MODE` /
`_FUNC`, from the texture or a sampler object) is sampled through
`sampler*Shadow`, with WebGL 2's `INVALID_OPERATION` for a shadow sampler
without comparison and a plain sampler with it; on a portability device
without `mutableComparisonSamplers` (an old MoltenVK) a program with a
shadow sampler fails to link instead. `getTexParameter` answers every
parameter.

**Pipeline state.** `polygonOffset`, `blendColor` (the `CONSTANT_*`
factors), `depthRange`, `lineWidth` (where wide lines exist),
`sampleCoverage` and `SAMPLE_ALPHA_TO_COVERAGE` (on multisampled targets;
ignored on single-sampled ones, as GL does) and `RASTERIZER_DISCARD` (which
also skips clears) all apply. `clear` with a partial `colorMask` or stencil
write mask writes only the enabled channels / bits. `DITHER` is accepted
and has no effect.

**Draws.** `drawArrays`, `drawElements`, `drawRangeElements` and the
instanced forms, with WebGL's validation: mode and index type, index
alignment, the index range against each enabled array's buffer, an enabled
array without a buffer, and the WebGL 2 type match between each array or
constant value and the shader input. Primitive restart is always on for
indexed draws (at the type's largest index, in strips and lists alike);
`LINE_LOOP`, 8-bit indices, 32-bit `INT` / `UNSIGNED_INT` / fixed-point
float arrays, and instance divisors above 1 on devices without them are
rewritten for Vulkan (indices regenerated, or vertices converted into the
upload stream). Samplers of different types on one texture unit make a draw
`INVALID_OPERATION`, and `validateProgram` reports it.

**Queries and sync.** `ANY_SAMPLES_PASSED` and `_CONSERVATIVE` are GPU
occlusion queries, available once the frame that recorded them has finished
on the GPU; `TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN` counts what the
captured draws wrote. Sync objects: `fenceSync` / `clientWaitSync` /
`getSyncParameter`, waits capped at `MAX_CLIENT_WAIT_TIMEOUT_WEBGL` = 1 s;
`waitSync` returns at once (all work is on one queue, so it is already
ordered) and `MAX_SERVER_WAIT_TIMEOUT` is 0.

**Transform feedback.** Where the device has
`vertexPipelineStoresAndAtomics` (else `createTransformFeedback` returns
null and the limits are 0): interleaved and separate capture into up to 4
buffers (`bindBufferBase` / `bindBufferRange`), of float, integer, vector,
matrix and array varyings and `gl_Position` (a struct or bool varying fails
the link), from `drawArrays` /
`drawArraysInstanced` in `POINTS` / `LINES` / `TRIANGLES`; pause / resume;
transform feedback objects; `getTransformFeedbackVarying`; and the ES 3.0
errors (mode mismatch, `drawElements` while capturing, overflow, a capture
buffer bound elsewhere, changing the program while active). The vertex
stage stores the varyings itself, so capture works with or without
`RASTERIZER_DISCARD`.

**Context loss.** `WEBGL_lose_context` loses and restores the context:
while lost every call is a no-op answering its zero value (`getParameter`,
`getContextAttributes` and `getSupportedExtensions` answer null),
`getError` reports `CONTEXT_LOST_WEBGL` once, and `webglcontextlost` fires
at the canvas a task later; a page that cancels it may `restoreContext()`,
after which `webglcontextrestored` fires and the context starts over with
default state and none of its old objects. A real Vulkan device loss is not
surfaced this way (the engine has one device for everything).

**Also implemented:** buffers with every `bufferData` / `bufferSubData` /
`getBufferSubData` signature, `copyBufferSubData`, `bindBufferBase` /
`bindBufferRange` (offset-alignment checked; a draw whose block range is
missing or too small is `INVALID_OPERATION`) and `getIndexedParameter`;
VAOs, integer attributes, constant attributes (`vertexAttrib*`,
`vertexAttribI4*`); sampler objects; uniform and block introspection
(`getActiveUniform(s)`, `getUniformIndices`,
`getActiveUniformBlockParameter` / `Name`); the getters `getUniform` (typed
as the IDL says: numbers, booleans, `Float32Array` / `Int32Array` /
`Uint32Array`, a boolean array for `bvec*`, the unit for a sampler),
`getVertexAttrib` / `getVertexAttribOffset`, `getBufferParameter`; the
`is*` predicates.

**Binding layer:** every object a call answers (`createBuffer`,
`getParameter(CURRENT_PROGRAM)`, `getFramebufferAttachmentParameter(...
OBJECT_NAME)`, `getVertexAttrib(..._BUFFER_BINDING)`, `getQuery`, ...) is
the same wrapper object from creation until deletion, so `===` compares
objects, and `getExtension` answers the same object on every call. Array
results are the typed arrays the IDL names.

**Not implemented:** an antialiased canvas (`antialias` is reported false;
render into a multisampled renderbuffer and blit instead);
`invalidateFramebuffer` / `invalidateSubFramebuffer` are accepted as the
hints they are and do nothing.

#### `BRO_buffer_map` — direct access to buffer storage

Not a WebGL extension. WebGL cannot expose `glMapBufferRange`, because doing so
means handing a web page a raw pointer into driver memory; bro is not a browser
and can.

**It is not faster than `bufferSubData`.** Measured per update, RTX 4090:

| payload | `bufferSubData` | `mapBufferRange` |
|---|---|---|
| 64 KB | 0.008 ms | 0.008 ms |
| 1 MB | 0.130 ms | 0.113 ms |
| 4 MB | 0.495 ms | 0.496 ms |

The intuition that mapping removes a copy does not apply here: bro's
`bufferSubData` already hands the caller's `TypedArray` pointer straight to GL,
so both paths move the bytes exactly once. And if JS *generates* the data
element by element rather than already holding it, the fill loop costs ~60 ns
per float against ~0.1 ns per byte of transport — the copy is nowhere near the
bottleneck, so removing it changes nothing (measured 0.93x–1.02x).

What mapping actually buys is two things `bufferSubData` cannot express:

- **Read-modify-write of a sub-range** with no JS-side mirror of the buffer.
  Changing a few values in a large buffer otherwise means either keeping a full
  shadow copy in JS or reading back first.
- **`MAP_UNSYNCHRONIZED_BIT` streaming**, where the app takes responsibility for
  not overwriting in-flight data (ring-buffer style) instead of letting the
  driver serialize.

```js
const M = gl.getExtension('BRO_buffer_map');   // null if unavailable
gl.bindBuffer(gl.ARRAY_BUFFER, buf);

// Nudge one vertex inside a large buffer, without a JS-side copy of it.
const ab = gl.mapBufferRange(gl.ARRAY_BUFFER, vertexIndex * 12, 12,
                             M.MAP_READ_BIT | M.MAP_WRITE_BIT);
const v = new Float32Array(ab);
v[1] += 0.5;
gl.unmapBuffer(gl.ARRAY_BUFFER);               // `ab` is detached here
```

- `mapBufferRange(target, offset, length, access)` → an `ArrayBuffer` aliasing
  the mapped range, or `null` with a GL error set. `access` takes the
  `MAP_*_BIT` values off the extension object.
- `unmapBuffer(target)` → `false` if the mapping was lost and its contents must
  be resubmitted (GL's documented `glUnmapBuffer` failure), otherwise `true`.
- `flushMappedBufferRange(target, offset, length)` for `MAP_FLUSH_EXPLICIT_BIT`.

**The returned `ArrayBuffer` is detached by `unmapBuffer`** — it aliases memory
the driver reclaims, so it cannot be allowed to outlive the mapping. Keep any
typed-array views inside the map/unmap pair. Mappings are tracked per buffer
object, so rebinding the target mid-mapping is harmless, and `deleteBuffer`
drops the mapping with the buffer.

GL 3.3 has no persistent mapping, so a mapping spans a single update rather
than a frame. As in raw GL, drawing from a buffer while it is mapped is
undefined; this layer passes calls through and does not police it.

### CPU mode (`--no-gpu`)

- No window, no SDL video subsystem, no Vulkan device
- Uses `RasterRenderer`: CPU-only Skia with real platform-native fonts
- Canvas 2D rendered via software command replay
- No WebGL support (apps fall back gracefully)
- No 3D scene: `canvas.getContext('scene')` returns `null`, so branch on it (`const s = canvas.getContext('scene'); if (!s) { /* 2D fallback */ }`). The 3D renderer is Vulkan end to end and requires a GPU device. Note that `bro.gpu.available` reports the ML/compute backend (Vulkan/CUDA/Metal).
- The same applies when a headless boot *tries* for GPU and fails: if SDL can't open a video device the engine logs `falling back to CPU raster rendering` and behaves exactly as `--no-gpu` from then on
- Screenshots captured directly from the Skia raster surface
- Input simulation (click, mouseDown, etc.) works fully, hit testing, event dispatch, focus management all function without a GPU

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

A script file with a top-level `await` runs as an ES module. It has the same
globals as a classic script, `require` included.
Node modules are `require('fs')` in both and never bare globals, so a bare
`fs` is a ReferenceError in either; `import ... from 'node:fs'` is rejected
at compile time (see docs/brokit-api.js).

A script file with a top-level `await` is evaluated as an ES module, and while
its evaluation promise is pending the runner drains **microtasks only**, no
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
  drained at the engine's safe point, which `flush()` reaches. A driving
  script can therefore observe it in-process: `advanceTime()` until the
  sub-doc calls reload, `flush()`, and the host sees another `load` event.

- **In the top-level document** it tears down the whole app document and its
  JS realm, then re-parses and re-runs the app in the same engine. In headless
  mode the driving script (`-e` expression or script file) runs
  *inside* that realm, so the reload can never commit while the script is
  still on the stack. It is drained **between evaluation units**: after engine
  construction (an app that reloads itself during its first run), after each
  `-e` expression, and after a script file finishes. A
  single script file cannot observe its own top-level reload, the realm that
  would do the asserting is the one being replaced. To test it, let the app
  reload itself and pass a *second* script that runs in the fresh realm, or
  spawn a child `bro-headless` (see `tests/engine/test_location_reload_toplevel.js`).

Both contexts share the web semantics: the call is deferred (the calling
script runs to completion), multiple requests in one frame coalesce, scripts
re-execute fresh in a new realm, and the old realm's timers and listeners do
not survive the swap.

## Notes

- `[INFO]` and `[console.log]` lines go to stderr; `-e` print results go to stdout. Separate them with `2>/dev/null`.
- Screenshots are PNG format.
- The default viewport is 1920x1080. Override with `--width` and `--height`. These are applied *after* the app config loads, so an appdir's `bro.json` `width`/`height` has no effect in headless ? every headless run is 1920x1080 on every machine unless the flags are passed. Derive test expectations from `window.innerWidth`/`innerHeight` rather than from the manifest.
- Audio engine runs in headless mode; by default no audio device is opened (pass `--audio` to open the real SDL device + mic). `advanceTime()` pumps the audio DSP pipeline, so voices, effects, sequencer, metering, recording, and FFT analysis all work. The pump carries the fractional frame between calls, so thirty `advanceTime(1000 / 30)` calls render exactly one second of audio; capture a soundtrack for a video rendered at a fixed frame rate with `startRecording({ channels: 2, seconds })` and `exportRecordingToWav()`. Use `getBusPeakL/R()`, `getBusRmsL/R()`, `getSpectrum()`, and `stopRecording()` to inspect audio output numerically.
