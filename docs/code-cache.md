# Code cache

Every script bro compiles in-process (page scripts, headless driver scripts, workers) goes through bronze's on-disk code cache. A launch of an unchanged app loads the compiled IL and skips parsing, type inference and lowering. Machine code is not cached.

## When an entry misses

Any of these differs from the stored entry:

- the bro/bronze executable (path, size, mtime): any rebuild starts a new key space;
- the entry script, its resolution path, the asset mounts, the host globals and natives manifest;
- any `BRONZE_*` environment variable except `BRONZE_TIMINGS`;
- any source file the module graph read, the file list of an `import()` glob directory, or a bare specifier's resolution.

`BRO_JIT_TIER` is not part of the key: every tier lowers the same IL (guarded method inlining included), so a unit cached at one tier loads at another.

On a reload each script unit is looked up separately: the classic scripts of a page are one unit, each `<script type="module">` is its own. An edit to any file of a unit recompiles that whole unit.

The log (`bro.log`, or stderr headless) says per compile whether it hit and why not: `compiled D:/app/index.html in 212 ms (code cache hit)`.

## Location and knobs

`<user cache dir>/code-cache/`: `%LOCALAPPDATA%\bro\code-cache`, `~/Library/Caches/bro/code-cache`, `$XDG_CACHE_HOME/bro/code-cache`. Held to 512 MB of disk space (whole 4 KiB units, warm lists `.bzw` included): a store that takes it past the limit has a background thread delete the least recently used entries, each with its warm list, down to 7/8 of the limit. The page thread never walks the directory; engine shutdown waits for a trim in progress. Deleting it is always safe.

| Variable | Effect |
|---|---|
| `BRO_CODE_CACHE=0` | no cache |
| `BRO_CODE_CACHE_DIR=<dir>` | another directory |

## Kernel cache

Every brokernel compile goes through a second on-disk cache, brokernel's
(`brokernel/compile_cache.h`). That covers `bro.kernel.compile` /
`compileAsync`, scene hooks (`setShader` / `setShaderAsync`, volume hooks) and
the scene's built-in stages. An entry stores everything the compile produced: SPIR-V, reflection, types,
constants, probes, costs, hashes and diagnostics. A hit returns the same
result a compile would. A kernel's CPU form is lowered the first time it runs
on the CPU (`eval`, `run`), at about the cost of one compile; a `@cpu`
kernel's begins on a thread of its own as its compile ends (docs/kernel-api.js). Only successful
compiles are stored.

An entry misses when any of these differs:

- the compiler build (the binary holding brokernel and brass), the version
  and the host's AVX2/FMA;
- the source and its file name;
- the modules registered by name (a hook's `modules`, `registerModule`), each
  by a hash of its source, so a built-in stage keeps an entry beside each hook
  it is compiled with;
- every module the compile read, each resolved again as the compiler would
  now resolve it and compared by a hash of its source;
- every compile option, knob overrides included.

It lives in `<user cache dir>/kernel-cache/` and has the same 512 MB limit
and least-recently-used trim as the code cache. Deleting it is always safe.

| Variable | Effect |
|---|---|
| `BRO_KERNEL_CACHE=0` | no cache |
| `BRO_KERNEL_CACHE_DIR=<dir>` | another directory |

`BRO_SCENE_SHADER_SOURCE=1` reads the built-in stages from the source tree.
Their compiles still go through the cache, keyed by the text read.

## Pipelines built off the frame thread

The scene's graphics pipelines (the mesh drawer's, the shadow casters') and its async compute pipelines are built on threads of their own, never on the frame thread: a draw is skipped until its pipeline is in, and `scene.pendingWork()` says `pipelines: 3 building` meanwhile. Whatever a frame skips for a pipeline still coming says so (`pipelines: 2 draws skipped last frame`), so `perf.settle` waits for a frame that skips nothing, and the shadow pages, which cache what was drawn into them, are all drawn again once a frame skips nothing. Every frame keeps that policy, a window's (`bro`, `bro --drm`, which so never stops answering) and headless's alike: headless runs the frames a window shows. What reads a frame's pixels back draws what it skipped: before a headless capture (`screenshot`, `getPixel`, `frameImage`), a page's `captureFrame` or `toImageData`, every scene whose last render skipped a draw renders once more, waiting for those pipelines, in creation order (so a scene drawing another's output through `asTexture` samples the redrawn one), and `captureFrame`'s own render waits for them, in a window as in headless. A reflection probe's capture, kept until asked again, waits for its pipelines too. `perf.asyncShaders(false)` makes every frame wait instead.

## Vulkan pipeline cache

`<user cache dir>/pipeline-cache/`, one file per GPU and driver, dropped past 128 MB. `BRO_PIPELINE_CACHE=0` keeps it in memory only, `BRO_PIPELINE_CACHE_DIR=<dir>` moves it. Deleting it is always safe.
