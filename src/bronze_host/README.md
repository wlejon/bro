# bronze_host

The engine's JavaScript surface for [bronze](https://github.com/wlejon/bronze):
the browser-shaped host globals (`document`, `window`, rAF, timers, `Image`,
`fetch`, observers, WebGL2, ...), bro's own namespaces (`bro.*`), every
sibling library's `<name>_api`, and the per-frame seam that runs JS. Page
scripts are compiled in-process at boot (`eval_jit.cpp`); an app folder may
instead carry an AOT-compiled `app.dll` / `.so` / `.dylib`.

## Compiled app modules

`bro <folder>` / `bro-headless <folder>` load `app.<ext>` beside `index.html`
(`app_module.cpp`), compare its ABI stamp with the runtime's and refuse a
mismatch. The module shares the host's runtime: link `bronze::runtime_shared`
only, never `bronze::embed` + `bronze::runtime` as well, or the module gets a
second heap and values are collected out from under it.

```bash
# the manifests of the binary that will run the app
./build/Release/bro-headless src/bronze_host/fixtures/appdir \
    --print-host-globals --print-native-manifest natives.json > host.globals

# compile the app into the folder that carries it
cmake --build build --config Release --target bronze-cli
BRONZE_SHARED_RT_LIB=$PWD/build/shared/Release/bronze_runtime_shared.lib \
  ./build/Release/bronze.exe build app.js -o myapp/app.dll \
    --emit-shared --host-globals host.globals --native-manifest natives.json

./build/Release/bro myapp
```

`BRONZE_SHARED_RT_LIB` is needed under a multi-config generator, where the
import library is one directory deeper than the CLI searches. Without
`--native-manifest` the app still runs; `bro.*` natives are then ordinary
property reads instead of direct calls. `tests/bronze_host/lib.sh` compiles
probes into app dirs on demand and is the worked example.

A `bro.json` with `"compiled": true` declares that the folder's logic is a
module; opened with none, bro logs a warning and runs the page anyway.

## The frame seam

`Engine::onFrame`, at the point the engine fires rAF (windowed frame, each
`advanceTime` step headless, the server tick), in this order:

1. leftover microtask drain
2. clock advance
3. host tasks (image loads, XHR/fetch completions, `webkitGetAsEntry` callbacks)
4. timers
5. rAF callbacks
6. MutationObserver / ResizeObserver delivery
7. microtask checkpoint

So anything asynchronous here resolves at frame granularity: an abort rejects a
fetch one frame after `abort()`, and a mutation made in an event handler is
reported in the next frame.

## Collecting between frames

`hostCollectBetweenFrames` (`host_gc.cpp`) runs once a frame is presented,
before the pacing wait: the JS heap is collected there, ahead of its own
triggers, so a pause falls in the time the frame has left rather than inside a
frame (the flight recorder's `gc` phase, with `gcWork` naming what ran:
`minor`, `full`, `start`, `mark`, `finish`, `sweep`). Each frame:

- a request from `hostNotifyIdleFrame` (idle, periodic, memory pressure on a
  quiet page) or the boot is a stop-the-world full collection; memory pressure
  on a busy page starts a marking cycle instead;
- a minor collection once eden is half its adaptive limit;
- the old generation incrementally (bronze `embed_gc.h`), by paced marking
  cycles. A cycle starts as late as lets it finish by 80% of the old
  generation's full-collection threshold at the frame's GC budget
  (`BRO_GC_FRAME_MS`, 0.5 ms): the growth left to 80%, at the promotion rate
  (old growth a frame, a running average), against the marking the last
  cycle took per MB of its threshold (times 1.5, plus four frames for the
  finish); never below 20% of the threshold, always by 75% (where the
  stop-the-world policy collects). So cycles run about as rarely as the
  stop-the-world fulls did, and a minor collection inside one, which costs
  more (it shades what it promotes), is rare. Each frame then gets a mark
  slice of what is left of the budget after a minor collection, never less
  than 0.25 ms, and more (up to twice the budget) when the estimate says the
  marking left would not be done by 80% at it. When a slice has drained the
  grey work, the next frame runs a minor collection if eden is a tenth full,
  and the one after finishes the cycle on its own (it evacuates the whole
  young generation, so it is kept small); then sweep slices from the same
  budget until swept. Only then can the next cycle start.
- A cycle still marking at 90% of the threshold is finished at once, one
  longer pause between frames, before the heap's own trigger would finish it
  at an allocation inside a frame. A host that never slices still gets the
  heap's own collections.
- A slice marks on brass's parked marker threads (`BRASS_GC_MARK_POOL=0`
  creates and joins them per slice, as before, which cost a 0.4 ms slice
  most of its parallelism). The same threads copy a parallel minor
  collection. `hostStartGcHelpers` creates them once, in the first
  between-frames slot (headless: after the page's first frame, before the
  test script), through bronze `embed::startGcHelperThreads`, rather than in
  the first collection that wants them, where creating seven took 0.7 ms of
  its pause. `BRO_GC_PRESTART_HELPERS=0` leaves them to that collection, for
  A/B. Workers' heaps never start theirs ahead: most never collect in
  parallel.

`BRO_GC_INCREMENTAL=0` restores the stop-the-world policy (a full collection
between frames at 75% of the threshold), for A/B. `BRO_GC_CYCLE_START`
(a fraction) fixes where cycles start in place of the paced start; 0 keeps one
always open, for measuring them on a heap that seldom grows that far. `BRO_GC_LOG=1` logs
a line per frame that collected (`gc: between frames 0.61 ms: minor 0.35
mark 0.26`) and one per cycle with its frames, the fills it started and finished at, its
slices against the estimate, finish and sweep, and the promotion rate;
`BRASS_GC_LOG=1` adds brass's line per collection. `perf.stats().bronze`
carries `gcCycles`, `gcCycleFrames` and `gcCycleOpen`.

## Adding a native or a namespace

- bro's own namespaces: `natives/<sub>/` (`native_<sub>_decl.h`,
  `native_<sub>_register.cpp`), the body in `native_<sub>.cpp`, the JS wrapper
  in `js/<sub>.js` (or `js/bro_core.js`), and `js/module.globals` when the
  wrapper reads a new root. Edit them together.
- Natives live under `__bro_native.<sub>` and the wrapper names each by its
  full dotted path (an alias defeats the direct-call lowering). Scalars cross
  as scalars, a fixed compound shape as its pieces, a dynamic value as JSON
  text, a list of strings as one newline-joined string, and a callback as a
  `dynamic` the C side keeps in a `Persistent` and calls from the frame seam.
- Sibling libraries: `installSiblingApis` (`host_sibling_apis.cpp`) is the only
  caller of any sibling `install*()`, once per realm. A second install
  `fatal()`s (brotensor) or makes classes that fail `instanceof` against the
  first. Workers use `installWorkerSiblingApis`.
- Install order (`installBroRoots`, `host_bro_root.cpp`): roots → sibling
  APIs → bro's hand-built namespaces and compiled-out stubs → natives →
  `js/bro_core.js`.
- A class an app can `instanceof` goes through `HostClass` (`host_class.cpp`).
- **Register a host global in every build.** A manifest name the registry does
  not have is `fatal()` at the read, not a `ReferenceError`. A compiled-out
  feature registers its names bound to `undefined` (e.g. `VideoEncoder` with
  `BRO_WITH_VIDEO=0`).

## Workers

A `Worker` is its own thread and realm with brokit, `ImageBitmap`,
`FastNoise`, and a cut-down `bro` (`installWorkerBroRoot`): `bro.net` +
`bro.net.sync`, `bro.server`, `bro.motion`, `bro.math`, `bro.gpu`,
`bro.media`, and the sibling compute APIs (ear, `SynthGraph`, gameagent, mesh,
rigging, tensor, lm, soundml compute, diffusion, vision, flora, image, themes,
keys, search). No
`bro.time`, `bro.window`, `bro.settings`, scene, physics, `AudioContext` or
`bro.mic`, and `document` is a `ReferenceError`.

## Differences from the web

- `once` listeners reaped by the engine stay in `removeEventListener`'s
  bookkeeping (removing one later is a no-op).
- Reassigning `el.on<type>` moves the handler to the end of the listener
  order; `document` and `window` have no `on<type>` slots.
- `click` has `offsetX` / `offsetY` of 0.
- An event object is live only during the listener call; calling
  `preventDefault()` etc. on a stored one throws.
- The object passed to `dispatchEvent` is the event every listener receives,
  `detail` untouched. An engine-raised `CustomEvent` carries a string `detail`,
  parsed as JSON when it starts with `{` or `[`.
- MutationObserver adds `unobserve(target)`; `addedNodes` / `removedNodes`
  hold at most one node; comment `data` changes are not observed.
- ResizeObserver reports once per frame.
- `DOMParser` documents are never freed. Appending a parsed node adopts it.
- `VideoEncoder` / `GifEncoder`: no `VideoFrame`; `addCanvasFrame` takes a 2D
  canvas only (use `addViewportFrame` for WebGL/scene); call `finish()` or the
  file has no trailer.
- `settings.get` types values by content, so a string that reads as a number
  comes back as a number.
