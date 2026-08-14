# bronze_host — running bronze-compiled JS apps in bro

The host layer for [bronze](../../../bronze)-compiled (AOT) JavaScript:
registers a browser-shaped set of host globals (`document`, `window`,
`requestAnimationFrame`, the timers, `Image`, `XMLHttpRequest`) backed by the
engine, wraps `webgl::WebGL2RenderingContext` as a bronze object covering the
WebGL2 surface three.js r160's renderer drives, and owns the per-frame seam that
advances the clock, delivers completions, fires callbacks and performs the
microtask checkpoint. `src/js/webgl2_bindings*` is the reference for the GL
surface and for every constant value; this layer mirrors it, QuickJS values
swapped for bronze embed Values.

Off by default; nothing here is in the default build.

## The files

| File | What it owns |
|---|---|
| `dom_globals.cpp` | `document`, canvas, `window`, rAF, `performance`, **and the frame seam** (`hostFrame`) |
| `host_internal.h` | the non-GL shared surface: error funnel, clock, task queue, handle tags |
| `host_events.cpp` | `on<type>` + `addEventListener` for the objects that fire events |
| `host_dom_events.cpp` | canvas / document / window listeners, wired to the **engine's** dispatch |
| `host_timers.cpp` | `setTimeout`/`setInterval` and the main-thread task queue |
| `host_image.cpp` | `Image`, and the decode behind `.src` |
| `host_xhr.cpp` | `XMLHttpRequest` (text over the app asset path; see its header) |
| `gl_*.cpp`, `gl_internal.h` | the WebGL2 binding, one file per call family |
| `host_main.cpp` | `bro-bronze-host`: Engine, globals, `runMain()`, frames |

## The frame seam, which is the thing to understand first

`installWebHostGlobals` registers one `Engine::onFrame` callback. The engine
fires it at the point its own `requestAnimationFrame` fires — `engine_frame.cpp`
step 3a windowed, the equivalent point in each `advanceTime` step headless, and
after the timer dispatch in the server tick — under rAF's pause gate, with the
scaled-clock delta. Inside it, in this order:

1. **a leftover microtask drain**, if anything is queued;
2. **the clock**, advanced before anything reads it;
3. **host tasks** — image loads, XHR completions — *before* rAF, which is where
   the web runs a load event relative to the rendering steps;
4. **timers**;
5. **rAF callbacks**;
6. **the microtask checkpoint** — `embed::drainMicrotasks()`.

Step 6 is after rAF and not before, because an rAF callback is the main producer
of promise jobs in a render loop. Draining first would run every frame's
continuations one frame late, against the wrong state, and would mean the last
rAF before shutdown never reaches quiescence — which is where an unhandled
rejection is reported, so that rejection would never be reported at all.

Step 1 exists because bro drains QuickJS **twice** per frame (once right after
rAF, once after the late pumps that can resolve a promise) and there is only one
host hook, at the first of those points. A producer that enqueues a bronze job
after we return this frame is therefore seen at the top of the next one, rather
than whenever something else happens to drain.

`dom_globals.cpp` carries the same explanation at the code.

## Events, and the one dispatch walk they arrive on

A listener a compiled app registers on its canvas, on `document` or on `window`
fires from a real click. It does so because it is not this layer's listener at
all: `canvas.addEventListener` calls `dom::Element::addEventListener`, the
engine's own C++ registration, and `js::dispatchDomEvent` already walks the
event path ONCE with both listener kinds merged on a shared registration
sequence (`dom/event_target.h`). So a compiled handler gets the capture /
at-target / bubble phases, the shadow retargeting, and its place in
registration order beside the page's own handlers — for the same reason an
interpreted one does, not by a parallel arrangement that agrees with it.

`document.addEventListener` is `documentElement.addEventListener`, exactly the
delegation `src/js/document_bindings.cpp` performs for the interpreted side and
for its reason: the event path is built from Elements. The visible consequence
is the one the interpreted side already lives with — `currentTarget` inside a
document handler is `<html>`, not the document.

### The boundary rule

**Engine objects are shared. Event data is copied. Heap values never cross.**

Both worlds run in one Engine, on one thread, against one DOM, interleaved and
never concurrent. What they share is the *engine's* objects: the same
`dom::Element`, the same document, the same clock, the same GL context. What
crosses the language boundary is a copy:

- A listener is handed a fresh bronze object holding **copies** of the fields
  its event kind carries — type, coordinates, key, button, deltas, modifiers —
  never a QuickJS value and never a pointer into either heap.
- `event.target` is the exception that proves it: a canvas this layer created
  answers as **itself**, the very value the program holds, because identity is
  the whole use of a target. Anything else answers a `{tagName, id, nodeId}`
  descriptor.
- `preventDefault()` / `stopPropagation()` / `stopImmediatePropagation()` write
  through to the `dom::Event` dispatch is walking with, so a compiled listener
  cancels an event for the interpreted listeners after it, and vice versa. The
  event object is **live only for the duration of the listener call**: calling
  one of the three on a stored event object afterwards is a named `TypeError`,
  not a silent no-op and not a write through a dangling pointer.

### CustomEvent, which is the sanctioned channel between the two worlds

`dispatchEvent` from compiled code takes a plain descriptor —
`{type, bubbles, cancelable, detail}` — because bronze cannot build a value on a
chosen prototype, so there is no `new CustomEvent(...)` to write (the same limit
that makes `img instanceof Image` false). `bubbles` and `cancelable` default to
true.

```js
// compiled → interpreted
document.dispatchEvent({ type: 'app:ready', detail: 'v2' });

// interpreted → compiled  (an ordinary page script)
document.dispatchEvent(new CustomEvent('page:reset', { detail: 'hard' }));
```

**`detail` is a string and only a string.** A `detail` is an arbitrary JS value
on the web, and an arbitrary JS value belongs to exactly one heap; a string is
the one shape both heaps can copy without agreeing on a type system. It is
carried by `dom::CustomEvent` (`src/dom/event.h`), which is what makes it
survive the trip in either direction. A compiled `dispatchEvent` whose `detail`
is an object is a `TypeError` naming the reason, not a stringification — an
interpreted listener receiving `"[object Object]"` would be worse than being
told it cannot go. In the other direction an interpreted dispatch with a
non-string detail still reaches the interpreted listeners with the real value;
only the compiled ones see no payload.

`tests/bronze_host/` pins a round trip in both directions.

### Not supported, precisely

- **`once` and `capture`** are accepted (`addEventListener(type, fn, true)` or
  `{capture, once}`) and honoured by the engine's own list — but a `once`
  listener the engine reaps is not removed from this layer's
  `removeEventListener` bookkeeping, so removing it afterwards is a no-op
  rather than an error.
- **`on<type>` properties** (`canvas.onclick = fn`) are not wired for DOM
  elements. `addEventListener` is the whole surface here. (`Image` and
  `XMLHttpRequest` keep their `on<type>` slots — different objects, different
  file: `host_events.cpp`.)
- **`click`'s `offsetX` / `offsetY` are 0.** Not this layer: bro synthesizes the
  `click` event without `applyMouseOffset`, so every listener sees 0, compiled
  and interpreted alike. `mousedown`, `mouseup`, `mousemove` and `wheel` carry
  real offsets.
- **Listeners on arbitrary elements.** This layer creates `<canvas>` and
  `<img>` and nothing else, so the reachable targets are a canvas it made, the
  document (i.e. `documentElement`), and the window. There is no
  `querySelector`.
- A registration that cannot be delivered **throws**. A type that is not a
  string, a listener that is not a function, a target element that does not
  exist yet — each is a `TypeError` or an `Error` naming the object, never a
  registration that quietly never fires.

## Configure

```bash
cmake -B build -DBRO_WITH_BRONZE=ON            # needs ../bronze sibling checkout
```

Resolves bronze at `../bronze` (`-DBRONZE_DIR=<path>` overrides; a missing
checkout is a configure error — no submodule fallback). bronze's own configure
requires doctest, so the toolchain must provide it (bronze auto-detects a vcpkg
root when bro's configure didn't set one).

## Compile and run an app

```bash
# 1. compile the app to an OBJECT against the manifest (bronze repo, bronze CLI)
bronze build src/bronze_host/app/main_scenegraph.js \
    -o /tmp/app.obj \
    --emit-obj \
    --host-globals src/bronze_host/web_host.globals

# 2. link it into the host executable
cmake -B build -DBRO_WITH_BRONZE=ON -DBRO_BRONZE_APP_OBJ=/tmp/app.obj
cmake --build build --config Release --target bro-bronze-host

# 3a. run it windowed against the minimal app dir
./build/Release/bro-bronze-host src/bronze_host/app/appdir

# 3b. or headless for a fixed number of frames, which is what the check does
./build/Release/bro-bronze-host src/bronze_host/app/appdir --headless --frames 8

# 3c. or headless under a driver script — bro-headless, scripting THIS app
./build/Release/bro-bronze-host src/bronze_host/app/appdir --headless drive.js
```

### More than one app in one tree

`BRO_BRONZE_APP_OBJ` names *the* app and builds `bro-bronze-host`.
`BRO_BRONZE_APPS` is a semicolon list of `name=objpath` pairs, each building
`bro-bronze-host-<name>`, and the two live side by side — the pinned check
names `bro-bronze-host` and must keep finding exactly that binary:

```bash
cmake -B build -DBRO_WITH_BRONZE=ON \
    -DBRO_BRONZE_APP_OBJ=/tmp/scenegraph.obj \
    -DBRO_BRONZE_APPS="events=/tmp/events.obj;lit=/tmp/lit.obj"
cmake --build build --target bro-bronze-host-events
```

Every one of them is the same `host_main.cpp` linked against a different
object, because that is what "another app" is here. A pair with no `=`, an
empty half, a path that does not exist, or a name used twice is a configure
error naming the pair: a mistyped one would otherwise become a missing target,
and "the app is broken" would read exactly like "the app was never built".

`--emit-obj` is what makes step 1 stop before linking: the object is destined
for **bro's** toolchain, and linking belongs to whoever owns the final binary.
`--host-globals` is what makes the app's reads of `document` and friends resolve
to the host registry instead of throwing `ReferenceError` — the manifest and
`installWebHostGlobals` are two halves of one list and must stay identical.

The app object must export `bronze_main` (bronze's entry convention);
`installWebHostGlobals` runs before `bronze::embed::runMain()`, and the frame
loop then drives everything the app scheduled.

## Driving a compiled app from a script (3c above)

`--headless` **without** `--frames` is bro-headless
(`engine/headless_driver.h`), not a second driver: the same argument parsing,
the same script / `-e` / REPL modes, and the same globals
[docs/headless.md](../../docs/headless.md) documents.

```bash
bro-bronze-host <appdir> --headless script.js          # run a script, then exit
bro-bronze-host <appdir> --headless -e "advanceTime(500)" -e "screenshot('out.png')"
bro-bronze-host <appdir> --headless                    # interactive REPL
```

The compiled app has no JS realm — but the **Engine** does, because it still
boots the app dir's page, and that realm is where the driver script runs.
Driver and app share the Engine, the document and the clock, which is the whole
mechanism:

- `advanceTime(ms)` steps the engine, and each step fires `Engine::onFrame` —
  this layer's frame seam. So one `advanceTime(16)` is one `APP frame=N` from
  the compiled app, rAF callbacks and microtask checkpoint included.
- `screenshot()` / `getPixel()` composite the real frame, the app's WebGL
  canvas in it: the app appended that `<canvas>` to the same document, so
  `document.querySelector('canvas')` in the script finds it and
  `getContext('webgl2')` on it hands back the very context the app is drawing
  through.
- `assert()` fails the run with a nonzero exit, as in bro-headless.

What the script does **not** get is the app's own JS objects — there are none;
its scene graph is machine code with no reflective surface. A driver observes
the app the way a user does: through the DOM, the frame, and the pixels. The
seam this rides on is `HeadlessHooks::afterEngine`, which runs the host-globals
install and `runMain()` at the point an interpreted app's own JS would have
just finished.

`--headless --frames N` is unchanged and stays a separate mode: it is the one
`tests/bronze_host/` pins, and it must not depend on a script existing.

### Building the object as part of the bro build

Off by default, and it stays that way — a build that shells out to another
compiler should never be something you acquire by configuring bro:

```bash
cmake -B build -DBRO_WITH_BRONZE=ON \
    -DBRO_BRONZE_BUILD_APP=ON \
    -DBRO_BRONZE_APP_SOURCE=src/bronze_host/app/main_scenegraph.js
cmake --build build --target bro-bronze-host
```

That runs exactly the command in step 1 above, so the two cannot drift. It makes
the bronze CLI a build dependency of `bro-bronze-host` and recompiles the app
whenever the source or the manifest changes. `BRO_BRONZE_BUILD_APP=ON` and
`BRO_BRONZE_APP_OBJ` are mutually exclusive: one compiles the app here, the
other adopts an object someone else compiled, and a configure that accepted both
would silently pick a winner.

## The app

`app/main_scenegraph.js` exercises everything below the renderer: scene graph,
matrix math, the host DOM, the WebGL2 context object, timers, rAF and the
microtask checkpoint. The renderer apps import `WebGLRenderer` from r160's
published `build/three.module.js`, vendored byte-for-byte in the bronze
checkout (`bronze/tests/oracle/threejs/three.module.js` — origin and sha256 in
the README beside it): `app/main.js` is the basic cube, `app/main_lit.js` adds
`MeshStandardMaterial` + lights, `app/main_textured.js` a procedural
`DataTexture` checkerboard. Each prints `gl.readPixels` predicates after
`render()`, so a correct frame is checkable from stdout alone.
`app/MISSING_MODULES.md` records why the bundle, not ~200 vendored modules.

`app/appdir` is the app directory the executable boots from — an Engine still
needs a document even when the app's JS was compiled away.

## The hybrid app dir: `"compiled": true`

An app dir a compiled host boots from declares itself in its `bro.json`:

```json
{ "title": "my app", "compiled": true }
```

It is a **declaration, not a switch** — nothing in the engine behaves
differently on it (`EngineConfig::compiledApp`). It exists so a mismatch
between an app dir and the binary opening it can be *reported* rather than
discovered:

- **plain `bro` / `bro-headless` on a `"compiled": true` dir** logs a warning
  naming the situation and runs anyway. Not a refusal, because the interpreted
  half of a hybrid dir is real and does run — the page, its styles, its own
  `<script>` tags. What is missing is the app's logic, and an app that runs its
  page and none of its logic is otherwise indistinguishable from one that is
  simply broken. The warning is the difference.
- **a compiled host on a dir that does not declare it** warns the other way:
  add the flag, because it is what tells any *other* binary that this dir needs
  one.

The mechanism is one `LOG_WARN` pair at engine init (`engine_init.cpp` step 6).
A host executable that has a compiled app linked in says so with
`EngineConfig::hostProvidesCompiledApp` (or `HeadlessHooks::providesCompiledApp`
in driver mode); that flag describes the *binary*, and is not a manifest key.

### Why an app dir can carry interpreted JS at all

The engine executes the page's `<script>` tags unconditionally
(`engine_init.cpp` step 10), before the host runs the compiled top level. So a
hybrid dir is not a special mode: it is an ordinary app dir whose page happens
to hold UI script, running in the Engine's QuickJS realm beside a compiled
program running as machine code. They share the DOM on one thread and talk
through it — see "The boundary rule" above, and
`tests/bronze_host/appdir_events/` for a working one.

`tests/bronze_host/` holds the integration check that runs the compiled app and
diffs its output against a committed expectation.

## Deliberately not covered (yet)

**GL**: samplers, sync, occlusion queries, transform feedback, PBO paths,
`mapBufferRange`, `getIndexedParameter`, 3D/array textures, non-square matrix
uniforms, `vertexAttrib*` default-value setters, and `getContext('2d')`.
`getParameter`'s array-shaped answers are pseudo-arrays (indexable, `length`, no
`Array.prototype`).

**Events**: the exact list is under "Not supported, precisely" above.

**Loading**: `fetch`. It cannot be provided at all today — it returns a Promise,
and the embed API has no way to create or resolve a bronze Promise from C++.
`XMLHttpRequest` is here instead because it settles through callbacks, but note
that three.js r160's own `FileLoader` is fetch-based, so this does not unblock
three's loaders. `XMLHttpRequest` serves only `responseType` `''` and `'text'`
(an ArrayBuffer is another thing the embed API cannot construct) and only local
files (the network belongs to brokit, which is QuickJS-native).

**Images**: `data:` URLs (no base64 reader), http(s) sources (same reason as
above), and `ImageBitmap`. `Image` is a host object, not a `dom::Element` — it
has no layout box, and `img instanceof Image` is false because the embed API
cannot build an object on a chosen prototype.
