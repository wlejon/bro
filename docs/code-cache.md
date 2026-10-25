# Code cache

Every script bro compiles in-process (page scripts, headless driver scripts, workers) goes through bronze's on-disk code cache. A launch of an unchanged app loads the compiled IL and skips parsing, type inference and lowering. Machine code is not cached.

## When an entry misses

Any of these differs from the stored entry:

- the bro/bronze executable (path, size, mtime): any rebuild starts a new key space;
- the entry script, its resolution path, the asset mounts, the host globals and natives manifest;
- any `BRONZE_*` environment variable except `BRONZE_TIMINGS`;
- any source file the module graph read, the file list of an `import()` glob directory, or a bare specifier's resolution.

`BRO_JIT_TIER` is not part of the key.

On a reload each script unit is looked up separately: the classic scripts of a page are one unit, each `<script type="module">` is its own. An edit to any file of a unit recompiles that whole unit.

The log (`bro.log`, or stderr headless) says per compile whether it hit and why not: `compiled D:/app/index.html in 212 ms (code cache hit)`.

## Location and knobs

`<user cache dir>/code-cache/`: `%LOCALAPPDATA%\bro\code-cache`, `~/Library/Caches/bro/code-cache`, `$XDG_CACHE_HOME/bro/code-cache`. Held to 512 MB of disk space (whole 4 KiB units, warm lists `.bzw` included): a store that takes it past the limit has a background thread delete the least recently used entries, each with its warm list, down to 7/8 of the limit. The page thread never walks the directory; engine shutdown waits for a trim in progress. Deleting it is always safe.

| Variable | Effect |
|---|---|
| `BRO_CODE_CACHE=0` | no cache |
| `BRO_CODE_CACHE_DIR=<dir>` | another directory |
| `BRO_CODE_CACHE_MAX_MB=<n>` | the trim limit |

## SPIR-V cache

GLSL compiled at run time (a world's modules spliced into the scene's shaders, custom shader chunks) is kept as SPIR-V in `<user cache dir>/spirv-cache/`, a file a source named by a 128-bit hash of the stage and the source, so a second run skips glslang. `BRO_SPIRV_CACHE=0` turns it off. Deleting it is always safe.

## Shader warm-up

A world's shader sets (its land, processes, regions, plants, grass and clouds modules spliced into the scene's shaders) are compiled on worker threads, never on the frame thread: a node draws nothing of itself (the grass draws its built-in set, without blades) until its set is in, and `scene.pendingWork()` says `shaders: compiling 12 / 24` meanwhile. The mesh drawer's graphics pipelines are built the same way, off the frame thread, the draw skipped until each is in (`pipelines: 3 building`), and so are the shadow casters' (built-in, cutout, grass, plants), the clouds' (the march and every bake: the deck is neither baked nor drawn until they are in, `cloudStats().compiled` false meanwhile) and the land's fill, process and region kernels (the land not ready until they are); a surface layer tile's fill, made once, still builds its pipeline when it asks. Whatever a frame skips for a pipeline or a set still coming says so (`pipelines: 2 draws skipped last frame`), so `perf.settle` waits for a frame that skips nothing, and the shadow pages, which cache what was drawn into them, are all drawn again once a frame skips nothing. That is a window's policy (`bro`, `bro --drm`): its frames never wait, so the OS never sees it stop answering. Headless renders are read back at once (tests, stills), so there a frame waits for what it draws, the compiles still running on the workers; `BRO_ASYNC_SHADERS=1` gives headless the window's behaviour (to measure boot frames as a window has them), `0` gives a window the wait.

Each run records the sets it asked for in the app's cache dir (`<bro.app.cacheDir>/shader-warmup.bin`: the stages by name and the modules' sources). The next launch reads it as the engine starts and queues them all before the page's script has run, beside the device's bring-up and the page's own compile, so after a rebuild (every SPIR-V cache entry missed) the compiles are done or under way when the nodes ask, and a set a node asks for while it compiles is joined, not compiled twice. The record is written again from what the run asks for, so a set the app no longer uses drops out. `BRO_SHADER_WARMUP=0` neither reads nor writes it. Deleting it is always safe.

## Vulkan pipeline cache

`<user cache dir>/pipeline-cache/`, one file per GPU and driver, dropped past 128 MB. `BRO_PIPELINE_CACHE=0` keeps it in memory only, `BRO_PIPELINE_CACHE_DIR=<dir>` moves it. Deleting it is always safe.
