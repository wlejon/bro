# Building Bro

## Quickstart

```bash
git clone https://github.com/wlejon/bro
cd bro

cmake -B build
cmake --build build --config Release      # Windows (Visual Studio multi-config)
```

That's it. **Skia downloads itself** (prebuilt, from the repo's GitHub releases)
and the default `app` profile builds the full runtime. The only external
dependency the default build needs is **vcpkg** (for networking + video); if you
don't have it, build the `minimal` profile instead, which needs nothing beyond
what configure downloads itself:

```bash
cmake -B build -DBRO_PROFILE=minimal      # HTML/CSS/JS + Canvas2D + WebGL + audio; no vcpkg
```

On Linux/macOS (single-config generators) drop `--config` and set the build type
at configure time. See [Build commands](#build-commands) below.

## Build profiles

The build is modular. `-DBRO_PROFILE=<name>` picks how much of the runtime is
compiled in; individual `-DBRO_WITH_*` flags override the profile per-feature.
A feature that's compiled out still installs its JS namespace as a stub that
reports `{ available: false }`, so apps feature-detect instead of crashing.

| Profile | Contents | Needs vcpkg? | Needs CUDA? |
|---|---|:--:|:--:|
| `minimal` | HTML/CSS/JS + Canvas2D + WebGL + audio. 2D renderer only. | no | no |
| `desktop` | Full renderer (3D scene graph, Jolt physics, audio, core game-AI), terminal, desktop libraries, remote. No vcpkg services (net/video/AVIF/Steam) and no AI tower. | no | no |
| **`app`** (default) | Full renderer (3D scene graph, Jolt physics, audio, core game-AI), terminal, desktop libraries, remote + networking, video, Steam. No AI tower. | yes | no |
| `full` | Everything, adding the on-device AI tower (tensor, LM, diffusion, vision, audio-ML). | yes | opt-in |

- **`minimal`** builds, links, and runs with no vcpkg, no CUDA, and no
  `brotensor`. It's the HTML/CSS/JS + Canvas2D + WebGL floor, plus audio (broaudio
  is self-contained and `<audio>`/Web Audio is core to HTML). `bro.scene`,
  `Physics`, `bro.net`, `VideoEncoder`, and the whole `bro.lm`/`bro.diffusion`/…
  tower report unavailable. CI builds this profile on all three platforms and
  runs `tests/_smoke_app/minimal_smoke.js` against it — the floor above, plus a
  check that every compiled-out namespace is still present and says which flag
  it wants. Run it yourself the same way:

  ```bash
  ./build/bro-headless tests/_smoke_app tests/_smoke_app/minimal_smoke.js
  ```

  Most of `tests/` needs features this profile doesn't have, so the full suite
  is not the check here.
- **`desktop`** enables the full renderer, `<terminal>`, `bro.remote`, and all
  desktop-environment libraries (`bro.vfs`, `bro.sys`, `bro.compositor`, …)
  while leaving out the vcpkg-backed services (`BRO_WITH_NET`, `BRO_WITH_VIDEO`,
  `BRO_WITH_AVIF`, `BRO_WITH_STEAM`) and the AI tower, so it builds with no
  vcpkg and no CUDA.
- **`app`** (default) is a complete web/app/game runtime. It needs vcpkg
  only because networking (GameNetworkingSockets) and video decode (libvpx/webm)
  pull vcpkg ports. A runtime where `bro.net` and `<video>` silently didn't work
  would be surprising, so they're in the default.
- **`full`** adds the AI tower. The CUDA GPU backend stays **opt-in even here**
  (`-DBRO_WITH_TENSOR_CUDA=ON`, needs the CUDA toolkit), since it's the single largest
  build-time cost. On Linux the Vulkan backend (`BRO_WITH_TENSOR_VULKAN`, the
  AMD GPU path, and any other Vulkan 1.2+ GPU) is auto-detected when `glslc` and
  the Vulkan headers and loader are present.

Enabling a flag force-enables its prerequisites (e.g. `-DBRO_WITH_DIFFUSION=ON`
pulls in `LM` → `TENSOR`), so inconsistent combinations are impossible. See
[docs/build-options.md](docs/build-options.md) for the full flag tier list, the
dependency graph, and the stub mechanism.

```bash
cmake -B build                                        # app (default)
cmake -B build -DBRO_PROFILE=minimal                  # 2D/canvas/WebGL floor, no vcpkg
cmake -B build -DBRO_PROFILE=desktop                  # full renderer + desktop substrate, no vcpkg
cmake -B build -DBRO_PROFILE=full                     # + AI tower (CPU)
cmake -B build -DBRO_PROFILE=full -DBRO_WITH_TENSOR_CUDA=ON   # + AI tower on CUDA
cmake -B build -DBRO_PROFILE=full -DBRO_WITH_TENSOR_VULKAN=ON # + AI tower on Vulkan (any Vulkan 1.2+ GPU)
cmake -B build -DBRO_PROFILE=minimal -DBRO_WITH_3D=ON # minimal + the 3D scene graph (forces PHYSICS + GAMEAI on)
```

> Note: `-DBRO_PROFILE` seeds flag defaults on the **first** configure of a build
> dir. Switching the profile on an existing build dir won't move flags already in
> the CMake cache. Reconfigure fresh (or clear the specific `BRO_WITH_*` cache
> entries) to re-baseline.

### JavaScript execution (bronze)

Bronze is bro's sole JavaScript engine: it compiles and runs JavaScript
via [bronze](https://github.com/wlejon/bronze) (backed by [brass](https://github.com/wlejon/brass)).
Bronze is **mandatory and included in all profiles**.

```bash
cmake -B build                                            # binaries with bronze JS runtime
cmake --build build --config Release --target bronze-cli # ...and the compiler that makes one
```

bronze resolves like every other sibling (see
[Dependencies](#dependencies)): a `../bronze` checkout first, otherwise the
head of bronze's main, downloaded at configure. The configure line
says which it took (`bronze: working tree <path>` /
`bronze: github.com/wlejon/bronze <sha> (default branch)`), so a build of GitHub's bronze is never
mistaken for a build against the tree you are editing.
`-DFETCHCONTENT_SOURCE_DIR_BRONZE=<path>` points it anywhere else.

The `bronze` target is `EXCLUDE_FROM_ALL` — bro links bronze's *runtime*, not
its compiler — so building the compiler means naming it, as above. Name
`bronze-cli` and not `bronze`: it builds the same binary and is the one name
that works under every generator, because Visual Studio leaves an
`EXCLUDE_FROM_ALL` subdirectory's targets out of the `.sln` and answers
`--target bronze` with "Project file does not exist". It needs nothing beyond
brass, which bro already builds from source as bronze's only backend. A tree
where the target was never built runs pre-built compiled apps without being
able to build new ones, and `tests/bronze_host/run_checks.sh` skips rather than
fails in that case.

A compiled module carries the ABI fingerprint of the bronze that emitted it and
the host refuses any module whose fingerprint is not its own, so rebuilding
bronze invalidates every module already built against it. `src/bronze_host/README.md`
is the reference.

## Prerequisites

Dependency versions are **pinned** by the `vcpkg.json` manifest at the repo
root: its `builtin-baseline` locks every port (and transitives such as protobuf,
openssl, and abseil) to one microsoft/vcpkg commit, so every machine and CI
build identical dependencies. When the vcpkg toolchain is active, the deps
install automatically into `<build>/vcpkg_installed` at configure time, with no
manual `vcpkg install` step. Your vcpkg clone must contain the baseline
commit; if configure can't resolve it, `git -C <vcpkg> pull` and retry. To
bump dependency versions, update the baseline in `vcpkg.json` **and** the
matching `VCPKG_COMMIT` in `.github/workflows/{ci,nightly}.yml`.

Every platform needs **Vulkan 1.3** (headers and loader) to build and a Vulkan
1.3 device with dynamic rendering and timeline semaphores to render (`--no-gpu`
draws with Skia on the CPU without one, minus WebGL and the 3D scene). Shaders need no tool: bro compiles GLSL
to SPIR-V with **glslang**, built from source and linked in-process
(`cmake/glslang.cmake` downloads a pinned release at configure, like Skia;
`-DFETCHCONTENT_SOURCE_DIR_GLSLANG=<checkout>` builds offline). The compiled
pipelines persist in `<user cache dir>/pipeline-cache/` beside the code cache
(`BRO_PIPELINE_CACHE=0` turns that off).

**Windows:**
- **MSVC** (Visual Studio 2022+). MinGW is not supported
- **CMake** 3.24+
- **Vulkan SDK** (LunarG, 1.3+) for the headers and `vulkan-1.lib`; its
  installer sets `VULKAN_SDK`, which CMake finds. The `vulkan-1.dll` loader
  ships with every GPU driver.
- **vcpkg**, only for the `app`/`full` profiles (networking + video). Set
  `VCPKG_ROOT`, or pass `-DCMAKE_TOOLCHAIN_FILE=…`, or install it at a
  common location (`../vcpkg`, `%HOME%/vcpkg`) where CMake auto-detects it.
  The `minimal` and `desktop` profiles need no vcpkg.

**Linux (Debian/Ubuntu):**
- **GCC 12+** or **Clang 15+**
- **CMake** 3.24+ and **Ninja** (`-G Ninja`, or install `ninja-build`)
- System packages (needed by all profiles, since Skia links them):
  `build-essential cmake ninja-build libfreetype-dev libfontconfig-dev libjpeg-dev libpng-dev libwebp-dev`
- **vcpkg**, only for `app`/`full` (networking + video). The video dep
  (libvpx) assembles with **`nasm`**, so `sudo apt-get install nasm` (vcpkg
  auto-acquires it on Windows/macOS, but refuses to on Linux).
- **Vulkan 1.3** headers and loader: `sudo apt-get install libvulkan-dev`
  (Arch: `vulkan-headers vulkan-icd-loader`), plus a driver — your GPU's
  (Mesa RADV/ANV, NVIDIA) or `mesa-vulkan-drivers` for Lavapipe, the software
  device headless runs on without a GPU. `vulkan-validationlayers` is
  optional; `tests/run_tests.sh` turns validation on wherever it is installed.

**macOS (12+, arm64 or x86_64):**
- **Xcode Command Line Tools** (`xcode-select --install`), Apple clang 17+
- `brew install cmake ninja bash pkg-config molten-vk vulkan-loader vulkan-headers`
  - **CMake** 3.24+ and **Ninja** for building
  - **bash 4+** for `tests/run_tests.sh` (system bash 3.2 lacks `mapfile`)
  - **pkg-config** is used by vcpkg's abseil port while building GameNetworkingSockets
  - **MoltenVK** + the **Khronos Vulkan loader**: Vulkan on macOS is MoltenVK
    over Metal (bro enables `VK_KHR_portability_enumeration` /
    `portability_subset`). The LunarG macOS SDK works too. A packaged bro
    carries its own loader and MoltenVK (`scripts/package-release.sh`), so
    users install nothing. `brew install vulkan-validationlayers` for
    validation in tests.
- **vcpkg**, only for `app`/`full`.

On Apple Silicon machines provisioned via Migration Assistant from an Intel Mac,
verify Homebrew is the native arm64 build at `/opt/homebrew`. A leftover Intel
brew at `/usr/local` runs under Rosetta and its cmake defaults to the `x64-osx`
vcpkg triplet, which won't match an arm64 GameNetworkingSockets install. The
top-level `CMakeLists.txt` detects arm64 hardware via `sysctl` and forces
`arm64-osx` / `-arch arm64` as a defense, but a native brew removes the whole
class of problem.

## Dependencies

There are no git submodules; a plain `git clone` is the whole checkout.
`cmake/bro_pins.cmake` declares every dependency: each ecosystem sibling with
no commit, so it tracks its main (FastNoise2, wlejon's fork, among them), and SDL, Jolt and libjpeg-turbo pinned to exact
commits. `cmake/bro_deps.cmake`'s `bro_dependency()` resolves each one at
configure time, in order:

1. a target that already exists;
2. a working tree at `../<name>` beside the source dir (siblings only, not
   third-party code), or any path given as `-DFETCHCONTENT_SOURCE_DIR_<NAME>=<path>`;
3. the GitHub archive tarball of one commit, downloaded into
   `<build>/_deps/<name>-src`: the commit `cmake/bro_lock.cmake` locks on a
   release tag, else a third-party pin, else the head of the sibling's main,
   looked up (all at once) at every configure.

So a lone clone downloads everything it needs, at the siblings' current mains,
and a checkout with siblings beside it (`brokit`, `htmlayout`, `broaudio`,
`bronze`, `brass`, …) builds against those working trees. Offline, a configure
reuses the commits that build directory last resolved, with a warning
(`-DBRO_DEPS_OFFLINE=ON` forces that). A release tag carries the lock written by
`scripts/lock-deps.sh`, so it builds the same commits forever. See
[docs/multi-repo-workflow.md](docs/multi-repo-workflow.md), and
[docs/ecosystem.md](docs/ecosystem.md) for the list of every repo.

## Skia

Skia is a **prebuilt** dependency, and by default CMake fetches it for you: on the
first configure it downloads the headers/source bundle and the Release library
from the repo's GitHub releases (pinned to Skia `chrome/m147`, SHA-256 verified),
and drops them into `third_party/skia/`. The Release library is used for **all**
configs, including Debug. There is nothing to do; it just works on:

- **Windows** (x64)
- **Linux** (x64)
- **macOS** Apple Silicon (arm64)

Disable the download with `-DBRO_FETCH_SKIA=OFF`. You need to **build Skia by
hand** only in these cases (no prebuilt is hosted, or you want a different one):

- **Intel (x86_64) macOS**, where no prebuilt arm64 lib would link
- a **Windows Debug** Skia lib (the hosted lib is Release; only needed if you
  specifically want a Debug Skia)
- a **different Skia version**

Build scripts are provided for Linux and macOS (they clone the source, sync deps,
build, and install the static lib into `third_party/skia/lib/{Debug,Release}/`):

```bash
cd third_party/skia
./build_skia_linux.sh            # Linux:  Release only  (or: Debug | all)
./build_skia_mac.sh              # macOS:  Release only  (CoreText backend; or: Debug | all)
```

On Windows, run `third_party\skia\build_skia_windows.ps1` from a Developer
PowerShell for VS 2022 (x64). It checks out the pinned commit
(`abbe599fb3c0ef2fa82bfadbb0ddcd321f22faf0`, the `chrome/m147` commit the source
bundle was cut from), syncs deps, builds Ganesh for Vulkan only with MSVC and the
dynamic CRT, merges in pathops, and installs `skia.lib` into
`third_party/skia/lib/{Debug,Release}/`. The hosted Windows lib predates it and
was built with both GL and Vulkan; bro uses only the Vulkan half.

Both build scripts build the Vulkan backend too (`skia_use_vulkan=true`,
`skia_use_gl=false`; on macOS it runs on MoltenVK). `SKIA_SRC=/path/to/checkout`
points them at a full checkout, since `third_party/skia/src` holds the trimmed
source bundle, which is not buildable.

Whichever route you take, the Skia **source** tree must carry the shaping
modules: `modules/skshaper`, `modules/skunicode`, and the bundled HarfBuzz and
ICU sources. bro compiles those itself (`third_party/skia/skia_modules.cmake`)
rather than taking them from the pre-built lib, and `BRO_WITH_TEXT_SHAPING`
defaults ON in every profile. A hand-built or older bundle that lacks them will
configure with shaping silently unavailable, which is a different renderer: no
ligatures, no cursive joining, no bidi. See `third_party/skia/BUNDLE.md`.

## Build commands

**Windows** uses the Visual Studio multi-config generator. One build dir; pick the
config at build time:

```bash
cmake -B build
cmake --build build --config Debug
cmake --build build --config Release
```

**Linux / macOS** use the Ninja single-config generator. `--config` is ignored, so use
a separate build dir per config:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

The build produces the engine library, `libbro` (`libbro.so`, `libbro.dll`,
`libbro.dylib`), and the executables that run it: `bro` (windowed),
`bro-headless` (JS scripting/testing), `bro-server`, and `bro-app-stub` (the
template installed apps' own executables are made from). On Windows they land
in `build/Debug/` and `build/Release/`; on Linux/macOS directly in the build
dir. They belong together: copy or package the directory, not one file of it.

## The engine library

All of bro (the static libraries under `src/`, and the sibling libraries
linked into them) is one shared library, `libbro`. `bro`, `bro-headless`,
`bro-server` and every installed app's own executable are a few lines that
call into it (`src/runtime/bro_entry.h`). The point is memory: a desktop runs
many bro processes (helm and every app it launches), and they all map the one
file, so the engine's code (well over 100 MB of it) is in memory once,
shared clean, rather than once per distinct executable. `bro-ctl mem`
reports it as the `engine image` line ([docs/agent-control.md](docs/agent-control.md#mem)).

**What crosses the boundary.** A C ABI is the contract everywhere:
`bro_run_windowed`, `bro_run_headless`, `bro_run_server`, `bro_run_app`,
`bro_entry_abi` and `bro_engine_version`. Beyond it:

- **Linux and macOS** export the whole C++ surface too. libbro is linked from
  whole archives (`--whole-archive` / `-force_load`) with default visibility,
  so an embedder that drives the `Engine` from C++ links it like any shared
  library: helm does (`find_package(bro)` against a bro build tree, which
  writes `bro-config.cmake`, or `bro::libbro` in-tree). Calls inside the
  library bind inside it (`-Bsymbolic-functions`), so the exported surface
  costs no PLT hop on the engine's own calls.
- **Windows** exports the C functions and nothing else. The engine's C++
  surface, with eleven sibling libraries inside it, is never annotated for
  export: a DLL's export table cannot hold it (65,535 entries; the full-profile
  libbro.so on Linux defines 68,767 dynamic symbols, and `WINDOWS_EXPORT_ALL_SYMBOLS`
  would add every data symbol on top), so exporting everything fails outright,
  and hand-annotating every class in every repository with an export macro
  would be a tax on every header forever. A Windows embedder that needs the
  C++ API links the static libraries, which are still built
  ([docs/embedding.md](docs/embedding.md)). On Windows that is only helm, and
  helm is Linux-first.

The static libraries remain the build's unit: bro's C++ tests,
`bro-native-manifest` and any embedder that wants a single binary link them
directly. libbro is built from them, not instead of them.

**bronze's runtime** is already its own shared library
(`bronze::runtime_shared`: `bronze_runtime_shared.dll`,
`libbronze_runtime_shared.so`), and stays one. libbro links it, and an app's
ahead-of-time module (`app.dll` / `app.so` beside `index.html`) links it too
and nothing of libbro's: the module reaches the host's globals at run time
through the realm, not through link-time symbols. An installed app's
executable carries its page's scripts compiled the same way (its AOT
directory, docs/apps.md "Ahead-of-time scripts"): each module imports only
from the bronze runtime, which the executable's process already has loaded
through libbro, and there is still exactly one runtime, one heap and one key
registry per process. Those modules are written at install by brass's own
image writer, in process, so installing needs no compiler or linker on any
platform that builds app executables. Each one is checked before it runs
against a record of what it was compiled from, including the bronze runtime
ABI, the realm's host globals and the natives manifest, rather than against
`bro_engine_version`: a relink of libbro that changes none of those keeps
them, and one that changes any of them runs the page on the JIT while the
directory is rebuilt.

**An app's own executable is made without a compiler.** `bro --install`
copies `bro-app-stub` (built with bro, shipped beside libbro), names the copy
for the app (`helm.calculator` → `helm-calculator`) and stamps it:

- An identity block, found by its marker and overwritten in place: the
  app's id, name, version and folder, the engine build and content hash it
  was made against, and where libbro and the stock bro are. This is
  format-agnostic (the same bytes in an ELF `.rodata` and a PE `.rdata`), and
  needs no section editing, relinking or tooling.
- On Windows, an icon (every size from 16 to 256 px, rendered from the app's
  icon) and a version resource (`FileDescription` is the app's name, which is
  what Task Manager shows), written with the system's own resource updater
  (`BeginUpdateResource`, in kernel32 on every Windows). No `rc.exe`, no SDK.
- On Linux there is nothing more to stamp: the process name is the file name,
  and the icon and window class belong to the desktop entry, which `--install`
  writes with `Exec=` the executable and `StartupWMClass=` the app's id.

The stub loads libbro at run time from the path in its block (`dlopen` /
`LoadLibraryEx`) rather than linking it, so the copy can live anywhere (with
the app, not beside bro) and still map the one engine every bro process
maps; and when libbro is missing or speaks another entry ABI, the stub runs
the app as a folder under the stock bro instead of failing. Nothing about it
depends on a toolchain being present on the machine the app is installed on.
Alternatives considered: generating a linker script or object and invoking
the system linker (needs a toolchain, which a user's machine does not have);
rewriting ELF dynamic sections or PE import tables (format surgery with many
edge cases, for no gain over a run-time load); a shell script or `.lnk` that
runs `bro <dir>` (the process is still named `bro`, and Windows groups and
labels by executable). macOS needs an app bundle (Info.plist, re-signing
after stamping); until bro builds one, a macOS install runs the folder under
bro.

**The engine version** (`bro_engine_version()`, `engineVersion()` in
`src/engine/app_exe.h`) is bro's version plus the entry ABI plus the size and
modification time of the engine image. Any relink of libbro changes it, with
no build-time stamping step to keep correct, and an executable built against
another engine build rebuilds itself on its next launch (docs/apps.md).

## Running

bro ships no apps of its own. The launcher and starter apps live in the sibling
[broworkshop](https://github.com/wlejon/broworkshop) repo. Clone it next to bro
and point bro at it:

```bash
cd ..
git clone https://github.com/wlejon/broworkshop
cd bro

# Windows
./build/Release/bro.exe ../broworkshop                  # opens the project manager / launcher
./build/Release/bro.exe ../broworkshop/games/snake

# Linux / macOS
./build-release/bro ../broworkshop
./build-release/bro ../broworkshop/games/snake
```

Running `bro` with no arguments opens the built-in project manager. For headless
scripting and testing (`bro-headless`), see [docs/headless.md](docs/headless.md).
