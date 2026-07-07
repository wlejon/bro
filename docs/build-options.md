# Build options — modular `--with` / `--without` builds

Status: **implemented.** bro is split into a small always-buildable core plus
optional feature groups, so a first build is trivial and extra capabilities can
be added later without a from-scratch rebuild. This document is the reference for
the profiles, the `BRO_WITH_*` flag tiers, and how a module is compiled in or
stubbed. For the day-to-day quickstart, see [BUILDING.md](../BUILDING.md).

All three profiles (`minimal` / `app` / `full`) build, link, and run today.

## Goals

1. **A minimal build is trivial to produce.** `git clone --recursive` → `cmake -B build` →
   `cmake --build build`. No vcpkg, no CUDA toolkit, no hand-built Skia, no
   proprietary SDKs.
2. **Heavy, self-contained subsystems are opt-in**, gated at their one real seam.
3. **Adding a module later is incremental** — flip a flag, reconfigure, relink;
   not a from-scratch rebuild.
4. **The JS API surface stays stable whether or not a module is compiled in** —
   an absent module reports `{ available: false }` (the existing `bro.steam` /
   `bro.gpu` pattern), so apps feature-detect instead of crashing.

Non-goal (for now): runtime-loaded plugin `.so`/`.dll`s that add a module with
*zero* relink. That's a plausible later step but a real architecture change
(must respect the no-mutex rule and the "QuickJS context outlives all handles"
lifetime constraint); it is out of scope here.

## Why this is worth doing (the cost centers)

The current build is one chain — `bro` → `bro_engine` → `bro_js` (one monolithic
static lib) → **every** sibling, linked unconditionally in
`src/js/CMakeLists.txt`. The onboarding friction is concentrated in three places,
each isolated to a small number of edges:

| Friction | Caused by | Isolated to |
|---|---|---|
| **vcpkg** required | `find_package` | `net` (GameNetworkingSockets) + `video` (libvpx/webm/Opus) — *only these two* |
| **CUDA toolkit** required | nvcc | brotensor's GPU backend only (already `BROTENSOR_WITH_CUDA`) |
| **Skia** hand-built | gitignored `lib/`, no prebuilt | core; fixed on a separate track (see [Skia](#skia-orthogonal-but-required)) |

Gate net/video and the AI tower, keep CUDA opt-in, auto-fetch Skia, and a minimal
build needs nothing but the submodules.

Build mass, for scale (sibling source-file counts): the AI tower —
brotensor (362) + brosoundml (171) + brolm (151) + brovisionml (121) +
brodiffusion (119) ≈ **924 files** — is the dominant cost and is exactly what the
default build sheds.

## The key structural finding: brotensor is fully extractable

`brotensor` (the 362-file Tensor library) is the universal backbone of the AI
tower, but its two *non-AI* consumers keep it quarantined, so a non-AI build can
omit the Tensor type entirely:

- **broimage → brotensor** is a single translation unit (`tensor_adapter.cpp`);
  its header only forward-declares `brotensor::Tensor`. The other 11 CPU source
  files never touch it. → gate with `BROIMAGE_WITH_TENSOR` (trivial).
- **brogameagent → brotensor** lives *entirely* in `nn/*` and `learn/*`. The core
  — navmesh, pathfinding, steering, perception, MCTS — has **zero** brotensor
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
> vcpkg, no Jolt, no bromesh/brogameagent, and no AI tower — the renderer and
> service subsystems are gated at their engine seams behind the `BRO_WITH_*`
> flags. `app` (~22 MB) is the default; `full` adds the AI tower.

```bash
cmake -B build                                 # app profile (default) — needs vcpkg for net/video
cmake -B build -DBRO_PROFILE=full              # everything (adds the AI tower)
cmake -B build -DBRO_PROFILE=app -DBRO_WITH_LM=ON   # app + language models (adds brolm+brotensor)
```

## Flags

All default-off flags, when OFF, still install their JS namespace as a stub that
reports `{ available: false }` and throws a clear "built without BRO_WITH_X"
error on use.

### Tier 0 — CORE (always on, no flag)

`util · platform · render · svg · layout · dom · canvas · webgl · engine ·
headless` + Skia · SDL · glad · qjs · qjsbind · brokit · htmlayout ·
**broimage (tensor-free)**. The 37 core `*_bindings.cpp`. A complete
HTML/CSS/JS + Canvas2D + WebGL runtime with working screenshots.

### Tier 1 — feature groups (brotensor-free)

| Flag | Pulls in | `minimal` | `app` | `full` | Notes |
|---|:--|:--:|:--:|:--:|---|
| `BRO_WITH_3D` | bromesh + scene graph + mesh/rigging/terrain/tile/gizmo bindings | off | on | on | 3D node types embed `bromesh` by value |
| `BRO_WITH_PHYSICS` | Jolt | off | on | on | header-isolated behind `physics::PhysicsWorld` |
| `BRO_WITH_AUDIO` | broaudio + audio_inference | off | on | on | self-contained, no vcpkg |
| `BRO_WITH_GAMEAI` | brogameagent **core** (nav/path/steer/MCTS) | off | on | on | brotensor-free |
| `BRO_WITH_FLORA` | broflora | off | on | on | needs `3D` (bromesh) |
| `BRO_WITH_NET` | GameNetworkingSockets | off | on | on | **needs vcpkg**; a runtime without JS network access would be surprising |
| `BRO_WITH_VIDEO` | libvpx/webm/Opus | off | on | on | **needs vcpkg**; `<video>` should work out of the box |
| `BRO_WITH_STEAM` | — (runtime dlopen) | off | on | on | already implemented; the stub template |

### Tier 2 — the AI tower (brotensor is the base)

| Flag | Pulls in | `full` | Requires |
|---|:--|:--:|---|
| `BRO_WITH_TENSOR` | brotensor (CPU); gpu/tensor bindings | on | — |
| `BRO_WITH_TENSOR_CUDA` | brotensor CUDA backend | **off** | `TENSOR` + CUDA toolkit |
| `BRO_WITH_LM` | brolm | on | `TENSOR` |
| `BRO_WITH_DIFFUSION` | brodiffusion | on | `LM` (text encoder) |
| `BRO_WITH_VISION` | brovisionml | on | `TENSOR` |
| `BRO_WITH_SOUNDML` | brosoundml (stt/tts/diar/wake/kws/sense/gesture/rave) | on | `TENSOR` + `LM` (stt tokenizer) + `AUDIO` (mic taps) |
| `BRO_WITH_TRIPOSPLAT` | triposplat pipeline | on | `VISION` + `DIFFUSION` + `3D` |
| `BRO_WITH_GAMEAI_NN` | brogameagent `nn/*` + `learn/*` | on | `GAMEAI` + `TENSOR` |

`BRO_WITH_TENSOR_CUDA` stays **off even in `full`** — it needs the CUDA toolkit
and is the single biggest build-time cost. It maps to the existing
`BROTENSOR_WITH_CUDA` (see the forwarding block in the top-level `CMakeLists.txt`)
and is turned on explicitly, orthogonal to the feature flags.

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
```

(All of the above also imply `TENSOR` transitively via `LM`/direct.)

## How a module is compiled in or stubbed

Per optional cluster, two source files that both define the same
`installXBindings(qjs::Context)` symbol:

- `x_bindings.cpp` — the real implementation (compiled when the flag is ON).
- `x_bindings_stub.cpp` — installs `bro.x = { available: false, ... }` whose
  methods throw `"bro.x unavailable: built without BRO_WITH_X"` (compiled when OFF).

`src/js/CMakeLists.txt` selects which file goes into `bro_js`'s source list, and
wraps the sibling `target_link_libraries` entry in the same `if()`. The
`installXBindings()` call in `engine_init.cpp` stays **unconditional** — the call
site never learns whether the module is real. `third_party/CMakeLists.txt` wraps
each optional sibling's `add_subdirectory` in `if(BRO_WITH_X)`.

This keeps `bro_js` monolithic (no new libraries) while making its heavy `.cpp`s
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
| `diffusion_bindings.cpp` | brodiffusion + brolm | no file work — auto-enable pulls `LM` |
| `stt_bindings.cpp` | brosoundml + brolm | no file work — auto-enable pulls `LM` |
| `ai_nn_bindings.cpp` | brogameagent + brotensor | `BRO_WITH_GAMEAI_NN` |
| `ai_learn_bindings.cpp` | brogameagent + brotensor | `BRO_WITH_GAMEAI_NN` |
| `flora_bindings.cpp` | broflora + bromesh | `BRO_WITH_FLORA` (implies `3D`) |

## Skia — orthogonal but required

Skia is core (dom/canvas/render need it) and is **not** a `BRO_WITH_` flag. It
used to be the worst onboarding step — `third_party/skia/{src,include,lib}` are
gitignored, so a fresh clone had to hand-build Skia (GN + ninja + `git-sync-deps`).

**Now auto-fetched** (`third_party/skia/skia.cmake`): on first configure,
`file(DOWNLOAD)` + SHA-256-verify pulls the headers/source bundle and the Release
library — pinned to Skia `chrome/m147`, so lib and headers always match — from
the repo's GitHub releases, and the Release lib is used for all configs (Debug
included). Prebuilt libs are hosted for Windows x64, Linux x64, and macOS arm64;
Intel macOS and a Windows Debug lib still fall back to the `build_skia_*.sh`
scripts. Set `-DBRO_FETCH_SKIA=OFF` to opt out. Skia is BSD-3-Clause, so
redistributing the prebuilt binaries (with the permissive licenses of its
vendored deps — HarfBuzz, zlib, libpng/jpeg/webp, expat, …) is permitted.

## Adding a module later

With the static-lib structure, `-DBRO_WITH_LM=ON` + rebuild reconfigures,
compiles only brolm (+ brotensor) and `lm_bindings.cpp`, and **relinks**
`bro`/`bro-headless`. As long as the build dir is intact, that is incremental
(minutes, mostly the sibling) — not a from-scratch rebuild. ccache/incremental
compilation covers the rest.

Caveat on presets: `-DBRO_PROFILE` seeds flag defaults on **first** configure via
`option()`; individual `-D` flags (already in the cache) win. Switching profile on
an existing build dir won't move flags already cached — clear the specific
`BRO_WITH_*` cache entries (or reconfigure fresh) to re-baseline.

## Implementation surface (where changes land)

- **Top-level `CMakeLists.txt`** — profile → flag defaults; the auto-enable
  resolve block (next to the existing CUDA forwarding).
- **`third_party/CMakeLists.txt`** — wrap each optional sibling's
  `add_subdirectory` in `if(BRO_WITH_X)`; add `BROIMAGE_WITH_TENSOR` /
  `BROGAMEAGENT_WITH_NN` forwarding.
- **`../broimage/CMakeLists.txt`** — `BROIMAGE_WITH_TENSOR` option (gate the
  brotensor `add_subdirectory` + link + `tensor_adapter.cpp`).
- **`../brogameagent/CMakeLists.txt`** — `BROGAMEAGENT_WITH_NN` option (gate the
  brotensor dep + the `nn/*` `learn/*` source list + NN tools).
- **`src/js/CMakeLists.txt`** — conditional source lists (real vs `_stub.cpp`) and
  conditional sibling links.
- **`src/scene/CMakeLists.txt`** — conditional brogameagent/physics/flora links;
  gate the AI-world files (`agent_binding.*`, `ai_world_ticker.*`) and the
  `scene_graph` AI/physics hooks.
- **New `*_bindings_stub.cpp`** per optional cluster.
- **`engine_init.cpp`** — unchanged call sites (the point of the stub pattern).

## Testing implications

The preset triple (`minimal` / `app` / `full`) is the CI matrix that matters —
three configurations cover the meaningful surface. Individual-flag combinations
are constrained by auto-enable, so the combinatorial space is small; spot-check
`app + one AI flag` to exercise the stub↔real boundary and the auto-enable rules.
