# What bro needs from bronze

Written from the bro side. Updated 2026-08-17, against bronze `8984e1b`.

## Landed, and what bro does with it

The folder load model works end to end: `bronze build app.js -o <appdir>/app.dll
--emit-shared --host-globals src/bronze_host/web_host.globals`, and the stock
`bro` / `bro-headless` load it. Every check in `tests/bronze_host/` runs on the
stock binary now, and the seven per-app `bro-bronze-host*` executables and the
CMake surface that enumerated them are deleted.

Four things bro relies on, so they are worth knowing are load-bearing:

- **`bronze_runtime_shared`** — bro links this and *never* `bronze::embed` +
  `bronze::runtime`. The static archives would give the process a second heap
  and a second key registry, and the failure would not be a link error but a
  value collected out from under the module.
- **`embed::runEntry`** — bro cannot open-code the root frame + microtask drain
  the way it did against the static runtime: `ShadowStackFrame` and
  `rtDrainMicrotasks` are runtime-internal and not on the export list. The same
  export list caught the one place in 7.7K lines of bro's binding layer that
  reached past the embed API (`ArrayHeader::getElem`), which is the C-boundary
  discipline in `embed.h` doing exactly what it says.
- **`embed::abiFingerprint()`** — compared against the module's stamp instead of
  the `BRONZE_ABI_FINGERPRINT` macro. With a shared runtime the macro is what
  *bro* was compiled against; the library answering the module's calls is the
  one that has to match.
- **`<entry>_host_globals`** — read and logged on every load. Emitting it
  unconditionally with count 0 is the detail that makes it usable: absence means
  "not a bronze module", never "no globals".

## Outstanding

### 1. Compile time and memory on some inputs (the pressing one)

Two probes in `tests/bronze_host/apps/`, both small, compile pathologically:

| probe | source | compile | peak RSS | module |
|---|---|---|---|---|
| `dom_probe.js` | 16 KB | ~3 s | small | 259 KB |
| `wild_orbit_probe.js` | 5 KB | minutes | — | 28 MB |
| `instanced_mesh_probe.js` | 5 KB | **~12 min** | **2.9 GB** | 27 MB |
| `pixi_sprites_probe.js` | 19 KB | **25 min+** | **4.6 GB** | — |

`instanced` is the clean reproducer because `wild` is a controlled comparison:
same 5 KB, same three.js, and both resolve `three` to the *identical*
`bronze/tests/oracle/threejs/three.module.js` (bro's `node_modules/three` is a
one-line package.json pointing at it). So it is not module-graph size. Something
about those probes' shape — `InstancedMesh` with `setMatrixAt`/`setColorAt` over
2,500 instances in one, pixi's batcher in the other — goes superlinear.

This sets the floor on an app author's edit-run loop, and 4.6 GB is close enough
to a memory ceiling that a bigger app may simply not compile.

### 2. A property trap, for the objects whose keys are not known in advance

The embed API can define a named property and a named accessor. It cannot
define what happens when a key nobody registered is read or written. Three
things on the web are exactly that shape, and all three are stuck:

- **`el.dataset`** — a live view of the element's `data-*` attributes. Reads and
  writes of keys that already exist can be faked by rebuilding the object on
  each read; `el.dataset.newKey = 'v'` cannot be faked at all, and a dataset
  that silently drops that write is worse than no dataset.
- **`el.style` and `getComputedStyle`** — today an accessor per property from a
  curated ~110-name list, because htmlayout has 363 and an accessor PAIR per
  property per element is paid by every element an app makes. A trap would make
  this both complete and free.
- **`localStorage`** — same shape, for the same reason.

What would close it: a `makeProxyHandle`-style call taking get/set/has/delete
callbacks, or narrower, a "dynamic property" hook on an existing handle object
consulted only when the ordinary lookup misses. The narrow one is enough for all
three, and it costs nothing on objects that do not declare it.

**This shrank a lot in bronze `9e40adb`.** The mechanism now exists: a real
`Proxy` with the 10.5 essential invariants, whose checks read the target and
never call a trap — so a proxy over an empty EXTENSIBLE object constrains
nothing and may answer every trap from an element's attributes, the
`dataset.newKey = 'v'` write included. What is left is purely reach. The embed
API has `registerGlobal` to WRITE a global and nothing to read one, and no
`construct`, so a host can hand `Proxy` to an app and still not build one
itself. Either of these closes it:

- `makeProxy(Value target, traps)`, direct and typed for this use; or
- `globalValue(std::string_view name)` + `construct(Value fn, args)`, general,
  and useful well beyond this — every "call the program's own constructor" a
  host might want is the same two calls.

The second is the smaller surface for the larger gain, and neither needs new
runtime machinery.

### 3. A host cannot build a function that carries statics

`URL` on the web is callable AND a namespace: `new URL(href)` and
`URL.createObjectURL(blob)` are the same object. So are `Promise.resolve`,
`Array.from`, `Object.assign` — the shape is everywhere. A bro host can build
neither half onto the other:

- `embed::setProperty` calls `requirePlainObject`, which on a function calls
  **`fatal()`** — the process aborts, so a host cannot even attempt it and
  recover.
- The obvious way around it, the program's own `Object.assign` reached from a
  throwaway object's `constructor`, is also a hard runtime error:

      unsupported: Object.assign on a function (its own keys come from three
      places — a `prototype` slot, a `length` and a `name` in the header, and a
      side object of statics — and only the last is a shape a descriptor could
      be written to)

  A hard error rather than a throw means there is no probe-and-degrade either:
  a host that tries it at startup takes the process down.

That message names the mechanism that would work — the side object of statics.
A `embed::setStatic(Value fn, std::string_view key, Value v)` writing into it
would close this completely, and it is the same call the runtime must already
make when compiled code writes `f.x = 1`.

`bro` currently ships `URL` as a plain namespace with `createObjectURL`,
`revokeObjectURL` and `parse` — `URL.parse` being the standard 2024 addition
that does the constructor's job and answers null instead of throwing. That is a
good outcome for URL specifically and not a general one: the next namespace with
this shape may not have a `parse` to fall back on.

`AbortSignal` is the second instance and cost nothing, because `new
AbortSignal()` is a TypeError on the web too — so a namespace object carrying
`abort`, `timeout` and `any` is the whole interface. Two for two on a lucky
escape is not a pattern to plan around; `Promise.resolve`, `Array.from` and
`Object.assign` are all the same shape and none of them has a way out.

### 4. The shared-runtime search misses multi-config layouts

`src/cli/link.cpp` looks for the import library in `shared/` beside the CLI and
one or two directories above it. Under a multi-config generator (MSBuild, Xcode)
CMake writes it one level deeper, in `shared/<Config>/`, so every invocation on
Windows needs `BRONZE_SHARED_RT_LIB` set by hand. Adding a `<config>` level to
the existing candidate list would fix the common case; the env override already
works, so this is friction rather than a blocker.

### 5. `bronze build --help` reads `--help` as a filename

Reports `cannot read --help`. Cosmetic.

### 6. Still open from the original list

- **A host-globals *lookup* in the embed API.** `rtHostGlobalLookup` exists in
  `runtime/host_globals.h` but is neither in the C ABI registry nor annotated
  `BRONZE_EMBED_API`, so it is not exported and bro cannot call it. Without it
  bro can only *report* `<entry>_host_globals`, not enforce it — enforcing means
  diffing against what bro registered, and those go in from a dozen call sites
  with no list to consult. Either an exported lookup or bro recording its own
  registrations would close it; the exported lookup is smaller and checks the
  thing that actually matters (what the runtime holds, not what bro intended).
- **`embed::setElement` filling arrays**, and the typed-array constructors.
  `createTypedArray`/`fillTypedArray` are now IN USE — `blob.bytes()` builds its
  Uint8Array with them (host_file.cpp) and they work exactly as documented.
  `TextEncoder` / `ImageData` / `ImageBitmap` are still unbuilt, but the
  primitive they were waiting on is proven.

### 7. A finalizer that may free a whole DOM subtree

Not a bronze bug — the missing piece is in bro — but it is what decides a
bronze-visible policy, so it is recorded here beside the rest.

`DOMParser` makes a second `dom::Document` per call (`host_parser.cpp`) and
this layer never frees any of them, so an app that parses a fragment every
frame grows without bound. The natural fix is a handle finalizer that deletes
the document when its wrapper is collected — a finalizer MAY free plain C++
memory, and a `dom::Document` is plain C++ memory. Two things stop it:

- `~Document` fires only `nodeDestroyingCb_`, the single callback slot the
  QuickJS realm owns, and only for elements. The freed-node observer *list* —
  documented in `dom/document.h` as existing precisely for a wrapper layer that
  is not the JS realm — is never fired from `~Document`, so every registry entry
  for a node of that document would be left pointing into released storage.
  This one is a small bro fix and is the reason the policy is a leak rather
  than a crash.
- A node wrapper outliving its document wrapper is ordinary code, and the web
  keeps the document alive through the node. This layer cannot: registry
  entries are never freed, so a node rooting its document would pin it forever
  regardless. Fixing it properly wants a registry entry that CAN be released,
  which wants a finalizer that can make embed calls — the same GC-rule wall
  everything else in this layer is shaped around.

Until both move, never-freeing is the choice that is only expensive rather
than also wrong.

## Fixed during integration

`cmake/bronze_shared_runtime.cmake` used `CMAKE_SOURCE_DIR` for the ABI header
it scans and for its include path. That is the *top-level* project — bronze when
bronze builds itself, **bro** when bro embeds it — so bro's configure died
looking for `D:/projects/bro/src/abi/bronze_abi.h`. Fixed in bronze `8984e1b`.
Nothing in bronze's own CI can catch this class of bug, because nothing there
builds bronze as a subdirectory; bro's configure is now that coverage.
