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

`<user cache dir>/code-cache/`: `%LOCALAPPDATA%\bro\code-cache`, `~/Library/Caches/bro/code-cache`, `$XDG_CACHE_HOME/bro/code-cache`. Trimmed to 512 MB, least recently used first. Deleting it is always safe.

| Variable | Effect |
|---|---|
| `BRO_CODE_CACHE=0` | no cache |
| `BRO_CODE_CACHE_DIR=<dir>` | another directory |
| `BRO_CODE_CACHE_MAX_MB=<n>` | the trim limit |

## Vulkan pipeline cache

`<user cache dir>/pipeline-cache/`, one file per GPU and driver, dropped past 128 MB. `BRO_PIPELINE_CACHE=0` keeps it in memory only, `BRO_PIPELINE_CACHE_DIR=<dir>` moves it. Deleting it is always safe.
