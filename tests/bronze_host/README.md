# bronze_host integration checks

Twenty-two checks, one manifest. `tests/run_tests.sh` enumerates them
(`run_checks.sh --list`) and runs each with the rest of the suite — exit 77
counts as SKIP there, and `BRO_TEST_BRONZE=0` leaves them out. By hand:

```bash
tests/bronze_host/run_checks.sh           # run all, with a summary
tests/bronze_host/run_checks.sh dom       # run one (exit 0 pass, 1 fail, 77 skip)
tests/bronze_host/run_checks.sh --list    # the names
```

The folder is three files. `lib.sh` holds the mechanics — binary discovery,
module build, output cut, diff — as `bh_run_check`. `run_checks.sh` is the
manifest: one short function per check, under a comment saying what only that
check catches. `run_loader_test.sh` stays its own file (run as the `loader`
check) because its subject is the loader itself, not a compiled app: it builds
four synthetic C modules to hit every refusal branch.

| name | subject |
|---|---|
| `loader` | the folder loader: ABI guard, missing entry point, non-module files |
| `scenegraph` | the three.js scene graph, a fixed frame count |
| `events` | event dispatch, under a driver script, on the hybrid app dir |
| `fetch` | fetch, under a driver script |
| `dom` | the element surface: tree, style, forms |
| `node` | text nodes, comments, fragments, cloneNode |
| `file` | Blob, File, FileReader, object URLs |
| `abort` | AbortController, AbortSignal, cancelled fetch |
| `observer` | MutationObserver, incl. a mutation made by the page |
| `resize` | ResizeObserver: the initial report, and the quiet frames |
| `parser` | DOMParser: a second document, and adoption out of it |
| `proxy` | the four proxy-backed live views: style, computed, dataset, localStorage |
| `class` | host classes, via Image: born-on-prototype, shared methods, instanceof |
| `input` | pointer lock, fullscreen, Gamepad |
| `video` | VideoEncoder, GifEncoder: every pixel source, and every refusal |
| `audio` | Web Audio & Sound Engine: nodes, params, buffers, decodeAudioData |
| `physics` | Jolt Physics: bodies, character controller, soft bodies, queries |
| `ai` | navmesh, nav grid, agents |
| `net` | bro.net over GNS, WebSocket, remote HTTP transport |
| `wild` | wild three.js scene + OrbitControls |
| `instanced` | instanced mesh under load (2,500 instances) |
| `pixi` | pixi.js v8: WebGL sprites + pixel readback |

**All twenty-two run the stock `bro-headless`** — the same binary every other test in
`tests/` uses. Each one's app is a directory carrying a compiled `app.dll` /
`app.so` / `app.dylib`, which `lib.sh` builds on demand with the bronze CLI and
rebuilds whenever the module is older than its probe **or than the compiler**
(a module carries the ABI stamp of the bronze that emitted it, so a rebuilt
runtime invalidates every module in the tree without any `.js` changing).

Nine of them — `node`, `file`, `abort`, `observer`, `resize`, `parser`,
`video`, `proxy` and `class`, the newer half of the layer — are written to a stricter rule than
their neighbours: every line of output is `APP <name>=<value>` and every
expectation beside them was derived from the spec and the fixture BEFORE the
first run, so a passing check is evidence the implementation matches the model
rather than a recording of what the build happened to print. (Named rather than
counted off the end of the list: the three.js checks sit last and are not
among them.)

This used to be seven executables — `bro-bronze-host`, `bro-bronze-host-dom`,
one per app, each `host_main.cpp` linked against a different object file — plus
the CMake surface that enumerated them. That is what the folder model replaced,
and the seven scripts' identical copies of "find my binary" became one `lib.sh`.

A check skips rather than fails when `bro-headless` is absent, or when the tree
has no bronze CLI (`-DBRONZE_WITH_LLVM=OFF`) and no already-built module. Skip
stays distinct from failure on purpose: "this tree cannot build the subject" and
"the subject is broken" must never read the same.

## Why these are shell checks and not `test_*.js`

The suite evaluates a `test_*.js` inside the app's JS realm. A compiled app
has no JS realm to evaluate anything in — the app is machine code, and what
the driver scripts here script is the *engine's* realm beside it. So each
check is a shell script, and `run_tests.sh` runs it as a process and reads its
exit code instead. What is conditional is not the binary (every build's
`bro-headless` can load a module) but the bronze CLI that PRODUCES one: a tree
configured `-DBRONZE_WITH_LLVM=OFF` has no way to build the subject, and exits
77 so that "this tree cannot build the subject" never reads as "the subject is
broken".

## What it actually proves

The subject is `src/bronze_host/fixtures/main_scenegraph.js` compiled to machine code
by bronze into the module `fixtures/appdir` carries. Getting one matching line out of it
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
`appdir_events/app.dll` — a **hybrid** app dir: a
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
`bh_run_check`'s `--split-streams` option says the same at the code.

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


## Building the modules by hand

`lib.sh` does this automatically; it is written out here because a check that
compiles its own subject should not be the only description of how.

```bash
BRONZE_SHARED_RT_LIB=$PWD/build/shared/Release/bronze_runtime_shared.lib ./build/Release/bronze.exe build tests/bronze_host/apps/dom_probe.js     -o tests/bronze_host/appdir_dom/app.dll     --emit-shared --host-globals src/bronze_host/web_host.globals
```

`BRONZE_SHARED_RT_LIB` is needed under a multi-config generator: bronze searches
`shared/` beside and above the CLI, and MSBuild writes the import library one
level deeper, in `shared/<Config>/`.

Two of the probes are slow to compile — `instanced_mesh_probe.js` takes minutes
and gigabytes, and `pixi_sprites_probe.js` more of both, while `dom_probe.js`
takes about three seconds. The modules are build output and are not committed,
so a first run of those two checks pays that cost.
