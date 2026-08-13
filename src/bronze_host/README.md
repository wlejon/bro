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
| `host_timers.cpp` | `setTimeout`/`setInterval` and the main-thread task queue |
| `host_image.cpp` | `Image`, and the decode behind `.src` |
| `host_xhr.cpp` | `XMLHttpRequest` (text over the app asset path; see its header) |
| `gl_*.cpp`, `gl_internal.h` | the WebGL2 binding, one file per call family |
| `host_main.cpp` | `bro-bronze-host`: Engine, globals, `runMain()`, frames |

## The frame seam, which is the thing to understand first

`installThreejsHostGlobals` registers one `Engine::onFrame` callback. The engine
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
    --host-globals src/bronze_host/threejs_host.globals

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

`--emit-obj` is what makes step 1 stop before linking: the object is destined
for **bro's** toolchain, and linking belongs to whoever owns the final binary.
`--host-globals` is what makes the app's reads of `document` and friends resolve
to the host registry instead of throwing `ReferenceError` — the manifest and
`installThreejsHostGlobals` are two halves of one list and must stay identical.

The app object must export `bronze_main` (bronze's entry convention);
`installThreejsHostGlobals` runs before `bronze::embed::runMain()`, and the frame
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

`app/main_scenegraph.js` is the runnable one: scene graph, matrix math, the host
DOM, the WebGL2 context object, timers, rAF and the microtask checkpoint — but
no renderer. `app/main.js` is the cube app with a real `WebGLRenderer`, and it
**does not compile today**: the renderer is not in the vendored three.js tree.
`app/MISSING_MODULES.md` has the exact missing list, the ~200-module transitive
closure, and the three options for closing it.

`app/appdir` is the app directory the executable boots from — an Engine still
needs a document even when the app's JS was compiled away.

`tests/bronze_host/` holds the integration check that runs the compiled app and
diffs its output against a committed expectation.

## Deliberately not covered (yet)

**GL**: samplers, sync, occlusion queries, transform feedback, PBO paths,
`mapBufferRange`, `getIndexedParameter`, 3D/array textures, non-square matrix
uniforms, `vertexAttrib*` default-value setters, `getContext('2d')`, and event
*dispatch* to canvas/document listeners (they are stored, never fired).
`getParameter`'s array-shaped answers are pseudo-arrays (indexable, `length`, no
`Array.prototype`).

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
