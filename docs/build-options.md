# Build options: modular `--with` / `--without` builds

bro is split into a small always-buildable core plus optional feature groups, so
a first build is trivial and extra capabilities can be added later without a
from-scratch rebuild. All three profiles (`minimal` / `app` / `full`) build,
link, and run. This document is the reference for the profiles, the `BRO_WITH_*`
flag tiers, and how a module is compiled in or stubbed. For the day-to-day
quickstart, see [BUILDING.md](../BUILDING.md).

## Goals

1. **A minimal build is trivial to produce.** `git clone --recursive` → `cmake -B build` →
   `cmake --build build`. No vcpkg, no CUDA toolkit, no hand-built Skia, no
   proprietary SDKs.
2. **Heavy, self-contained subsystems are opt-in**, gated at their one real seam.
3. **Adding a module later is incremental**: flip a flag, reconfigure, relink;
   not a from-scratch rebuild.
4. **The JS API surface stays stable whether or not a module is compiled in**:
   an absent module reports `{ available: false }` (the existing `bro.steam` /
   `bro.gpu` pattern), so apps feature-detect instead of crashing.

Non-goal (for now): runtime-loaded plugin `.so`/`.dll`s that add a module with
*zero* relink. That's a plausible later step but a real architecture change
(must respect the no-mutex rule and the "QuickJS context outlives all handles"
lifetime constraint); it is out of scope here.

## Why this is worth doing (the cost centers)

The current build is one chain: `bro` → `bro_engine` → `bro_js` (one monolithic
static lib) → **every** sibling, linked unconditionally in
`src/js/CMakeLists.txt`. The onboarding friction is concentrated in three places,
each isolated to a small number of edges:

| Friction | Caused by | Isolated to |
|---|---|---|
| **vcpkg** required | `find_package` | `net` (GameNetworkingSockets) + `video` (libvpx/webm/Opus): *only these two* |
| **CUDA toolkit** required | nvcc | brotensor's GPU backend only (already `BROTENSOR_WITH_CUDA`) |
| **Skia** hand-built | gitignored `lib/`, no prebuilt | core; fixed on a separate track (see [Skia](#skia-orthogonal-but-required)) |

Gate net/video and the AI tower, keep CUDA opt-in, auto-fetch Skia, and a minimal
build needs nothing but the submodules.

Build mass, for scale (sibling source-file counts): the AI tower (
brotensor (362) + brosoundml (171) + brolm (151) + brovisionml (121) +
brodiffusion, 119) is roughly **924 files**, the dominant cost, and exactly what the
default build sheds.

## The key structural finding: brotensor is fully extractable

`brotensor` (the 362-file Tensor library) is the universal backbone of the AI
tower, but its two *non-AI* consumers keep it quarantined, so a non-AI build can
omit the Tensor type entirely:

- **broimage → brotensor** is a single translation unit (`tensor_adapter.cpp`);
  its header only forward-declares `brotensor::Tensor`. The other 11 CPU source
  files never touch it. → gate with `BROIMAGE_WITH_TENSOR` (trivial).
- **brogameagent → brotensor** lives *entirely* in `nn/*` and `learn/*`. The core (
  navmesh, pathfinding, steering, perception, MCTS) has **zero** brotensor
  references, already walled off by directory. → gate with `BROGAMEAGENT_WITH_NN`
  (mechanical: move the nn/learn source list + the brotensor link behind the flag).

Result: the default build is **brotensor-free**, and core game-AI (navmesh/
pathfinding) still ships.

## Profiles (presets)

`-DBRO_PROFILE=<name>` seeds the individual flag defaults. Individual
`-DBRO_WITH_*` flags always override the profile.

| Profile | What it is | Needs vcpkg? | Needs CUDA? |
|---|---|---|---|
| `minimal` | HTML/CSS/JS + Canvas2D + WebGL + audio. 2D renderer only. | no | no |
| **`app`** (default) | Full renderer (3D scene graph, physics, audio, core game-AI) + net/video/steam. No AI tower. | yes | no |
| `full` | Everything except the CUDA sub-lever (still opt-in). | yes | no (opt-in) |

> All three profiles build, link, and run. `minimal` (~16 MB) compiles with no
> vcpkg, no Jolt, no bromesh/brogameagent, and no AI tower: the renderer and
> service subsystems are gated at their engine seams behind the `BRO_WITH_*`
> flags. `app` (~22 MB) is the default; `full` adds the AI tower.

```bash
cmake -B build                                 # app profile (default), needs vcpkg for net/video
cmake -B build -DBRO_PROFILE=full              # everything (adds the AI tower)
cmake -B build -DBRO_PROFILE=app -DBRO_WITH_LM=ON   # app + language models (adds brolm+brotensor)
```

## Flags

All default-off flags, when OFF, still install their JS namespace as a stub that
reports `{ available: false }` and throws a clear "built without BRO_WITH_X"
error on use.

### Tier 0: CORE (always on, no flag)

`util · platform · render · svg · layout · dom · canvas · webgl · engine ·
headless` + Skia · SDL · glad · qjs · qjsbind · brokit · htmlayout ·
**broimage (tensor-free)**. The core, always-compiled `*_bindings.cpp`. A complete
HTML/CSS/JS + Canvas2D + WebGL runtime with working screenshots.

### Tier 1: feature groups (brotensor-free)

| Flag | Pulls in | `minimal` | `app` | `full` | Notes |
|---|:--|:--:|:--:|:--:|---|
| `BRO_WITH_3D` | bromesh + scene graph + mesh/rigging/terrain/tile/gizmo bindings | off | on | on | 3D node types embed `bromesh` by value |
| `BRO_WITH_PHYSICS` | Jolt | off | on | on | header-isolated behind `physics::PhysicsWorld` |
| `BRO_WITH_AUDIO` | broaudio + audio_inference | off | on | on | self-contained, no vcpkg |
| `BRO_WITH_GAMEAI` | brogameagent **core** (nav/path/steer/MCTS) | off | on | on | brotensor-free |
| `BRO_WITH_FLORA` | broflora | off | on | on | needs `3D` (bromesh) |
| `BRO_WITH_TEXT_SHAPING` | HarfBuzz + Skia's UAX#9 ICU bidi subset + `modules/skunicode` | **on** | on | on | no vcpkg, no Skia rebuild: compiled from the Skia source bundle. On in *every* profile so there is one text path, not two. Off = 1:1 codepoint→glyph (no ligatures, kerning or Arabic joining). Needs a source bundle carrying the shaping sources. See [third_party/skia/BUNDLE.md](../third_party/skia/BUNDLE.md) |
| `BRO_WITH_WEBP` | libwebp decoder | **on** | on | on | no vcpkg, no Skia rebuild: compiled from the Skia source bundle alongside HarfBuzz. On in *every* profile for the same reason shaping is: the pinned pre-built Skia has no libwebp while a hand-built Linux/macOS one does, so leaving it to Skia makes `.webp` work on one platform and fail on another. Off = `.webp` does not decode anywhere |
| `BRO_WITH_NET` | GameNetworkingSockets | off | on | on | **needs vcpkg**; a runtime without JS network access would be surprising |
| `BRO_WITH_VIDEO` | libvpx/webm/Opus | off | on | on | **needs vcpkg**; `<video>` should work out of the box |
| `BRO_WITH_STEAM` | none (runtime dlopen) | off | on | on | already implemented; the stub template |

### Tier 2: the AI tower (brotensor is the base)

| Flag | Pulls in | `full` | Requires |
|---|:--|:--:|---|
| `BRO_WITH_TENSOR` | brotensor (CPU); gpu/tensor bindings | on | none |
| `BRO_WITH_TENSOR_CUDA` | brotensor CUDA backend | **off** | `TENSOR` + CUDA toolkit |
| `BRO_WITH_TENSOR_METAL` | brotensor Metal backend | **off** | `TENSOR` + macOS |
| `BRO_WITH_LM` | brolm | on | `TENSOR` |
| `BRO_WITH_DIFFUSION` | brodiffusion | on | `LM` (text encoder) |
| `BRO_WITH_VISION` | brovisionml | on | `TENSOR` |
| `BRO_WITH_SOUNDML` | brosoundml (stt/tts/diar/wake/kws/sense/gesture/rave) | on | `TENSOR` + `LM` (stt tokenizer) + `AUDIO` (mic taps) |
| `BRO_WITH_TRIPOSPLAT` | triposplat pipeline | on | `VISION` + `DIFFUSION` + `3D` |
| `BRO_WITH_GAMEAI_NN` | brogameagent `nn/*` + `learn/*` | on | `GAMEAI` + `TENSOR` |

`BRO_WITH_TENSOR_CUDA` stays **off even in `full`**. It needs the CUDA toolkit
and is the single biggest build-time cost. It maps to the existing
`BROTENSOR_WITH_CUDA` (see the forwarding block in the top-level `CMakeLists.txt`)
and is turned on explicitly, orthogonal to the feature flags.

### Tier 3: outside the profiles

These three are not seeded by `BRO_PROFILE`; each answers a question the
feature flags do not.

| Flag | Default | What it does |
|---|:--:|---|
| `BRO_BUILD_EXECUTABLES` | ON top-level, **OFF** under `add_subdirectory` | Builds `bro` / `bro-headless` / `bro-server`. An embedder linking `bro_engine` with its own `main` wants the libraries, not a second `bro.exe` in its tree, and gets that without asking. See [embedding.md](embedding.md). |
| `BRO_WITH_BRONZE` | **OFF** | The host layer for bronze-compiled (AOT) JavaScript: `src/bronze_host` re-exposes the engine's DOM, WebGL2, audio, physics and AI as bronze host globals, so an app compiled to machine code runs on the stock binaries. Entirely outside the default configure path — turning it ON resolves a bronze checkout, `../bronze` first and the `third_party/bronze` submodule second, and builds its shared runtime into this tree. The configure prints which of the two it chose. `-DBRONZE_WITH_LLVM=ON` (the default for a fresh cache) additionally builds the `bronze` compiler itself, as an `EXCLUDE_FROM_ALL` target: `cmake --build build --target bronze`. `src/bronze_host/README.md` is the reference. |
| `BRO_QUICKJS_CLANG_CL` | ON when the VS ClangCL toolset is installed (Windows only) | Builds **only** `qjs.lib` with clang-cl, as a nested ExternalProject. MSVC optimises QuickJS's interpreter loop badly; clang-cl is worth ~1.3-1.5x on real-world JS for no source change. Configuring all of bro with `-T ClangCL` does not work (SDL3 fails on a PCH C-standard mismatch), which is why it is scoped to one library. A CI image without the "C++ Clang tools for Windows" component silently gets the slower interpreter. |

An app built for `BRO_WITH_BRONZE` is a **folder** carrying
`app.dll`/`app.so`/`app.dylib` beside its `index.html`; nothing in bro's build
knows the app exists. `tests/bronze_host/run_checks.sh` runs the checks —
`tests/run_tests.sh` discovers them with the rest of the suite, and they skip
(exit 77, reported as SKIP) when the tree has no bronze CLI to compile a
module with.

## Dependency auto-enable

Enabling a flag force-enables its prerequisites (with a status message), so an
inconsistent combo like `DIFFUSION` without `LM` is impossible. This mirrors the
CUDA-forwarding block already in the top-level `CMakeLists.txt`. Resolution runs
once, after the profile is applied and before `add_subdirectory(third_party)`:

```
TRIPOSPLAT → VISION, DIFFUSION, 3D
DIFFUSION  → LM
SOUNDML    → LM, AUDIO
LM         → TENSOR
VISION     → TENSOR
GAMEAI_NN  → GAMEAI, TENSOR
FLORA      → 3D
3D         → PHYSICS, GAMEAI
```

`3D → PHYSICS, GAMEAI` is not a convenience: `scene_graph.h` includes
`physics_node.h` / `agent_binding.h` / `ai_world_ticker.h` and `bro_scene` hard-links
`bro_physics` + `brogameagent`, so the scene graph cannot be built without them.
It resolves after `FLORA → 3D` so the whole `FLORA → 3D → {PHYSICS, GAMEAI}`
chain settles in one pass.

(All of the above also imply `TENSOR` transitively via `LM`/direct.)

## How a module is compiled in or stubbed

There is **one** `x_bindings.cpp` per cluster, always in `bro_js`'s source list.
Its body is wrapped in `#if BRO_WITH_X`, and the `#else` branch installs the
stub via the shared helper in `src/js/feature_stub.h`:

```cpp
#if BRO_WITH_LM
void installLmBindings(qjs::Context& ctx) { /* the real thing */ }
#else
void installLmBindings(qjs::Context& ctx) {
    installUnavailableNamespace(ctx, "lm", "BRO_WITH_LM");
}
#endif
```

`installUnavailableNamespace` installs `bro.x` as a **Proxy** over
`{available: false}`: any property other than `available` resolves to a function
that throws `"bro.x is unavailable: this build was compiled without BRO_WITH_X"`.

> Feature-detect with `bro.x.available === false`, **not** `if (bro.x)`. The
> namespace object always exists and is always truthy, and so is every method
> reached through it; the throw happens on call, not on lookup.

`src/js/CMakeLists.txt` doesn't swap sources; it only wraps each optional
sibling's `target_link_libraries` entry in the matching `if()`. The
`installXBindings()` call in `engine_init.cpp` stays **unconditional**: the call
site never learns whether the module is real. `third_party/CMakeLists.txt` wraps
each optional sibling's `add_subdirectory` in `if(BRO_WITH_X)`.

This keeps `bro_js` monolithic (no new libraries) while making its heavy code
and sibling links conditional, and keeps `engine_init.cpp` clean.

## Seam files that need special handling

The binding→sibling comb found 9 files that straddle clusters. Each has a
contained fix:

| File | Straddles | Handling |
|---|---|---|
| `scene_bindings.cpp` | bromesh + Jolt | belongs to `3D`; physics-specific code behind `#if BRO_WITH_PHYSICS` |
| `triposplat_bindings.cpp` | vision + diffusion + tensor + image | its own flag `BRO_WITH_TRIPOSPLAT` |
| `message_serializer.cpp` | bromesh (worker plumbing) | mesh-serialization branch behind `#if BRO_WITH_3D`; forward-decl otherwise |
| `tile_bindings.cpp` | broimage (core) + brogameagent nav_grid | tile is `3D`; the nav_grid branch behind `#if BRO_WITH_GAMEAI` |
| `diffusion_bindings.cpp` | brodiffusion + brolm | no file work: auto-enable pulls `LM` |
| `stt_bindings.cpp` | brosoundml + brolm | no file work: auto-enable pulls `LM` |
| `ai_nn_bindings.cpp` | brogameagent + brotensor | `BRO_WITH_GAMEAI_NN` |
| `ai_learn_bindings.cpp` | brogameagent + brotensor | `BRO_WITH_GAMEAI_NN` |
| `flora_bindings.cpp` | broflora + bromesh | `BRO_WITH_FLORA` (implies `3D`) |

## Skia: orthogonal but required

Skia is core (dom/canvas/render need it) and is **not** a `BRO_WITH_` flag. It
used to be the worst onboarding step. `third_party/skia/{src,include,lib}` are
gitignored, so a fresh clone had to hand-build Skia (GN + ninja + `git-sync-deps`).

**Now auto-fetched** (`third_party/skia/skia.cmake`): on first configure,
`file(DOWNLOAD)` + SHA-256-verify pulls the headers/source bundle and the Release
library, pinned to Skia `chrome/m147`, so lib and headers always match, from
the repo's GitHub releases, and the Release lib is used for all configs (Debug
included). Prebuilt libs are hosted for Windows x64, Linux x64, and macOS arm64;
Intel macOS and a Windows Debug lib still fall back to the `build_skia_*.sh`
scripts. Set `-DBRO_FETCH_SKIA=OFF` to opt out. Skia is BSD-3-Clause, so
redistributing the prebuilt binaries (with the permissive licenses of its
vendored deps, HarfBuzz, zlib, libpng/jpeg/webp, expat, …) is permitted.

## Adding a module later

With the static-lib structure, `-DBRO_WITH_LM=ON` + rebuild reconfigures,
compiles only brolm (+ brotensor) and `lm_bindings.cpp`, and **relinks**
`bro`/`bro-headless`. As long as the build dir is intact, that is incremental
(minutes, mostly the sibling), not a from-scratch rebuild. ccache/incremental
compilation covers the rest.

Caveat on presets: `-DBRO_PROFILE` seeds flag defaults on **first** configure via
`option()`; individual `-D` flags (already in the cache) win. Switching profile on
an existing build dir won't move flags already cached. Clear the specific
`BRO_WITH_*` cache entries (or reconfigure fresh) to re-baseline.

## Implementation surface (where changes land)

- **Top-level `CMakeLists.txt`**: profile → flag defaults; the auto-enable
  resolve block (next to the existing CUDA forwarding).
- **`third_party/CMakeLists.txt`**: wrap each optional sibling's
  `add_subdirectory` in `if(BRO_WITH_X)`; add `BROIMAGE_WITH_TENSOR` /
  `BROGAMEAGENT_WITH_NN` forwarding.
- **`../broimage/CMakeLists.txt`**: `BROIMAGE_WITH_TENSOR` option (gate the
  brotensor `add_subdirectory` + link + `tensor_adapter.cpp`).
- **`../brogameagent/CMakeLists.txt`**: `BROGAMEAGENT_WITH_NN` option (gate the
  brotensor dep + the `nn/*` `learn/*` source list + NN tools).
- **`src/js/CMakeLists.txt`**: conditional sibling links (the source list is
  unconditional; the gating lives in `#if BRO_WITH_X` inside each
  `*_bindings.cpp`).
- **`src/scene/CMakeLists.txt`**: conditional brogameagent/physics/flora links;
  gate the AI-world files (`agent_binding.*`, `ai_world_ticker.*`) and the
  `scene_graph` AI/physics hooks.
- **`src/js/feature_stub.h` + `feature_stubs.cpp`**: the one shared
  `installUnavailableNamespace` helper every compiled-out cluster falls back to.
- **`engine_init.cpp`**: unchanged call sites (the point of the stub pattern).

## Testing implications

The preset triple (`minimal` / `app` / `full`) is the CI matrix that matters:
three configurations cover the meaningful surface. Individual-flag combinations
are constrained by auto-enable, so the combinatorial space is small; spot-check
`app + one AI flag` to exercise the stub↔real boundary and the auto-enable rules.
