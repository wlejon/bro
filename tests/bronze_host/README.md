# bronze_host integration checks

Three checks, run by hand. Exit codes are `0` pass, `1` fail, `77` skip (the
binary is not built).

```bash
tests/bronze_host/run_bronze_host_test.sh   # the scene graph, a fixed frame count
tests/bronze_host/run_events_test.sh        # events, under a driver script
tests/bronze_host/run_fetch_test.sh         # fetch, under a driver script
```

They pin three different executables, which is why they are three scripts. The
first boots `bro-bronze-host` headless, advances a fixed number of frames, and
compares the compiled app's output against
`expected/main_scenegraph.expected` line for line. The second boots
`bro-bronze-host-events` under bro-headless's driver — the only mode that can
produce a click — and compares both worlds' output against
`expected/events_probe.expected`. The third boots `bro-bronze-host-fetch` under
the headless driver and checks `expected/fetch_probe.expected`. The multi-app
CMake surface (`BRO_BRONZE_APPS`, `src/bronze_host/README.md`) exists so one
configured tree can hold all three.

## What it actually proves

The subject is `src/bronze_host/app/main_scenegraph.js` compiled to machine code
by bronze and linked into `bro-bronze-host`. Getting one matching line out of it
requires every seam in the layer to be right at once:

- the compiled program's reads of `document`, `window`, `requestAnimationFrame`,
  `performance`, `setTimeout` and `WebGL2RenderingContext` reach the host
  registry — which they only do if the app was compiled against
  `web_host.globals` and that file still matches what
  `installWebHostGlobals` registers;
- `document.createElement('canvas')` builds a real `dom::Element`,
  `body.appendChild` puts it in the engine's document, and
  `canvas.getContext('webgl2')` reaches `Engine::createWebGL2Context`;
- the width/height setters resize the drawing buffer, and the engine's
  per-frame `syncWebGLCanvasSizes` then leaves it alone;
- `Engine::onFrame` fires the host seam once per `advanceTime` step, and the
  seam runs timers before rAF and the microtask checkpoint after it — the
  `timeout` line lands before frame 0 and each `microtask=N` line lands between
  frame N and frame N+1 for exactly that reason;
- three.js's own arithmetic survives compilation: the scene graph builds and
  `updateMatrixWorld` produces a matrix that is still orthonormal with
  determinant 1 after five frames of accumulated rotation.

A wrong answer on any of them moves or removes a line.

## What the events check proves

Its subject is `apps/events_probe.js` compiled to machine code, linked into
`bro-bronze-host-events`, booted on `appdir_events/` — a **hybrid** app dir: a
`bro.json` with `"compiled": true`, and an `index.html` whose own `<script>` is
interpreted UI JS running beside the compiled program. `drive_events.js` is the
driver. Three programs, two languages, one Engine, one DOM, one thread.

- a `click(20, 30)` goes through hit testing and the real input pipeline and
  lands in a listener the **compiled** app registered on the canvas it created
  — and `event.target === canvas` inside it, the same value the program holds;
- `mouseMove` and `wheel` reach the same canvas with their payloads intact,
  and a `keyDown` with nothing focused reaches the compiled app's `document`
  listener by **bubbling** to `documentElement`, which is where a document
  registration actually lives;
- a `CustomEvent` crosses **both** directions with a string `detail`: the page
  script dispatches `page:toApp`, a compiled listener answers on `app:toPage`,
  and the page script prints what came back. `PAGE fromApp=pong:one` cannot
  exist unless both crossings happened;
- `preventDefault()` from a compiled listener makes the interpreted
  dispatcher's `dispatchEvent(...)` answer `false`, and `stopPropagation()`
  from a compiled canvas listener keeps a compiled document listener from ever
  seeing the same event — so cancellation is one shared walk and not two.

The output is diffed as **two blocks** — every `APP ` line, then every
interpreted line — because the compiled app prints to stdout and the engine log
carrying the interpreted `console.log`s is stderr. Two streams, two buffers, so
their interleaving is not something a byte-for-byte expectation may depend on.
Each stream's own order is pinned, and causality across the boundary survives
the split because it is carried in the payload rather than in the interleaving.
`run_events_test.sh` says the same at the code.

## Why the expectation is only booleans and integers

Same rule as `bronze/tests/oracle/threejs/README.md`. Nothing accumulated is
pinned: the rotation loop accumulates rounding in every matrix element, so what
is checked for it is what stays true of any rotation matrix, inside a tolerance
(1e-12) far wider than double rounding over a few dozen flops and far tighter
than any miscompilation or wrong host value. A pinned float that came out of an
accumulation would be a record of what the build printed, not of what is true.

The GL query in the app prints a boolean for the same reason from the other
direction: `MAX_TEXTURE_SIZE` is the driver's number, so what is pinned is that
the context answered at all.

## Why it is not in `tests/run_tests.sh`

Two reasons, and either alone would be enough:

1. The suite discovers `test_*.js` and evaluates each through `bro-headless`.
   There is no JS realm here to evaluate anything in — the app is a linked
   object file with no scripting surface.
2. `bro-bronze-host` does not exist unless somebody configured
   `-DBRO_WITH_BRONZE=ON` and supplied an app object. Auto-discovering a check
   whose binary is absent from every default build makes "missing" and "broken"
   the same result.

The events check is not in the suite for reason 2 alone — its driver *is* JS,
but its binary is just as absent from a default build.

## Getting the binaries

`src/bronze_host/README.md` has the sequence: compile the app with bronze's
`--emit-obj --host-globals`, configure bro with `-DBRO_WITH_BRONZE=ON`, build.
Both at once, from the bro tree, with `<bronze>` the sibling bronze checkout's
CLI:

```bash
<bronze> build src/bronze_host/app/main_scenegraph.js -o build/scenegraph.obj \
    --emit-obj --host-globals src/bronze_host/web_host.globals
<bronze> build tests/bronze_host/apps/events_probe.js -o build/events.obj \
    --emit-obj --host-globals src/bronze_host/web_host.globals
<bronze> build tests/bronze_host/apps/fetch_probe.js -o build/fetch.obj \
    --emit-obj --host-globals src/bronze_host/web_host.globals

cmake -B build -DBRO_WITH_BRONZE=ON \
    -DBRO_BRONZE_APP_OBJ=$PWD/build/scenegraph.obj \
    -DBRO_BRONZE_APPS="events=$PWD/build/events.obj;fetch=$PWD/build/fetch.obj"
cmake --build build --config Release \
    --target bro-bronze-host bro-bronze-host-events bro-bronze-host-fetch
```

Re-running any check after editing its `.js` means recompiling that object
and relinking its executable — the app is the linked object, so nothing else
notices the edit.
