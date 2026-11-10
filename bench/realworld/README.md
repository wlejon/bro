# Real-world scenes: three.js and pixi.js in bro and Chromium

Seven deterministic scenes built on unmodified three.js r160 and pixi.js v8
(`vendor/`), each one page that runs in node (JS state only), bro and
Chromium: `three_meshes`, `three_instanced`, `three_skinned`, `three_lights`,
`pixi_sprites`, `pixi_graphics`, `pixi_text`. A scene's load is `n` (meshes,
instances, sprites, ...). The harness checks that bro runs them correctly,
then times them in a window against Chromium.

`rw.py` drives everything; `timing.py` holds its timing commands. Work files
go to `bench/realworld/.work/` (or `$RW_WORK`). Windowed runs use the Wayland
display in `WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` (default: helm's `wayland-0`).
bro comes from `build-release/`, Chromium is `chromium` on the PATH.

## How a scene gets its configuration

- **Chromium** opens `<scene>.html?n=...&mode=...` from `file://`, with
  `--allow-file-access-from-files` (it cannot reach a localhost server here).
  Files a page produces come back as `RWBLOB` console lines.
- **bro** runs a generated folder app per run (`.work/<run>/`): `lib/`,
  `scenes/` and `vendor/` symlinked, the page as `index.html`, the
  configuration as `config.js`. Its `bro.json` declares the `files`
  permission and the run grants it (`--grant declared`), so the page can write
  its output to `<run>/out/`. The console goes to `<run>/bro.log`.
- **node** takes `key=value` arguments (`mode=state`, and `mode=bench`: "JS bench" below).

pixi runs as shipped in both browsers: its `new Function` uniform, UBO and
shader sync generators compile in bronze, at the JIT tiers and in an AOT
`app.so` alike.

## Correctness levels

| Command | Level | What it checks |
|---|---|---|
| `rw.py l1` | 1, JS state | After K frames with no renderer, the scene's state (world matrices, the culled and sorted render list, bone matrices, pixi transforms and batched vertices) hashed in node and in bro-headless at each bronze tier (`jit`, `aot`, `tier0`, `tier2`, `deopt3`, `gcstress`). `match` is bit-exact; `ulp` means the float64 hash differs but the float32 hash and counts agree (last-ulp libm differences). |
| `rw.py surface` | 2a, WebGL surface | Extensions, `getParameter` values, shader precision and context attributes in windowed bro and Chromium, diffed. |
| `rw.py record` | 2b, WebGL calls | Every WebGL call of frames 0-2 at a small N, in both, diffed; prints the calls per frame and the functions whose counts differ. Equal per-frame counts mean the library made the same decisions in both. |
| `rw.py shot --scene S` | 3, pixels | `readPixels` of one frame in both, compared by `compare_shots.py`. |

## Timing

```bash
python3 bench/realworld/rw.py time --browser chromium|bro|bro-aot|bro-app --scene S --n N
python3 bench/realworld/rw.py sweep [--scenes a,b] [--out DIR]
python3 bench/realworld/rw.py perf --scene S --n N [--aot]
python3 bench/realworld/rw.py report --json DIR/timing.json [--out DIR/report.md]
```

`bro` is the folder under the JIT, `bro-aot` the folder with an `app.so`
from `bronze build`, and `bro-app` the app as an installed one runs: its own
executable with the page's scripts compiled at build (`bro --build-exe`,
docs/apps.md "Ahead-of-time scripts"). `bro-app` is the number an installed
app gets.

Each timed run renders 120 warm-up frames (shaders compile, JIT tiers
settle), then times 300. Per frame (`lib/timing.js`):

- **interval**: wall-clock time between consecutive rAF callback starts, the
  frame interval. The clock is `performance.now()` in Chromium and
  `__host.wallMs()` in bro, whose `performance.now()` and rAF timestamp are its
  frame clock (constant within a frame, snapped to the predicted vblank), so
  uncapped frames inside one refresh share a timestamp. The rAF timestamp
  difference is kept too, as `raf`.
- **cpu**: wall-clock time around the scene's update + render inside the
  callback.
- **gpu**: `TIME_ELAPSED_EXT` (EXT_disjoint_timer_query_webgl2) around the
  same WebGL work. bro's timestamps bracket the work's completion on the GPU;
  ANGLE on OpenGL ES may measure something narrower, so compare gpu within a
  browser more than across them.
- **delivered**: the span from the first timed frame's GPU completion (its
  timer query first seen available) to the last one's, per frame. Chromium's
  rAF loop does not wait for the GPU (it queues frames until its command
  buffer fills, then stalls), so where the GPU is the bottleneck the rAF
  interval says little; delivered is bounded by the GPU in both.
- **frame** = max(median rAF interval, delivered): what a point, a knee and a
  ratio are judged by.
- in bro, the flight recorder's records of the timed frames
  (`controlCommand('trace', ...)`, docs/agent-control.md): the frame split into
  input, tick (`js` of it: the rAF callback with its WebGL calls), composite,
  present, gc.

Frames are uncapped in both: Chromium with `--disable-gpu-vsync
--disable-frame-rate-limit`, bro with `vsync: false` and `maxFps: 0` in
bro.json (a MAILBOX or IMMEDIATE swapchain). bro clamps an unfocused window to
30 fps; a run records the window's focus at the start and end of timing
(`bro.window.focused`, `document.hasFocus()` in Chromium). On helm, Chromium's
rAF loop runs uncapped but its window commits to the compositor at 60 Hz
regardless of the flags (`WAYLAND_DEBUG=client` shows it); bro presents every
frame. Uncapped numbers are frames produced and finished by the GPU, not
frames shown.

`time` is one run. `sweep` steps N by x1.5 from the scene's minimum until
Chromium's median frame interval passes 16.7 ms (60 fps) and two steps
further, then runs bro (JIT and AOT) at the same N, each point twice (three
times when the two medians differ by more than 10%), one browser at a time,
with a fixed rest after each run (GPU-heavy runs rest as long as they ran, up
to 20 s, since halo's GPU slows down under a minute of sustained load). Slow bro
points time fewer frames (at least 30 + 60). It writes `timing.json` (summaries,
knees, ratios at Chromium's knee, perf groups) and `timing_raw.json` (every
run's per-frame arrays, by run id). With `--perf 1` (the default) it then
profiles bro at Chromium's knee with `perf`.

`perf` records the bro process with `perf record -k CLOCK_MONOTONIC` (user
space; brass writes a perf map of the JIT code with `BRASS_PERF_MAP=1`), reports
only the samples inside the timed window (the flight recorder's clock is
CLOCK_MONOTONIC too), and groups them: `js-compiled` (JIT code, or `app.so`
under AOT), `bronze-runtime`, `jit-compiler` (brass compiling on its threads),
`bro-webgl` (src/webgl), `bro-bindings` (src/bronze_host), `vulkan-driver`,
`skia`, `compositor-present`, `bro-engine`, `libc`, `window-system`, overall and
for the main thread alone.

The AOT mode builds `app.so` from the scene's module graph the way an app's
executable does: `bro-headless --print-host-globals --print-native-manifest`,
then `bronze build --emit-shared`. `config.js` is a classic script compiled at
boot, so one `app.so` per scene serves every N.

## JS bench: the scenes' JavaScript without the engine

```bash
node bench/realworld/scenes/<scene>.js mode=bench n=N warm=W frames=F
python3 bench/realworld/rw.py js <scene>|all [--n N] [--warm W] [--frames F] [--bronze PATH] [--flags "..."] [--harvest]
python3 bench/realworld/rw.py js <scene> --profile [--harvest]
```

`mode=bench` runs the page's whole frame, scene step plus the library's
render (three's `WebGLRenderer.render`, pixi's `renderer.render`), into
`lib/glstub.js`, a stub WebGL2 context, in node or in a `bronze build`
executable: no engine, no DOM, no GPU. It renders `warm` frames, times
`frames` more with `performance.now()` in one synchronous loop, and prints
`RW bench {...}` with ms, draw / GL / uniform calls and uploads per frame and
a checksum: the scene's state hash (float32 lanes) and the last frame's GL
call counts, so two engines that print the same checksum did the same work.

The stub (`lib/glstub.js`, `lib/glsl_reflect.js`, `lib/glconst.js`) has every
WebGL2 method; draws, uploads and uniform calls only count. Its surface is
Chromium's on a desktop GPU (32 texture units, highp, Chromium's 32
extensions). Programs link; their active uniforms, attributes and uniform
blocks are read from the shader source (preprocessed, structs and arrays
named as GL names them). A name counts as active when it occurs in some
function body, which approximates the driver's dead-code elimination: a local
variable with an attribute's name (three's `uv`) keeps that attribute active.
`lib/bench_env.js` stubs the rest: canvases whose 2D context measures glyphs
at a fixed advance and draws nothing, a `requestAnimationFrame` that never
calls back (pixi's scheduler and texture GC never run), a minimal `document`.

`rw.py js` (`jsbench.py`) builds each scene with bronze into
`.work/js/<scene>/<key>/` and runs node and the executable, each pinned to one
core (`taskset -c 4`) under `perf stat -e instructions:u,cycles:u`, twice,
the faster run reported, and checks the checksums agree (a mismatch is
printed as `MISMATCH`). Counting covers the timed frames only: with `mark=1`
the page prints a marker before and after them, padded so the write blocks
on a one-page pipe until `rw.py` has switched counting on or off through
perf's control fifo. Instructions are user space, all threads (node's JIT
compiler threads included, on the same core).

A bronze executable takes no arguments (its host passes none to the
program), so its configuration is compiled in: a generated `main.js`
imports `cfg.js` (`globalThis.BENCH_CONFIG`) and then the scene. A build is
cached by the digest of the bronze binary (mtime + size), the flags, the
configuration and the scene, `lib/` and `vendor/` files; a new N is a new
build (about a minute: three.js and pixi are big). `bronze build
scenes/<scene>.js` alone also works and benches at the scene's defaults.
A bronze executable has no `eval` either, and pixi generates its uniform,
UBO and shader sync functions with `new Function`: for pixi scenes `rw.py`
first runs node with `capture=<file>`, which records every body pixi passes
to `Function`, and compiles them into the build (`gen.js`, a `Function` that
returns the compiled body for its arguments). The code that runs is the
same; it is compiled ahead of time instead of at run time.

`--harvest` builds the way an installed app's scripts are built: a plain
build with `--harvest-out`, one run to write the inline-cache harvest, and
the build on it (`--feedback-in`), which is the one measured (bronze
docs/type-feedback.md, "Harvest"). `--profile` records the bronze
executable's timed frames with `perf record` and prints the top 25 symbols.

What the bench leaves out, so its ms is not the frame time:

- every scene: the WebGL implementation (bro's command encoding and
  WebGL thread, Chromium's ANGLE and GPU process), the driver, the GPU,
  presentation and the rest of the engine's frame.
- `three_skinned`: the bone texture upload per skeleton per frame (counted,
  not copied); `three_instanced`: the instance-matrix buffer upload.
- `pixi_sprites`, `pixi_graphics`: the vertex / index buffer uploads.
- `pixi_text`: glyph rasterisation (`fillText` / `strokeText` draw nothing,
  `measureText` is the fixed-advance stub) and the per-text canvas texture
  upload. In the browsers these are most of the frame (Chromium ~19 ms at
  n=400 against ~1.5 ms for node here); the bench is pixi's text layout,
  texture pooling and batching only.
- pixi's text uses its own grapheme fallback (`[...s]`) in both engines (node
  has `Intl.Segmenter`, a bronze executable does not).
