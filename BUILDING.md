# Building Bro

## Quickstart

```bash
git clone --recursive https://github.com/wlejon/bro
cd bro

cmake -B build
cmake --build build --config Release      # Windows (Visual Studio multi-config)
```

That's it — **Skia downloads itself** (prebuilt, from the repo's GitHub releases)
and the default `app` profile builds the full runtime. The only external
dependency the default build needs is **vcpkg** (for networking + video); if you
don't have it, build the `minimal` profile instead, which needs nothing beyond
the submodules:

```bash
cmake -B build -DBRO_PROFILE=minimal      # HTML/CSS/JS + Canvas2D + WebGL + audio; no vcpkg
```

On Linux/macOS (single-config generators) drop `--config` and set the build type
at configure time — see [Build commands](#build-commands) below.

## Build profiles

The build is modular. `-DBRO_PROFILE=<name>` picks how much of the runtime is
compiled in; individual `-DBRO_WITH_*` flags override the profile per-feature.
A feature that's compiled out still installs its JS namespace as a stub that
reports `{ available: false }`, so apps feature-detect instead of crashing.

| Profile | Contents | Needs vcpkg? | Needs CUDA? |
|---|---|:--:|:--:|
| `minimal` | HTML/CSS/JS + Canvas2D + WebGL + audio. 2D renderer only. | no | no |
| **`app`** (default) | Full renderer (3D scene graph, Jolt physics, audio, core game-AI) + networking, video, Steam. No AI tower. | yes | no |
| `full` | Everything — adds the on-device AI tower (tensor, LM, diffusion, vision, audio-ML). | yes | opt-in |

- **`minimal`** (~16 MB) builds, links, and runs with no vcpkg, no CUDA, and no
  `brotensor`. It's the HTML/CSS/JS + Canvas2D + WebGL floor, plus audio (broaudio
  is self-contained and `<audio>`/Web Audio is core to HTML). `bro.scene`,
  `Physics`, `bro.net`, `VideoEncoder`, and the whole `bro.lm`/`bro.diffusion`/…
  tower report unavailable.
- **`app`** (~22 MB, default) is a complete web/app/game runtime. It needs vcpkg
  only because networking (GameNetworkingSockets) and video decode (libvpx/webm)
  pull vcpkg ports — a runtime where `bro.net` and `<video>` silently didn't work
  would be surprising, so they're in the default.
- **`full`** adds the AI tower. The CUDA GPU backend stays **opt-in even here**
  (`-DBRO_WITH_TENSOR_CUDA=ON`, needs the CUDA toolkit) — it's the single largest
  build-time cost.

Enabling a flag force-enables its prerequisites (e.g. `-DBRO_WITH_DIFFUSION=ON`
pulls in `LM` → `TENSOR`), so inconsistent combinations are impossible. See
[docs/build-options.md](docs/build-options.md) for the full flag tier list, the
dependency graph, and the stub mechanism.

```bash
cmake -B build                                        # app (default)
cmake -B build -DBRO_PROFILE=minimal                  # 2D/canvas/WebGL floor, no vcpkg
cmake -B build -DBRO_PROFILE=full                     # + AI tower (CPU)
cmake -B build -DBRO_PROFILE=full -DBRO_WITH_TENSOR_CUDA=ON   # + AI tower on CUDA
cmake -B build -DBRO_PROFILE=minimal -DBRO_WITH_3D=ON # minimal + just the 3D scene graph
```

> Note: `-DBRO_PROFILE` seeds flag defaults on the **first** configure of a build
> dir. Switching the profile on an existing build dir won't move flags already in
> the CMake cache — reconfigure fresh (or clear the specific `BRO_WITH_*` cache
> entries) to re-baseline.

## Prerequisites

Dependency versions are **pinned** by the `vcpkg.json` manifest at the repo
root: its `builtin-baseline` locks every port (and transitives — protobuf,
openssl, abseil, …) to one microsoft/vcpkg commit, so every machine and CI
build identical dependencies. When the vcpkg toolchain is active, the deps
install automatically into `<build>/vcpkg_installed` at configure time — no
manual `vcpkg install` step. Your vcpkg clone must contain the baseline
commit; if configure can't resolve it, `git -C <vcpkg> pull` and retry. To
bump dependency versions, update the baseline in `vcpkg.json` **and** the
matching `VCPKG_COMMIT` in `.github/workflows/{ci,nightly}.yml`.

**Windows:**
- **MSVC** (Visual Studio 2022+) — MinGW is not supported
- **CMake** 3.24+
- **vcpkg** — only for the `app`/`full` profiles (networking + video). Set
  `VCPKG_ROOT`, or pass `-DCMAKE_TOOLCHAIN_FILE=…`, or install it at a
  common location (`../vcpkg`, `%HOME%/vcpkg`) where CMake auto-detects it.
  The `minimal` profile needs no vcpkg.

**Linux (Debian/Ubuntu):**
- **GCC 12+** or **Clang 15+**
- **CMake** 3.24+ and **Ninja** (`-G Ninja`, or install `ninja-build`)
- System packages (needed by all profiles — Skia links them):
  `build-essential cmake ninja-build libfreetype-dev libfontconfig-dev libgl-dev libjpeg-dev libpng-dev libwebp-dev`
- **vcpkg** — only for `app`/`full` (networking + video). The video dep
  (libvpx) assembles with **`nasm`** — `sudo apt-get install nasm` (vcpkg
  auto-acquires it on Windows/macOS, but refuses to on Linux).

**macOS (12+, arm64 or x86_64):**
- **Xcode Command Line Tools** (`xcode-select --install`) — Apple clang 17+
- `brew install cmake ninja bash pkg-config`
  - **CMake** 3.24+ and **Ninja** for building
  - **bash 4+** for `tests/run_tests.sh` (system bash 3.2 lacks `mapfile`)
  - **pkg-config** is used by vcpkg's abseil port while building GameNetworkingSockets
- **vcpkg** — only for `app`/`full`.

On Apple Silicon machines provisioned via Migration Assistant from an Intel Mac,
verify Homebrew is the native arm64 build at `/opt/homebrew` — a leftover Intel
brew at `/usr/local` runs under Rosetta and its cmake defaults to the `x64-osx`
vcpkg triplet, which won't match an arm64 GameNetworkingSockets install. The
top-level `CMakeLists.txt` detects arm64 hardware via `sysctl` and forces
`arm64-osx` / `-arch arm64` as a defense, but a native brew removes the whole
class of problem.

**Submodules** must be initialized (a plain `git clone` without `--recursive`
leaves them empty and CMake stops with a clear error):

```bash
git submodule update --init --recursive
```

Sibling libraries (`brokit`, `htmlayout`, `broaudio`, `bromesh`, `qjsbind`,
`brogameagent`, …) are also picked up from standalone checkouts at `../<name>`
if present — see [docs/multi-repo-workflow.md](docs/multi-repo-workflow.md).

## Skia

Skia is a **prebuilt** dependency, and by default CMake fetches it for you: on the
first configure it downloads the headers/source bundle and the Release library
from the repo's GitHub releases (pinned to Skia `chrome/m147`, SHA-256 verified),
and drops them into `third_party/skia/`. The Release library is used for **all**
configs, including Debug. Nothing to do — it just works on:

- **Windows** (x64)
- **Linux** (x64)
- **macOS** Apple Silicon (arm64)

Disable the download with `-DBRO_FETCH_SKIA=OFF`. You need to **build Skia by
hand** only in these cases (no prebuilt is hosted, or you want a different one):

- **Intel (x86_64) macOS** — no prebuilt arm64 lib would link
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

On Windows, build Skia with `gn`/`ninja` (`cd third_party/skia/src && python3
tools/git-sync-deps`, then `bin/gn gen` + `ninja` — see the args in `CLAUDE.md`)
and place `skia.lib` in `third_party/skia/lib/{Debug,Release}/`.

## Build commands

**Windows** — Visual Studio multi-config generator. One build dir; pick the
config at build time:

```bash
cmake -B build
cmake --build build --config Debug
cmake --build build --config Release
```

**Linux / macOS** — Ninja single-config generator. `--config` is ignored, so use
a separate build dir per config:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

All three executables — `bro` (windowed), `bro-headless` (JS scripting/testing),
and `bro-server` — are produced. On Windows they land in `build/Debug/` and
`build/Release/`; on Linux/macOS directly in the build dir.

## Running

bro ships no apps of its own — the launcher and starter apps live in the sibling
[broworkshop](https://github.com/wlejon/broworkshop) repo. Clone it next to bro
and point bro at it:

```bash
cd ..
git clone https://github.com/wlejon/broworkshop
cd bro

# Windows
./build/Release/bro.exe ../broworkshop                  # → project manager / launcher
./build/Release/bro.exe ../broworkshop/games/snake

# Linux / macOS
./build/bro ../broworkshop
./build/bro ../broworkshop/games/snake
```

Running `bro` with no arguments opens the built-in project manager. For headless
scripting and testing (`bro-headless`), see [docs/headless.md](docs/headless.md).
