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
- **node** takes `key=value` arguments (`mode=state` only).

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
python3 bench/realworld/rw.py time --browser chromium|bro|bro-aot --scene S --n N
python3 bench/realworld/rw.py sweep [--scenes a,b] [--out DIR]
python3 bench/realworld/rw.py perf --scene S --n N [--aot]
python3 bench/realworld/rw.py report --json DIR/timing.json [--out DIR/report.md]
```

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
