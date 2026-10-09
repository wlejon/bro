# Multi-Repo Workflow: bro + sibling libraries

This page is bro's side of working across repositories. [ecosystem.md](ecosystem.md) is the index of every repo in the family (including the desktop-environment libraries and apps that bro does not build against) and of the dependency convention they share; `scripts/repos.txt` is the same list for tooling.

bro depends on sibling repos: the libraries linked directly into the engine (the terminal ones, bropty, brosearch, brothemes and bromux, included), **[bronze](https://github.com/wlejon/bronze)** (the JavaScript compiler and runtime), and **[brass](https://github.com/wlejon/brass)** (the code-generation backend bronze requires). There are **no git submodules** anywhere in the ecosystem; a plain `git clone` of any repo is its whole checkout.

Each dependency is a `bro_dependency()` call (see [How it works](#how-it-works)): it builds from the working tree at `../<name>` when there is one, and otherwise downloads the commit bro pins in `cmake/bro_pins.cmake`. The configure prints which it took for every dependency (`bronze: working tree D:/projects/bronze` or `bronze: https://github.com/wlejon/bronze/archive/<sha>.tar.gz`) — a build against the pin must never be mistaken for a build against the checkout you are editing.

Every library but bromath, htmlayout and the four terminal libraries also owns its JavaScript binding, a `<name>_api` static library under the sibling's `src/api/`, so those siblings depend on bronze — and through it brass. See [Sibling JavaScript APIs](#sibling-javascript-apis-name_api) below for what that changes.

One more sibling repo, **[broworkshop](https://github.com/wlejon/broworkshop)** at `../broworkshop`, is **not** a library or CMake dependency. It's the apps tree (launcher, games, tools, demos, AI) and no CMake dependency at all; bro just runs it via `bro ../broworkshop` or `bro ../broworkshop/bro.json`. See the [Apps tree](#apps-tree) section below. The dependency runs the other way for **[broterm](https://github.com/wlejon/broterm)**, **[helm](https://github.com/wlejon/helm)** and **[ffmpeg-bro](https://github.com/wlejon/ffmpeg-bro)**: each is its own executable that pins bro with `bro_dependency(bro ...)`, so it builds `../bro` when present and the pinned bro otherwise.

`cmake/bro_pins.cmake` pins every ecosystem library bro builds against (all of `scripts/repos.txt`'s `pinned` rows: the engine, terminal and desktop libraries, brolink/brovideo/broremote/brodmabuf, bronze and brass), plus bro's own third-party code: SDL, Jolt and FastNoise2.

## Directory Layout

```
D:/projects/
├── bro/                          # main project
│   ├── cmake/bro_deps.cmake      # bro_dependency(), identical in every repo
│   ├── cmake/bro_pins.cmake      # every pin bro builds against
│   └── build/_deps/<name>-src/   # downloaded pins (only for what has no ../<name>)
├── bromath/ brokit/ htmlayout/   # sibling working trees (preferred for dev)
├── broaudio/ bromesh/ broflora/
├── brotensor/ brogameagent/ brolm/ brodiffusion/ broimage/ brosoundml/ brovisionml/
├── bropty/ brosearch/ brothemes/ bromux/ brolink/
├── brass/                        # the backend JIT/AOT compiler
├── bronze/                       # the JS compiler + runtime
├── broworkshop/                  # apps tree (launcher + games/tools/demos/ai)
└── ...                           # desktop libraries, broterm, helm, ...: see ecosystem.md
```

Any of the sibling directories may be missing; that dependency then comes from its pin.

## How It Works

Every repo that has dependencies carries the same file, `cmake/bro_deps.cmake`, byte for byte (copy bro's when it changes). It defines one function:

```cmake
bro_dependency(<name> [GITHUB <owner/repo> REF <sha>] [TARGET <target>]
               [THIRD_PARTY] [PIN_ONLY | SOURCE_ONLY] [OPTIONS <VAR>=<value> ...])
```

which resolves `<name>` in this order:

1. **An existing target** (`TARGET`, default `<name>`): someone already added it, so nothing happens. This is how a dependency shared by several siblings is built once.
2. **A working tree**: `FETCHCONTENT_SOURCE_DIR_<NAME>` if set (`-D` on the command line), else `../<name>` beside the top-level source dir (then beside the current project's). `THIRD_PARTY` dependencies skip the `../` lookup — a stray `../SDL` is not taken for the pin.
3. **The pin**: FetchContent of `https://github.com/<owner/repo>/archive/<sha>.tar.gz` (a tarball, not a clone) into `<build>/_deps/<name>-src`, added with `add_subdirectory(... EXCLUDE_FROM_ALL)`.

`OPTIONS` become cache entries before the add (each sibling's tests/tools off, and so on). `PIN_ONLY` only records the pin; `SOURCE_ONLY` resolves and populates the source (`<name>_SOURCE_DIR`) without adding it, for a dependency whose CMake entry point is not its root (Jolt's `Build/`) or that needs work done between populate and add.

**Pins are first-declaration-wins.** A pin is a global property; a second `bro_dependency(<name> GITHUB ... REF ...)` for the same name keeps the first one's sha. bro includes `cmake/bro_pins.cmake` right after `project()`, before any sibling is added, so a sibling's own pins never override bro's, and every sibling's dependency on (say) bromath resolves to the one commit bro pins. A sibling built standalone has nobody above it and uses its own pins.

This means:

- **Edit once**: only touch files in the sibling's working tree at `../<name>`; bro builds it from there.
- **One build**: `cmake --build build` in bro compiles every sibling from its working tree or pin.
- **Pins only matter without a working tree**: CI, fresh clones, a machine with only bro checked out.
- **Never edit `_deps/<name>-src`**: it is a download of the pinned commit, not a checkout.

A pin line keeps `name`, `GITHUB` and `REF` on one line, `bro_dependency(<name> GITHUB <owner/repo> REF <40-hex sha> ...)`, because `scripts/bump-deps.sh` and the repo-status scripts read and rewrite it with a line-oriented match.

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
| bropty, brosearch, brothemes | `BRO_WITH_TERMINAL` |
| bromux | `BRO_WITH_TERMINAL` (persistent sessions); it needs brolink |
| brovideo, broremote | `BRO_WITH_REMOTE` (Windows and Linux; off by default on macOS); off, `bro.remote` is the unavailable stub. broremote needs brolink; brovideo's VA-API encoder on Linux needs brodmabuf (`BRO_WITH_DMABUF`) |
| brolink | `BRO_WITH_TERMINAL` or `BRO_WITH_REMOTE`: bro adds it once, before bromux and broremote, so one copy serves both |
| brodmabuf | `BRO_WITH_DMABUF` (Linux) |
| desktop libraries (brovfs, brosys, brocred, ...) | their `BRO_WITH_<NAME>` flags; see [build-options.md](build-options.md) |
| bronze | Mandatory (always ON; `BRO_WITH_BRONZE=1`) |

With a gate off, the sibling is never added and the features it backs are compiled out. The flags auto-resolve their prerequisites (`_bro_require` in the top-level `CMakeLists.txt`), so e.g. `BRO_WITH_DIFFUSION=ON` forces `BRO_WITH_LM` and `BRO_WITH_TENSOR` on.

### brotensor

**bro is the first loader of brotensor**: `third_party/CMakeLists.txt` adds it early, before broimage, so its backend options (below) are bro's, and every later sibling's `bro_dependency(brotensor ...)` finds the target already there.

brotensor is the foundation of the ML stack: it owns the unified `brotensor::Tensor` type (one type, runtime `Device` tag), the device-neutral op family, and the full training surface (forward + backward ops, losses, optimizers) that every other ML sibling composes on. Its backend is fixed at the **first** `add_subdirectory()`, which is why bro's top-level `CMakeLists.txt` forwards the GPU choice into the `BROTENSOR_WITH_CUDA` / `_WITH_METAL` / `_WITH_VULKAN` cache vars **before** `add_subdirectory(third_party)`. The CPU backend is always built; CUDA and Metal are additive and opt-in via `-DBRO_WITH_TENSOR_CUDA=ON` / `-DBRO_WITH_TENSOR_METAL=ON`. Vulkan (`-DBRO_WITH_TENSOR_VULKAN=ON`, `BROTENSOR_WITH_VULKAN`) is the AMD GPU path (there is no HIP / ROCm backend); it can sit beside CUDA, and switches itself on under Linux when glslc and the Vulkan headers and loader are present. With a GPU backend selected, brotensor publishes the `BROTENSOR_HAS_CUDA` / `_HAS_METAL` / `_HAS_VULKAN` / `_HAS_GPU` defines; without one, brotensor still builds CPU-only and `BROTENSOR_HAS_GPU` stays undefined. bro's own code names the backend it was built for (`brotensor::Device::vulkan` in `host_gpu.cpp`), so a new backend in brotensor and its first use in bro have to reach CI as one pin bump.

bro also forwards `BROIMAGE_WITH_TENSOR` (from `BRO_WITH_TENSOR`), `BROGAMEAGENT_WITH_NN` (from `BRO_WITH_GAMEAI_NN`), and `BROGAMEAGENT_WITH_CUDA` / `_WITH_METAL` / `_WITH_VULKAN` (from the tensor backend choice) in the same block. Those are internal plumbing, configure with the `BRO_WITH_*` flags, not the sibling ones.

**brolm** (language/text-model inference, tokenizers, text encoders, and generative text / vision-language / translation models) depends on `bromath` + `brotensor`. Because it also provides the text encoders **brodiffusion** consumes, bro's `third_party/CMakeLists.txt` adds it **after the siblings that have already loaded brotensor/bromath** and **before brodiffusion**. brolm guards both deps with `if(NOT TARGET ...)`, reusing the already-loaded targets.

**brodiffusion** (diffusion / flow-matching generative inference, `bro.diffusion` subsystem) depends on `bromath` + `brotensor` + `brolm`. Its `third_party/CMakeLists.txt` block **must be added after brolm's** (it consumes brolm's text encoders). brodiffusion's CMake guards those deps with `if(NOT TARGET ...)` so it reuses the targets bro already added. Its CPU FP32 path is always built, so whenever `BRO_WITH_DIFFUSION` is on the feature is real regardless of GPU backend; `-DBRO_WITH_TENSOR_CUDA=ON` additionally compiles brodiffusion's fused CUDA kernels.

**broimage** (image decode/encode + composable kernels) is the single home for image work across the stack: brokit's `bro.image` kernels, bro's HTML `Image` decode, the model siblings' host-side resize + normalize, and pixel preprocessing for the generative siblings. It depends on `bromath` and, when `BRO_WITH_TENSOR` is on, `brotensor` (the tensor adapter forwards `image_normalize` / `image_u8_to_f32_nhwc_to_nchw` to brotensor when the destination is a GPU `Tensor`), bro forwards that choice as `BROIMAGE_WITH_TENSOR`. bro's `third_party/CMakeLists.txt` adds broimage **before brokit** because brokit's image kernels link against `broimage::broimage`, and **after** brotensor so it reuses that target. broimage also vendors its own `stb_image` static lib (with a `if(NOT TARGET stb_image)` guard); bro's own `stb_image` declaration is guarded to match, so either load order works.

**brosoundml** (audio-ML model inference, speech-to-text, text-to-speech, speaker diarization, neural codec, and the streaming wake / keyword / sensor listening stack) depends on `brotensor` for its audio op family (FFT/STFT, 1D conv, vocoder/codec activations, resampling, AR sampling) and on `brolm` for shared text tokenizers. Its `third_party/CMakeLists.txt` block **must be added after brolm's**: a brosoundml tokenizer target links against a brolm tokenizer target, and brosoundml guards `bromath`/`brotensor` with `if(NOT TARGET ...)` so it reuses the already-loaded targets. It backs bro's audio-ML subsystem (`bro.stt`, `bro.tts`, `bro.wake`, and the rest of the listening stack). Like brodiffusion its CPU path is always built, so with `BRO_WITH_SOUNDML` on the features are real; `-DBRO_WITH_TENSOR_CUDA=ON` additionally compiles its fused CUDA kernels.

**brovisionml** (vision-model inference, segmentation, depth, surface normals, background removal, image backbones, the ControlNet conditioning annotators, and generative image models) depends on `bromath` + `brotensor` + `broimage`. By the time its `third_party/CMakeLists.txt` block is added (after brodiffusion) all three are already targets, and brovisionml guards all three with `if(NOT TARGET ...)`. It backs the `bro.vision` subsystem, image in (ImageBitmap / ImageData) → ImageBitmap + typed-array out. Like brodiffusion its CPU path is always built, so with `BRO_WITH_VISION` on the subsystem is real; it ships its own CUDA kernels gated on `BROTENSOR_WITH_CUDA`, so `-DBRO_WITH_TENSOR_CUDA=ON` compiles them automatically.

## Sibling JavaScript APIs: `<name>_api`

The JavaScript surface of every sibling (`bro.mesh`, `bro.lm`, `AudioContext`, `bro.tensor`, ...) lives in the sibling that owns the C++ it wraps, so a change to a library and a change to its JS shape are one commit in one repo, and bro's host layer is only the place that mounts them. brokit's `brokit_api` also carries its polyfills and web globals.

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
| broremote | `broremote_api` (`BROREMOTE_ENABLE_API`, forced on by bro) | `broremote::api::installRemote()` (+ `setHostHooks`, `tickRemote`, `shutdownRemote`; bro's side is `host_remote.cpp`) | `broremote_test_api` |

**What a sibling api is.** `src/api/` in the sibling builds a static library, `<name>_api`, that links the sibling's own library plus `bronze_runtime_shared`. It is a bronze *embed* binding: classes built through the sibling's copy of `HostClass` (`src/api/host_class.{h,cpp}`, `object_builder.h`), natives registered under `__bro_native.<ns>` with `bronze::embed::registerNative`, and for brokit, broflora and brotensor a bronze-compiled JS module (`src/api/js/*.js`, compiled at build time by the `bronze` CLI against `src/api/<name>.globals`, natively on x86_64 and AArch64 alike, Apple Silicon included; `bronze_js_stubs_nobackend.cpp` stands in only where brass has no code generator, see below). brotensor additionally runs its own `brotensor-native-manifest` tool at build time so `tensor.js` compiles against a native manifest, the same cycle-breaking bro's `bro-native-manifest` does. Each `install*()` mounts onto the `bro` root it finds on `globalThis` (or, absent one, registers its own — which is why bro's order below matters) and registers the classes it wants as compiled-app globals.

**The public header is a trampoline.** `include/<name>/api.h` is two lines that include `../../src/api/api.h`, guarded by a named `#ifndef <NAME>_API_H` rather than `#pragma once`. The reason is in each header: every sibling ships this same two-line file, and GCC identifies a `#pragma once` header by content and mtime rather than path, so two siblings checked out in the same second make GCC silently skip the second include — and bro includes all of them from one file (`src/bronze_host/host_sibling_apis.cpp`). broflora is the odd one out: its entry is `include/broflora/api/api.h`, a real declaration under `#pragma once`, not a trampoline. brokit has no `include/` header; bro reaches its `api/api.h` through the target's include directories.

**Siblings depend on bronze and brass like anything else.** Each sibling with a binding pins bronze (`bro_dependency(bronze ... TARGET bronze_runtime_shared OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)`) and brass (`PIN_ONLY`; bronze adds it). bronze resolves brass itself, in `bronze/src/codegen-brass/CMakeLists.txt`: an explicit `BRASS_ROOT` (cache or environment) first, else `bro_dependency(brass)` — `../brass` or bronze's pin — **added with `add_subdirectory`**, so nothing is pre-built and brotensor's kernel-JIT block then finds the `brass` target already there.

Under bro, `third_party/CMakeLists.txt` adds brass and then bronze before any sibling, at bro's pins, so every sibling's bronze/brass `bro_dependency` finds the targets already there and the configure prints one `bronze:` / `brass:` line for the trees actually in use.

**Sibling CI: a lone checkout.** Because every dependency falls back to its pin, a sibling's workflow is just its own checkout and a configure; the pins download at configure time:

```yaml
- uses: actions/checkout@v7
- run: cmake -B build ...
```

bronze and brass compile *inside the sibling's build tree*, with the job's own generator, compiler, CRT and compiler launcher, because the api is bound across bronze's C++ embed boundary, which needs the same compiler and CRT on both sides and cannot be checked at load time; a bronze prebuilt elsewhere (bronze's nightly zips, a `BRASS_ROOT` pointed at a separately built brass) would be a silent ABI hazard rather than a shortcut. The sibling's `<name>_test_api` ctest is then the check that the library works the way bro uses it: a bronze realm, the sibling's `install*()`, its namespace driven from JavaScript. Each sibling's CodeQL workflow takes the same checkout and keeps bronze out of the traced build (pre-built ahead of `init`, or only the library target traced) so bronze's findings are not filed under the sibling.

Two rules follow from that layout. CI builds beside the checkout rather than inside it, so a test never walks to a fixture relative to the working directory (`../../third_party/...`); it takes the source dir from a compile definition (`BROMESH_SOURCE_DIR`). And anything that has to *run* at build time (brotensor's `brotensor-native-manifest`, which links the CUDA backend) must load on a runner with no GPU driver, which is why brotensor resolves the CUDA driver API through `cudaGetDriverEntryPoint` (`src/cuda/cuda_driver.cpp`) rather than linking `libcuda` — the same property that lets one CUDA build of bro start on a machine without an NVIDIA driver.

**Architecture Support and Apple Silicon.** brass supports native machine code generation for both **x86_64** and **AArch64** (including Linux, macOS on Apple Silicon, and Windows ARM64). The configure variable **`BRASS_HOST_BACKEND`**, a `CACHE INTERNAL` set in brass's `CMakeLists.txt` from the build's target architecture, evaluates to `ON` for `x86_64`, `AMD64`, `aarch64`, and `arm64`, enabling native AOT/JIT code emission and compiling Bronze JS modules directly into machine code objects on both architectures. On architectures where `BRASS_HOST_BACKEND` is not supported or explicitly disabled, no-op stub files (`bronze_js_stubs_nobackend.cpp` in brokit, broflora and brotensor, `js_entry_stubs.cpp` in bro) satisfy link-time entry symbols. In addition, `js_entry_stubs.cpp` breaks the build-time cycle in `bro-native-manifest` before `bro_core.o` is generated. Neither bronze nor brass forces the build architecture when nested as a subproject.


**How bro links and installs them.** `src/bronze_host/CMakeLists.txt` links `brokit_api` and `broimage_api` unconditionally and each other `<name>_api` under its feature flag (`BRO_WITH_AUDIO`, `BRO_WITH_3D` for bromesh, `BRO_WITH_FLORA`, `BRO_WITH_GAMEAI`, `BRO_WITH_TENSOR`, `BRO_WITH_LM`, `BRO_WITH_SOUNDML`, `BRO_WITH_DIFFUSION`, `BRO_WITH_VISION`) — a bare name on the link line for a sibling `third_party/` never configured would be taken for a file. The install side is `installSiblingApis` in `src/bronze_host/host_sibling_apis.cpp`, and that function is the **only** call site of any sibling `install*()` in bro. `installBroRoots` (`host_bro_root.cpp`) calls it right after `bro`, `__bro` and `__bro_native` are published and before bro's own natives and `js/bro_core.js`, in a fixed order: audio (+ mic), gameagent, mesh (+ rigging), tensor, lm, soundml, diffusion, vision, flora, then brokit's image kernels and broimage. The installers are not re-entrant — brotensor's native registration `fatal()`s on a second registration of the same path, and the `HostClass`-based ones rebuild every class on each call, so a second `Mesh` breaks `instanceof` against the first — which is why the rule is one call per sibling per realm and not "install where convenient". A Worker realm gets its own share through `installWorkerSiblingApis` (same file, called from `installWorkerBroRoot`): the siblings whose state is per thread — `bro.ear` and `SynthGraph`, gameagent, mesh + rigging, tensor, lm, soundml's compute classes, diffusion, vision, flora — and nothing that reaches the engine (`AudioContext`, `bro.mic`, the listen stack). The Workers section of `src/bronze_host/README.md` has the rules.

**Sibling api tests.** Each sibling has `tests/test_*api*.cpp` registered as a ctest (names in the table; they run standalone only, since the siblings gate `tests/` on being the top-level project) that boots a bronze realm, calls the installer, and checks the mount points and shape. Every one carries the same `if(WIN32)` block: a `POST_BUILD` `copy_if_different` of `$<TARGET_FILE:bronze_runtime_shared>` and its import library beside the test executable, and `PATH` set on the test property, because bronze builds the DLL into `BRONZE_SHARED_RUNTIME_DIR` and the PE loader only looks beside the `.exe` (`0xc0000135` and a modal "dll was not found" otherwise). bro's `bro_bronze_stage_runtime` does the same for bro's own executables, but as one custom command all four depend on rather than a `POST_BUILD` on each: four `copy_if_different`s of one file into one directory at the end of a parallel build race, and on macOS (`clonefile()` behind an unlink) a racing copy can remove the file another just staged.

**What `-Sync` now means.** A sibling api is compiled against bronze's `embed` headers and linked to `bronze_runtime_shared`, and the compiled-JS siblings carry bronze's ABI fingerprint in their objects. A bronze change that moves that surface therefore has to land in bronze **and** in every sibling that binds it before bro's pins can move, and the pins have to move together: a bro commit that pins a new bronze against old sibling pins does not build in CI, where every sibling is its pin. That is exactly the case `scripts/repo-status.sh --sync` exists for (one commit moving every stale pin), and one more reason pin bumps are batched at the end of a session rather than made per commit.

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
# bronze and brass come from ../bronze and ../brass or their pins, and on
# Windows the POST_BUILD step stages bronze_runtime_shared.dll beside the test exe.
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

### 4. Move the pins — at the end of the session, not per commit

The pins are what CI and a fresh clone build, so they have to move eventually — but **not with every sibling commit**. Move them once at the end of a session, after the siblings are pushed, so every pin names a commit that is on GitHub. Per-commit bumps produce a bro history of pin-only commits and, since the [`<name>_api` libraries](#sibling-javascript-apis-name_api) bind bronze, a half-moved set (new bronze, old siblings) that does not build in CI. `AGENTS.md` at the bro root states the same rule.

`scripts/bump-deps.sh` rewrites pins in place (review with `git diff`, then commit):

```bash
scripts/bump-deps.sh                     # every wlejon pin -> that repo's GitHub HEAD
scripts/bump-deps.sh brokit bronze       # just these
scripts/bump-deps.sh --local brokit      # -> HEAD of ../brokit (warns if not pushed yet)
scripts/bump-deps.sh SDL                 # third-party pins move only when named
scripts/bump-deps.sh --repo ../bromux --local   # a sibling's own pins
git commit -m "Pin brokit: add new API" -- cmake/bro_pins.cmake
```

A sibling's own pins (used when it is built standalone) move the same way with `--repo`. Push in dependency order — leaves first, bro last, the apps that pin bro after it — so that no pushed pin names a commit GitHub does not have.

## Status, pull, sync across all repos

`scripts/repo-status.ps1` (Windows) and `scripts/repo-status.sh` (Linux/macOS) are the same tool in two ports. Run either from anywhere; both resolve paths from the script location.

Both walk every repo in `scripts/repos.txt`, grouped as there (runtime, compiler, engine, terminal, desktop, app, tool): branch, dirty/staged/untracked counts, and ahead/behind against the upstream as last fetched (`up<n>` / `dn<n>`; an upstream that is not on `origin` is shown in brackets, a branch with none says `no upstream`). A repo that is not checked out at `../<name>` is listed and skipped. The pin report then compares, for each repo the list marks `pinned`, the commit `cmake/bro_pins.cmake` pins against the HEAD of `../<name>` (`STALE PIN` when they differ), and names any wlejon pin that the list does not mark, so the two cannot drift apart unnoticed.

```powershell
pwsh scripts/repo-status.ps1              # working-tree state + stale pins
pwsh scripts/repo-status.ps1 -ListFiles   # also list changed files in dirty repos
pwsh scripts/repo-status.ps1 -Pull        # fast-forward everything first, then report
pwsh scripts/repo-status.ps1 -Pull -Sync  # ...and move bro's stale pins + commit
pwsh scripts/repo-status.ps1 -Push        # push repos ahead of upstream to their remotes
pwsh scripts/repo-status.ps1 -Sync -Push  # move pins, commit in bro, push all (bro last)
```

```bash
scripts/repo-status.sh              # -v / --verbose, -p / --pull, -s / --sync, -u / --push
scripts/repo-status.sh --pull
scripts/repo-status.sh --sync --push
```

**`-Pull` / `--pull`** fast-forwards every listed repo onto its upstream before the report, so what you read reflects the remotes rather than whatever you last fetched. Use it after a round of merges lands on GitHub (dependabot, PRs merged from the web) to bring the whole tree forward in one shot. It is deliberately conservative:

- `--ff-only`, so a repo that has diverged from its upstream is reported and skipped, never merged or rebased. Resolve those by hand.
- `-c pull.rebase=false`, because a repo configured to rebase on pull refuses outright when the tree is dirty — even for a pure fast-forward. Forcing the merge backend removes that false failure without ever allowing a real merge.
- `--no-recurse-submodules`, a no-op now that there are none, kept so a stray `.gitmodules` in some unrelated checkout is never followed.
- Detached HEADs and branches with no upstream are reported and skipped.

**`-Sync` / `--sync`** then moves bro's stale pins to the `../<name>` HEADs (`scripts/bump-deps.sh --local`) and records them in a single bro commit of `cmake/bro_pins.cmake`. It only acts where the working tree is ahead of (or diverged from) the pin; a sibling whose working tree is *behind* the pin is left alone, since that one needs a pull, not a bump. Note the ordering `-Pull -Sync` implies: pull first so the pins you record are the real remote HEADs.

**`-Push` / `-u, --push`** pushes every listed repo that has local commits ahead of its upstream, siblings first and bro last, so bro's pins never name a commit GitHub does not have yet. With `-Sync` (`-Sync -Push`) the pins move and are committed first.

## Overriding Paths

Any dependency can be pointed at another tree with FetchContent's own variable, upper-cased name:

```bash
cmake -B build \
    -DFETCHCONTENT_SOURCE_DIR_BROKIT=/path/to/brokit \
    -DFETCHCONTENT_SOURCE_DIR_BRONZE=/path/to/bronze \
    -DFETCHCONTENT_SOURCE_DIR_SDL=/path/to/SDL
```

To build against the pins even with working trees beside bro, configure from a directory that has no siblings (a fresh clone somewhere else) — or point a single dependency at a downloaded copy the same way.

brass also honours an explicit `BRASS_ROOT` (cache variable or environment), read in one place inside bro, `third_party/CMakeLists.txt`, which runs before anything else names brass; every later brass block (bronze's, the siblings') finds the target already there.

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
