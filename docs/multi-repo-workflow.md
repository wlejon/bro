# Multi-Repo Workflow: bro + sibling libraries

bro depends on fifteen sibling repos with submodule fallbacks under `third_party/`: thirteen libraries linked directly into the engine, **[bronze](https://github.com/wlejon/bronze)** (the JavaScript runtime & AOT compiler), and **[brass](https://github.com/wlejon/brass)** (the backend JIT/AOT compiler required by bronze).

Each has a standalone repo at `../<name>` and a git submodule fallback under `third_party/`. The configure that resolves bronze and brass reports which trees it picked (`bronze: standalone tree (...)` or `bronze: submodule tree (...)`) — a build against the pinned submodule must never be mistaken for a build against the checkout you are editing.

Eleven of the thirteen libraries (every one but bromath and htmlayout) also own their JavaScript binding, a `<name>_api` static library under the sibling's `src/api/`, so those siblings depend on bronze — and through it brass — which they did not before 2026-09-13. See [Sibling JavaScript APIs](#sibling-javascript-apis-name_api) below for what that changes.

A sixteenth sibling repo, **[broworkshop](https://github.com/wlejon/broworkshop)** at `../broworkshop`, is **not** a library or CMake dependency. It's the apps tree (launcher, games, tools, demos, AI) with no submodule fallback; bro just runs it via `bro ../broworkshop` or `bro ../broworkshop/bro.json`. See the [Apps tree](#apps-tree) section below.

| Library / Tool | Standalone repo | Submodule fallback |
|---------|----------------|-------------------|
| **bromath** | `../bromath` | `third_party/bromath` |
| **brokit** | `../brokit` | `third_party/brokit` |
| **htmlayout** | `../htmlayout` | `third_party/htmlayout` |
| **broaudio** | `../broaudio` | `third_party/broaudio` |
| **bromesh** | `../bromesh` | `third_party/bromesh` |
| **broflora** | `../broflora` | `third_party/broflora` |
| **brotensor** | `../brotensor` | `third_party/brotensor` |
| **brogameagent** | `../brogameagent` | `third_party/brogameagent` |
| **brolm** | `../brolm` | `third_party/brolm` |
| **brodiffusion** | `../brodiffusion` | `third_party/brodiffusion` |
| **broimage** | `../broimage` | `third_party/broimage` |
| **brosoundml** | `../brosoundml` | `third_party/brosoundml` |
| **brovisionml** | `../brovisionml` | `third_party/brovisionml` |
| **brass** (compiler backend, required) | `../brass` | `third_party/brass` |
| **bronze** (JS runtime, mandatory) | `../bronze` | `third_party/bronze` |

## Directory Layout

```
D:/projects/
├── bro/                          # main project
│   └── third_party/
│       ├── bromath/              # submodule (CI / fallback)
│       ├── brokit/               # submodule (CI / fallback)
│       ├── htmlayout/            # submodule (CI / fallback)
│       ├── broaudio/             # submodule (CI / fallback)
│       ├── bromesh/              # submodule (CI / fallback)
│       ├── broflora/             # submodule (CI / fallback)
│       ├── brotensor/            # submodule (CI / fallback)
│       ├── brogameagent/         # submodule (CI / fallback)
│       ├── brolm/                # submodule (CI / fallback)
│       ├── brodiffusion/         # submodule (CI / fallback)
│       ├── broimage/             # submodule (CI / fallback)
│       ├── brosoundml/           # submodule (CI / fallback)
│       ├── brovisionml/          # submodule (CI / fallback)
│       ├── brass/                # submodule (CI / fallback; backend for bronze)
│       └── bronze/               # submodule (CI / fallback; JS runtime)
├── bromath/                      # standalone repo (preferred for dev)
├── brokit/                       # standalone repo (preferred for dev)
├── htmlayout/                    # standalone repo (preferred for dev)
├── broaudio/                     # standalone repo (preferred for dev)
├── bromesh/                      # standalone repo (preferred for dev)
├── broflora/                     # standalone repo (preferred for dev)
├── brotensor/                    # standalone repo (preferred for dev)
├── brogameagent/                 # standalone repo (preferred for dev)
├── brolm/                        # standalone repo (preferred for dev)
├── brodiffusion/                 # standalone repo (preferred for dev)
├── broimage/                     # standalone repo (preferred for dev)
├── brosoundml/                   # standalone repo (preferred for dev)
├── brovisionml/                  # standalone repo (preferred for dev)
├── brass/                        # standalone repo (the backend JIT/AOT compiler)
├── bronze/                       # standalone repo (the AOT compiler)
└── broworkshop/                  # apps tree (launcher + games/tools/demos/ai)
```

## How It Works

bro's CMake auto-detects standalone repos at `../<name>`. If found, it builds from there directly, **no submodule copy involved**. This means:

- **Edit once**: only touch files in the standalone repo
- **One build**: `cmake --build build` in bro compiles every sibling from its standalone source
- Submodules are only used when standalone repos aren't present (CI, fresh clones)

The detection pattern in `third_party/CMakeLists.txt`:
```cmake
set(BROKIT_DIR "${CMAKE_SOURCE_DIR}/../brokit" CACHE PATH "...")
if(EXISTS "${BROKIT_DIR}/CMakeLists.txt")
    add_subdirectory("${BROKIT_DIR}" "${CMAKE_BINARY_DIR}/brokit" EXCLUDE_FROM_ALL)
else()
    add_subdirectory(brokit EXCLUDE_FROM_ALL)
endif()
```

The same pattern is used for every sibling in the table above.

Note: bromath is pulled in transitively by several siblings (bromesh, brogameagent, etc.). bro's `third_party/CMakeLists.txt` guards the `add_subdirectory` with `if(NOT TARGET bromath)` so the first loader wins, overriding `BROMATH_DIR` only takes effect if bro is the first to add it.

### Feature gates

Most siblings are added **conditionally**, behind the modular-build flags (see [build-options.md](build-options.md)). `bromath`, `brokit`, `htmlayout`, `broaudio`, and `broimage` are unconditional; the rest are gated:

| Sibling | Gate |
|---------|------|
| bromesh | `BRO_WITH_3D` |
| broflora | `BRO_WITH_FLORA` |
| brogameagent | `BRO_WITH_GAMEAI` |
| brotensor | `BRO_WITH_TENSOR` |
| brolm | `BRO_WITH_LM` |
| brosoundml | `BRO_WITH_SOUNDML` |
| brodiffusion | `BRO_WITH_DIFFUSION` |
| brovisionml | `BRO_WITH_VISION` |
| bronze | Mandatory (always ON; `BRO_WITH_BRONZE=1`) |

With a gate off, the sibling is never added and the features it backs are compiled out. The flags auto-resolve their prerequisites (`_bro_require` in the top-level `CMakeLists.txt`), so e.g. `BRO_WITH_DIFFUSION=ON` forces `BRO_WITH_LM` and `BRO_WITH_TENSOR` on.

### brotensor

**bro is the first loader of brotensor**: `third_party/CMakeLists.txt` adds it early, before broimage. This is deliberate: each sibling's own fallback resolution assumes it is the standalone CMake root, so when nested under bro it looks for `../brotensor` / `third_party/brotensor` relative to the wrong directory and misses the submodule (this is what broke the full-profile nightly configure). Loading it once from bro, with bro's correct paths, makes every sibling's `if(NOT TARGET brotensor)` guard trip instead.

brotensor is the foundation of the ML stack: it owns the unified `brotensor::Tensor` type (one type, runtime `Device` tag), the device-neutral op family, and the full training surface (forward + backward ops, losses, optimizers) that every other ML sibling composes on. Its backend is fixed at the **first** `add_subdirectory()`, which is why bro's top-level `CMakeLists.txt` forwards the GPU choice into the `BROTENSOR_WITH_CUDA` / `_WITH_METAL` cache vars **before** `add_subdirectory(third_party)`. The CPU backend is always built; CUDA and Metal are additive and opt-in via `-DBRO_WITH_TENSOR_CUDA=ON` / `-DBRO_WITH_TENSOR_METAL=ON`. With a GPU backend selected, brotensor publishes the `BROTENSOR_HAS_CUDA` / `_HAS_METAL` / `_HAS_GPU` defines; without one, brotensor still builds CPU-only and `BROTENSOR_HAS_GPU` stays undefined.

bro also forwards `BROIMAGE_WITH_TENSOR` (from `BRO_WITH_TENSOR`), `BROGAMEAGENT_WITH_NN` (from `BRO_WITH_GAMEAI_NN`), and `BROGAMEAGENT_WITH_CUDA` / `_WITH_METAL` (from the tensor backend choice) in the same block. Those are internal plumbing, configure with the `BRO_WITH_*` flags, not the sibling ones.

**brolm** (language/text-model inference, tokenizers, text encoders, and generative text / vision-language / translation models) depends on `bromath` + `brotensor`. Because it also provides the text encoders **brodiffusion** consumes, bro's `third_party/CMakeLists.txt` adds it **after the siblings that have already loaded brotensor/bromath** and **before brodiffusion**. brolm guards both deps with `if(NOT TARGET ...)`, reusing the already-loaded targets.

**brodiffusion** (diffusion / flow-matching generative inference, `bro.diffusion` subsystem) depends on `bromath` + `brotensor` + `brolm`. Its `third_party/CMakeLists.txt` block **must be added after brolm's** (it consumes brolm's text encoders). brodiffusion's CMake guards those deps with `if(NOT TARGET ...)` so it reuses the targets bro already added. Its CPU FP32 path is always built, so whenever `BRO_WITH_DIFFUSION` is on the feature is real regardless of GPU backend; `-DBRO_WITH_TENSOR_CUDA=ON` additionally compiles brodiffusion's fused CUDA kernels.

**broimage** (image decode/encode + composable kernels) is the single home for image work that used to be duplicated across the stack: brokit's `bro.image` kernels, bro's HTML `Image` decode, the model siblings' host-side resize + normalize, and pixel preprocessing for the generative siblings. It depends on `bromath` and, when `BRO_WITH_TENSOR` is on, `brotensor` (the tensor adapter forwards `image_normalize` / `image_u8_to_f32_nhwc_to_nchw` to brotensor when the destination is a GPU `Tensor`), bro forwards that choice as `BROIMAGE_WITH_TENSOR`. bro's `third_party/CMakeLists.txt` adds broimage **before brokit** because brokit's image kernels link against `broimage::broimage`, and **after** brotensor so it reuses that target. broimage also vendors its own `stb_image` static lib (with a `if(NOT TARGET stb_image)` guard); bro's own `stb_image` declaration is guarded to match, so either load order works.

**brosoundml** (audio-ML model inference, speech-to-text, text-to-speech, speaker diarization, neural codec, and the streaming wake / keyword / sensor listening stack) depends on `brotensor` for its audio op family (FFT/STFT, 1D conv, vocoder/codec activations, resampling, AR sampling) and on `brolm` for shared text tokenizers. Its `third_party/CMakeLists.txt` block **must be added after brolm's**: a brosoundml tokenizer target links against a brolm tokenizer target, and brosoundml guards `bromath`/`brotensor` with `if(NOT TARGET ...)` so it reuses the already-loaded targets. It backs bro's audio-ML subsystem (`bro.stt`, `bro.tts`, `bro.wake`, and the rest of the listening stack). Like brodiffusion its CPU path is always built, so with `BRO_WITH_SOUNDML` on the features are real; `-DBRO_WITH_TENSOR_CUDA=ON` additionally compiles its fused CUDA kernels.

**brovisionml** (vision-model inference, segmentation, depth, surface normals, background removal, image backbones, the ControlNet conditioning annotators, and generative image models) depends on `bromath` + `brotensor` + `broimage`. By the time its `third_party/CMakeLists.txt` block is added (after brodiffusion) all three are already targets, and brovisionml guards all three with `if(NOT TARGET ...)`. It backs the `bro.vision` subsystem, image in (ImageBitmap / ImageData) → ImageBitmap + typed-array out. Like brodiffusion its CPU path is always built, so with `BRO_WITH_VISION` on the subsystem is real; it ships its own CUDA kernels gated on `BROTENSOR_WITH_CUDA`, so `-DBRO_WITH_TENSOR_CUDA=ON` compiles them automatically.

## Sibling JavaScript APIs: `<name>_api`

Until 2026-09-13 the JavaScript surface of every sibling (`bro.mesh`, `bro.lm`, `AudioContext`, `bro.tensor`, ...) was hand-written in bro's `src/bronze_host/`. It now lives in the sibling that owns the C++ it wraps, so a change to a library and a change to its JS shape are one commit in one repo, and bro's host layer is only the place that mounts them. brokit had this shape first (`brokit_api`, its polyfills and web globals); the other ten followed it.

| Sibling | Target | Install entry points bro calls | Standalone ctest |
|---|---|---|---|
| brokit | `brokit_api` | `brokit::api::install*` (see `host_brokit.cpp`) | one ctest per `tests/js/*.js` |
| broaudio | `broaudio_api` | `broaudio::api::installAudio()`, `installMic()` | `broaudio_test_api` |
| bromesh | `bromesh_api` | `bromesh::api::installMesh()`, `installRigging()` | `test_mesh_api` |
| broflora | `broflora_api` | `broflora::api::installFlora()` | `broflora_api_test` |
| brotensor | `brotensor_api` | `brotensor::api::installTensor()` | `brotensor_test_api` |
| brogameagent | `brogameagent_api` | `brogameagent::api::installGameAi()` (+ `setNavMeshHooks`) | `brogameagent_test_api` |
| brolm | `brolm_api` | `brolm::api::installLM()` | `brolm_test_api` |
| brodiffusion | `brodiffusion_api` | `brodiffusion::api::installDiffusion()` (also `bro.triposplat`) | `brodiffusion_test_api` |
| broimage | `broimage_api` | `broimage::api::installImage()` | `broimage_test_api` |
| brosoundml | `brosoundml_api` | `brosoundml::api::installSoundML()` (+ path/log hooks, `tickSoundML`, `shutdownSoundML`) | `brosoundml_test_api` |
| brovisionml | `brovisionml_api` | `brovisionml::api::installVision()` | `brovisionml_test_api` |

**What a sibling api is.** `src/api/` in the sibling builds a static library, `<name>_api`, that links the sibling's own library plus `bronze_runtime_shared`. It is a bronze *embed* binding: classes built through the sibling's copy of `HostClass` (`src/api/host_class.{h,cpp}`, `object_builder.h`), natives registered under `__bro_native.<ns>` with `bronze::embed::registerNative`, and for brokit, broflora and brotensor a bronze-compiled JS module (`src/api/js/*.js`, compiled at build time by the `bronze` CLI against `src/api/<name>.globals`, with `bronze_js_stubs_arm64.cpp` standing in on Apple Silicon). brotensor additionally runs its own `brotensor-native-manifest` tool at build time so `tensor.js` compiles against a native manifest, the same cycle-breaking bro's `bro-native-manifest` does. Each `install*()` mounts onto the `bro` root it finds on `globalThis` (or, absent one, registers its own — which is why bro's order below matters) and registers the classes it wants as compiled-app globals.

**The public header is a trampoline.** `include/<name>/api.h` is two lines that include `../../src/api/api.h`, guarded by a named `#ifndef <NAME>_API_H` rather than `#pragma once`. The reason is in each header: every sibling ships this same two-line file, and GCC identifies a `#pragma once` header by content and mtime rather than path, so two siblings checked out in the same second make GCC silently skip the second include — and bro includes all of them from one file (`src/bronze_host/host_sibling_apis.cpp`). broflora is the odd one out: its entry is `include/broflora/api/api.h`, a real declaration under `#pragma once`, not a trampoline. brokit has no `include/` header; bro reaches its `api/api.h` through the target's include directories.

**Siblings now depend on bronze and brass, and carry no fallback for either.** Each sibling's top-level `CMakeLists.txt` has an `if(NOT TARGET bronze_runtime_shared)` block that takes `-DBRONZE_DIR`, then `../bronze`, and is a `FATAL_ERROR` ("bronze not found") otherwise; it forces `BRONZE_BUILD_SHARED_RUNTIME=ON` and `BRONZE_BUILD_TESTS=OFF` before adding bronze `EXCLUDE_FROM_ALL`. Every sibling carries the same block. brass is resolved by bronze itself (`bronze/src/codegen-brass/CMakeLists.txt`: `BRASS_ROOT`, then `<bronze>/../brass`, then `<root>/../brass`, then `<root>/third_party/brass`) and **added with `add_subdirectory`**, so a brass checkout beside bronze is all it takes — nothing is pre-built — and brotensor's own kernel-JIT block then finds the `brass` target already there. **No sibling has a `third_party/bronze` or `third_party/brass` submodule**, so a sibling built standalone needs `../bronze` and `../brass` checked out beside it, and so does its CI. The resolution is unconditional in every sibling but broimage (which gates it under `BROIMAGE_ENABLE_API`), so `-D<NAME>_ENABLE_API=OFF` where that option exists does not remove the requirement.

Under bro the first sibling `third_party/CMakeLists.txt` adds resolves bronze from its own `../bronze` — `D:/projects/bronze` for a standalone checkout, `third_party/bronze` for a submodule, the same two trees bro would pick — and every later guard trips, bro's own in `src/bronze_host/CMakeLists.txt` included, which is what prints the `bronze: standalone tree` / `submodule tree` line for the tree actually in use. brass is added by bro's `third_party/CMakeLists.txt` before any sibling, so bronze's and brotensor's brass blocks never run inside bro.

**Sibling CI: one layout, one action.** A sibling's workflow reproduces the development layout in `$GITHUB_WORKSPACE` — itself under `path: <name>`, each C++ dependency it needs under `path: <dep>` (bromath, brotensor, ...), and bronze + brass laid out by bronze's composite action:

```yaml
- uses: actions/checkout@v7
  with: { path: brotensor }
- uses: wlejon/bronze/.github/actions/checkout-toolchain@main
- run: cmake -S brotensor -B build ...
```

(`bronze/.github/actions/checkout-toolchain/action.yml`; inputs `bronze-ref` / `brass-ref`, default `main`.) The action only checks the two trees out. bronze and brass then compile *inside the sibling's build tree*, with the job's own generator, compiler, CRT and compiler launcher, because the api is bound across bronze's C++ embed boundary, which needs the same compiler and CRT on both sides and cannot be checked at load time; a bronze prebuilt elsewhere (bronze's nightly zips, a `BRASS_ROOT` pointed at a separately built brass) would be a silent ABI hazard rather than a shortcut. The sibling's `<name>_test_api` ctest is then the check that the library works the way bro uses it: a bronze realm, the sibling's `install*()`, its namespace driven from JavaScript. brokit had this shape first; the other ten adopted it on 2026-09-16. Each sibling's CodeQL workflow takes the same checkout and keeps bronze out of the traced build (pre-built ahead of `init`, or only the library target traced) so bronze's findings are not filed under the sibling.

Two things that shape only showed once configure got past bronze, for the record: a test that walks to a fixture from the working directory (`../../third_party/...`) only worked while the build tree sat inside the checkout — CI builds beside it, so tests take the source dir from a compile definition (`BROMESH_SOURCE_DIR`); and anything that has to *run* at build time (brotensor's `brotensor-native-manifest`, which links the CUDA backend) must load on a runner with no GPU driver, which is why brotensor resolves the CUDA driver API through `cudaGetDriverEntryPoint` (`src/cuda/cuda_driver.cpp`) rather than linking `libcuda` — the same property that lets one CUDA build of bro start on a machine without an NVIDIA driver.

**Architecture Support and Apple Silicon.** brass supports native machine code generation for both **x86_64** and **AArch64** (including Linux, macOS on Apple Silicon, and Windows ARM64). The configure variable **`BRASS_HOST_BACKEND`**, a `CACHE INTERNAL` set in brass's `CMakeLists.txt` from the build's target architecture, evaluates to `ON` for `x86_64`, `AMD64`, `aarch64`, and `arm64`, enabling native AOT/JIT code emission and compiling Bronze JS modules directly into machine code objects on both architectures. On architectures where `BRASS_HOST_BACKEND` is not supported or explicitly disabled, no-op stub files (`bronze_js_stubs_arm64.cpp` in siblings, `js_entry_stubs.cpp` in bro) satisfy link-time entry symbols. In addition, `js_entry_stubs.cpp` breaks the build-time cycle in `bro-native-manifest` before `bro_core.o` is generated. Neither bronze nor brass forces the build architecture when nested as a subproject.


**How bro links and installs them.** `src/bronze_host/CMakeLists.txt` links `brokit_api` and `broimage_api` unconditionally and each other `<name>_api` under its feature flag (`BRO_WITH_AUDIO`, `BRO_WITH_3D` for bromesh, `BRO_WITH_FLORA`, `BRO_WITH_GAMEAI`, `BRO_WITH_TENSOR`, `BRO_WITH_LM`, `BRO_WITH_SOUNDML`, `BRO_WITH_DIFFUSION`, `BRO_WITH_VISION`) — a bare name on the link line for a sibling `third_party/` never configured would be taken for a file. The install side is `installSiblingApis` in `src/bronze_host/host_sibling_apis.cpp`, and that function is the **only** call site of any sibling `install*()` in bro. `installBroRoots` (`host_bro_root.cpp`) calls it right after `bro`, `__bro` and `__bro_native` are published and before bro's own natives and `js/bro_core.js`, in a fixed order: audio (+ mic), gameagent, mesh (+ rigging), tensor, lm, soundml, diffusion, vision, flora, then brokit's image kernels and broimage. The installers are not re-entrant — brotensor's native registration `fatal()`s on a second registration of the same path, and the `HostClass`-based ones rebuild every class on each call, so a second `Mesh` breaks `instanceof` against the first — which is why the rule is one call per sibling per realm and not "install where convenient" (commit `7ce9e63e` has the failure that taught it). A Worker realm gets none of them: `installWorkerBroRoot` gives it `bro.net` / `bro.net.sync` only (the Workers section of `src/bronze_host/README.md`).

**Sibling api tests.** Each sibling has `tests/test_*api*.cpp` registered as a ctest (names in the table; they run standalone only, since the siblings gate `tests/` on being the top-level project) that boots a bronze realm, calls the installer, and checks the mount points and shape. Every one carries the same `if(WIN32)` block: a `POST_BUILD` `copy_if_different` of `$<TARGET_FILE:bronze_runtime_shared>` and its import library beside the test executable, and `PATH` set on the test property, because bronze builds the DLL into `BRONZE_SHARED_RUNTIME_DIR` and the PE loader only looks beside the `.exe` (`0xc0000135` and a modal "dll was not found" otherwise). bro's `bro_bronze_stage_runtime` does the same for bro's own executables, but as one custom command all four depend on rather than a `POST_BUILD` on each: four `copy_if_different`s of one file into one directory at the end of a parallel build race (macOS copies with `clonefile()` behind an unlink, and two of the four lost the file the third had just staged).

**What `-Sync` now means.** A sibling api is compiled against bronze's `embed` headers and linked to `bronze_runtime_shared`, and the compiled-JS siblings carry bronze's ABI fingerprint in their objects. A bronze change that moves that surface therefore has to land in bronze **and** in every sibling that binds it — ten `<name>_api` repos plus brokit — before bro's pointers can move, and the pointers have to move together: a bro commit that pins a new bronze against old sibling pointers does not configure in CI, where every sibling is its submodule. That is exactly the case `scripts/repo-status.sh --sync` exists for (one commit recording every stale pointer), and one more reason pointer bumps are batched at the end of a session by the repo owner rather than made per commit.

## Day-to-Day Development

### 1. Edit a library

Edit files only in the standalone repo (e.g. `D:/projects/brokit/src/...`, `D:/projects/bromesh/src/...`, `D:/projects/bronze/src/...`).

### 2. Build and test

```bash
# Build bro (uses standalone repos automatically)
cd D:/projects/bro
cmake --build build --config Debug

# Each sibling has its own build + tests; examples:
cd D:/projects/brokit && cmake --build build --config Debug
./build/tests/Debug/brokit_test.exe tests/js

cd D:/projects/htmlayout && cmake --build build --config Debug
./build/tests/Debug/htmlayout_test.exe

cd D:/projects/broaudio && cmake --build build --config Debug
./build/tests/Debug/broaudio_test.exe

# Note: bromesh tests must be built in Release, meshoptimizer's Debug
# assertions trigger a modal abort() dialog on Windows.
cd D:/projects/bromesh && cmake --build build --config Release
./build/tests/Release/bromesh_test.exe

# The sibling's JS binding has its own ctest (see the <name>_api section):
# it needs ../bronze and ../brass beside the sibling, and on Windows the
# POST_BUILD step stages bronze_runtime_shared.dll beside the test exe.
cd D:/projects/bromesh && ctest --test-dir build -C Release -R test_mesh_api
cd D:/projects/brolm   && ctest --test-dir build -C Release -R brolm_test_api
cd D:/projects/brokit  && ctest --test-dir build -C Release      # one test per tests/js/*.js

# bronze (Release dev preset; brass is the only backend, found via ../brass)
cd D:/projects/bronze
.\dev.cmd cmake --preset dev
.\dev.cmd cmake --build --preset dev
.\dev.cmd ctest --preset dev -LE "threejs|pixi"
```

### 3. Commit the library

```bash
cd D:/projects/brokit
git add src/api/new_api.cpp
git commit -m "Add new API"
```

### 4. Sync submodule pointers — at the end of the session, not per commit

The submodule pointers are what CI and a fresh clone build, so they have to move eventually — but **not with every sibling commit**. Bumping is the repo owner's job, done once at the end of a session before pushing, with `scripts/repo-status.sh --sync` (below), which records every stale pointer in one bro commit. Per-commit bumps produce a bro history of pointer-only commits and, since the [`<name>_api` libraries](#sibling-javascript-apis-name_api) bind bronze, a half-synced set (new bronze, old siblings) that does not configure in CI. `AGENTS.md` at the bro root states the same rule.

The manual shape of a single bump, for reference:

```bash
cd D:/projects/bro/third_party/brokit
git fetch ../../../brokit main
git checkout FETCH_HEAD

cd D:/projects/bro
git add third_party/brokit
git commit -m "Update brokit: add new API"
```

Same shape for `bromath`, `htmlayout`, `broaudio`, `bromesh`, `broflora`, `brotensor`, `brogameagent`, `brolm`, `brodiffusion`, `broimage`, `brosoundml`, `brovisionml`, `brass`, and `bronze`.

## Status, pull, sync across all fifteen repos

`scripts/repo-status.ps1` (Windows) and `scripts/repo-status.sh` (Linux/macOS) are the same tool in two ports. Run either from anywhere; both resolve paths from the script location.

```powershell
pwsh scripts/repo-status.ps1              # working-tree state + submodule-pointer drift
pwsh scripts/repo-status.ps1 -ListFiles   # also list changed files in dirty repos
pwsh scripts/repo-status.ps1 -Pull        # fast-forward everything first, then report
pwsh scripts/repo-status.ps1 -Pull -Sync  # ...and bump bro's stale pointers + commit
pwsh scripts/repo-status.ps1 -Push        # push repos ahead of upstream to their remotes
pwsh scripts/repo-status.ps1 -Sync -Push  # sync submodules, commit in bro, and push all
```

```bash
scripts/repo-status.sh              # -v / --verbose, -p / --pull, -s / --sync, -u / --push
scripts/repo-status.sh --pull
scripts/repo-status.sh --sync --push
```

**`-Pull` / `--pull`** fast-forwards bro, broworkshop, and every sibling onto its upstream before the report, so what you read reflects the remotes rather than whatever you last fetched. Use it after a round of merges lands on GitHub (dependabot, PRs merged from the web) to bring the whole tree forward in one shot. It is deliberately conservative:

- `--ff-only`, so a repo that has diverged from its upstream is reported and skipped, never merged or rebased. Resolve those by hand.
- `-c pull.rebase=false`, because a repo configured to rebase on pull refuses outright when the tree is dirty — even for a pure fast-forward. Forcing the merge backend removes that false failure without ever allowing a real merge.
- `--no-recurse-submodules`, so pulling bro never drags `third_party/<name>` checkouts along. Pointer moves are `-Sync`'s job.
- Detached HEADs and branches with no upstream are reported and skipped.

**`-Sync` / `--sync`** then bumps bro's stale submodule pointers to the standalone HEADs and records them in a single bro commit. It only acts where the standalone is ahead of (or diverged from) the recorded pointer; a sibling whose standalone is *behind* bro is left alone, since that one needs a pull, not a bump. Note the ordering `-Pull -Sync` implies: pull first so the pointers you record are the real remote HEADs.

**`-Push` / `-u, --push`** pushes bro, broworkshop, and every sibling that has local commits ahead of its upstream. When combined with `-Sync` (`-Sync -Push`), it updates and commits the submodule pointers in `bro` first, then pushes both `bro` and all sibling repos in one pass.

## Overriding Paths

```bash
cmake -B build \
    -DBROMATH_DIR=/path/to/bromath \
    -DBROKIT_DIR=/path/to/brokit \
    -DHTMLAYOUT_DIR=/path/to/htmlayout \
    -DBROAUDIO_DIR=/path/to/broaudio \
    -DBROMESH_DIR=/path/to/bromesh \
    -DBROFLORA_DIR=/path/to/broflora \
    -DBROTENSOR_DIR=/path/to/brotensor \
    -DBROGAMEAGENT_DIR=/path/to/brogameagent \
    -DBROLM_DIR=/path/to/brolm \
    -DBRODIFFUSION_DIR=/path/to/brodiffusion \
    -DBROIMAGE_DIR=/path/to/broimage \
    -DBROSOUNDML_DIR=/path/to/brosoundml \
    -DBROVISIONML_DIR=/path/to/brovisionml \
    -DBRASS_ROOT=/path/to/brass \
    -DBRONZE_DIR=/path/to/bronze
```

Setting any `*_DIR` to a nonexistent path forces the submodule fallback:

```bash
cmake -B build -DBROMATH_DIR=none -DBROKIT_DIR=none \
               -DHTMLAYOUT_DIR=none -DBROAUDIO_DIR=none -DBROMESH_DIR=none \
               -DBROFLORA_DIR=none -DBROTENSOR_DIR=none -DBROGAMEAGENT_DIR=none \
               -DBROLM_DIR=none -DBRODIFFUSION_DIR=none -DBROIMAGE_DIR=none \
               -DBROSOUNDML_DIR=none -DBROVISIONML_DIR=none
```

bronze and brass are the exceptions: neither falls through on a bad path. The first sibling to resolve bronze (brotensor or broimage, from `third_party/CMakeLists.txt`) honours an explicit `BRONZE_DIR` and errors with "bronze not found" when it does not exist, and bro's brass block does the same with "brass not found" for a `BRASS_ROOT` (cache variable or environment) holding no brass. Forcing either submodule means pointing at it: `-DBRONZE_DIR=<abs path>/third_party/bronze -DBRASS_ROOT=<abs path>/third_party/brass`. `BRASS_ROOT` is read in exactly one place inside bro, `third_party/CMakeLists.txt`, which runs before anything else names brass and prints `brass: standalone tree (...)` or `brass: submodule tree (...)`; every later brass block (bronze's, the siblings') finds the target already there. A build directory configured before 2026-09-24 still carries a `BRASS_DIR` cache entry, which nothing reads any more.

## Apps tree

Apps live in [broworkshop](https://github.com/wlejon/broworkshop) at `../broworkshop`, a sibling repo, not a CMake dependency. Its layout:

```
broworkshop/
├── bro.json                  # project manifest (default_app, lib, system)
├── lib/                      # shared JS modules, apps load via "/lib/foo.js"
├── launcher/                 # the default app (bro grid launcher)
├── games/                    # blockfall, snake, asteroids, ...
├── tools/                    # synth, mesh-viewer, scene-editor, ...
├── demos/                    # terrain, lighting-demo, flora, ...
└── thumbnails/               # launcher tile images (in launcher/)
```

Run any app three equivalent ways:

```bash
bro ../broworkshop                       # project root → default_app
bro ../broworkshop/bro.json              # explicit project manifest
bro ../broworkshop/games/snake           # specific app
```

The first two read `default_app: "launcher"` from the workshop's `bro.json` and set up `/lib` + `/system` mounts. The third inherits the project root via `BRO_PROJECT_ROOT` only if the parent process exported it (e.g. when launched from the launcher itself).
