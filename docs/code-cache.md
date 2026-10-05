# The code cache: warm launches skip the compiler's front end

Every page script, headless driver script and worker script bro compiles
in-process goes through bronze's on-disk code cache. The first launch of an
app compiles from source as always and stores the result; a later launch of
the same, unchanged app loads it and skips parsing, type inference and
lowering, which are nearly all of a cold compile. For a large app (tats: ~11k
functions) the page compile drops from about 2.3 s to a few hundred
milliseconds.

## What is stored

One file per compiled program: the program's bronze IL, as lowering finished
it, plus the name and a 128-bit digest of every source file its module graph
read and the graph's other dependencies (template-literal `import()` globs,
bare-specifier resolutions). The tiered engine still lowers each function body
to machine IR lazily on its first call, cache or not; machine code is not
cached.

## When it is used

An entry is used only when all of these match what the compile would read now:

- the IL format version, bronze's ABI fingerprint, and the identity of the
  executable (path, size, modification time) — any rebuild of bro or bronze
  starts a new key space;
- the entry script's name and text, its resolution path, the module roots
  (asset mounts), the realm's already-published modules, the host globals and
  natives manifest, the pins file and census path;
- every `BRONZE_*` environment variable (the lowering's switches), except
  `BRONZE_TIMINGS`;
- every source file the graph read still hashes the same, every glob directory
  still lists the same files, and every bare specifier still resolves to the
  same file.

Anything else — no entry, an edited file, a new file under a glob, a
different switch, a truncated or corrupt entry (every entry carries a checksum
and is decoded bounds-checked) — is a miss: the program compiles from source
and the entry is rewritten. Entries are written to a temporary file and
renamed into place, so a crash mid-write leaves no half entry behind.

The execution tier is not part of the key: the IL is the same at every tier,
so `BRO_JIT_TIER` does not invalidate anything.

## Reloads

A reload (F5, the source watcher, `location.reload()`) compiles each of the
page's script units again, and each unit consults the cache separately: a unit
none of whose files changed is a hit. Inside one unit bronze infers types over
the whole module graph at once, so an edit to any file of a unit recompiles
that whole unit from source (and stores it for the next launch). The classic
scripts of a page are one unit, each `<script type="module">` is its own.

## Where it lives, and how big it gets

`<user cache dir>/code-cache/` — `%LOCALAPPDATA%\bro\code-cache` on Windows,
`~/Library/Caches/bro/code-cache` on macOS, `$XDG_CACHE_HOME/bro/code-cache`
(or `~/.cache/bro/code-cache`) elsewhere. A per-user cache directory rather
than beside the app because an app's folder may be read-only (an install, a
zip mount) or a git checkout, and because the cache is rebuildable data that
should neither roam with a Windows profile nor be backed up.

After each store the directory is trimmed to 512 MB by deleting the least
recently used entries (a hit refreshes its entry). A large app's entry is a
few tens of MB.

| Variable | Effect |
|---|---|
| `BRO_CODE_CACHE=0` | no cache at all |
| `BRO_CODE_CACHE_DIR=<dir>` | keep it somewhere else |
| `BRO_CODE_CACHE_MAX_MB=<n>` | the trim limit |

`bro.log` (or stdout headless) says per compile whether it hit or missed and
why: `compiled D:/app/index.html in 212 ms (code cache hit)`. Deleting the
directory is always safe.

## The GPU pipeline cache beside it

`<user cache dir>/pipeline-cache/` holds the other half of a warm launch: the
Vulkan driver's compiled pipelines (scene passes, WebGL programs, the
presenter), one file per GPU and driver, named by the cache UUID the driver
reports. It is checked against that device's header before use, written back at
shutdown when it grew (temporary file + rename), and dropped rather than
written past 128 MB. `BRO_PIPELINE_CACHE=0` keeps it in memory only,
`BRO_PIPELINE_CACHE_DIR=<dir>` moves it; deleting it is always safe.
`src/render/vulkan_pipeline_cache.{h,cpp}`.

## Where the pieces live

- bronze `src/eval/code_cache.{h,cpp}` — key, entry format, validation, trim.
- bronze `src/il/serialize.{h,cpp}` — the IL's binary encoding.
- bronze `src/modules/deps.cpp` — the non-textual dependencies of a load.
- `src/bronze_host/eval_jit.cpp` — `configureCodeCache`, applied to every
  in-process compile; `host_worker.cpp` applies it to workers.
