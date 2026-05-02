# Building Bro

## Prerequisites

**Windows:**
- **MSVC** (Visual Studio 2022+) — MinGW is not supported
- **CMake** 3.24+
- **vcpkg** (for GameNetworkingSockets — e.g. `D:/vcpkg`, with `VCPKG_ROOT` set or passed via `-DCMAKE_TOOLCHAIN_FILE`)
- **Skia** pre-built libraries in `third_party/skia/`

**Linux (Debian/Ubuntu):**
- **GCC 12+** or **Clang 15+**
- **CMake** 3.24+
- System packages: `build-essential cmake libfreetype-dev libfontconfig-dev libgl-dev libjpeg-dev libpng-dev libwebp-dev`
- **vcpkg** (for GameNetworkingSockets)
- **Skia** pre-built library (see below)

**macOS (12+, arm64 or x86_64):**
- **Xcode Command Line Tools** (`xcode-select --install`) — Apple clang 17+
- One command for the brew deps: `brew install cmake ninja bash pkg-config`
  - **CMake** 3.24+ and **Ninja** for building
  - **bash 4+** for `tests/run_tests.sh` (system bash 3.2 lacks `mapfile`)
  - **pkg-config** is needed by vcpkg's abseil port while building GameNetworkingSockets
- **vcpkg** (for GameNetworkingSockets)
- **Skia** pre-built library (see below)

On Apple Silicon machines that were provisioned via Migration Assistant from
an Intel Mac, double-check that Homebrew is the native arm64 build at
`/opt/homebrew` — a leftover Intel brew at `/usr/local` runs under Rosetta
and its cmake will default to the `x64-osx` vcpkg triplet, which won't match
the arm64 GameNetworkingSockets install. The top-level `CMakeLists.txt`
detects arm64 hardware via `sysctl` and forces `arm64-osx` / `-arch arm64`
as a defense, but running a native brew removes the whole class of problem.

## Setup

```bash
git clone --recursive https://github.com/wlejon/bro
cd bro
```

## Building Skia

Skia is a pre-built dependency — it is not built automatically by CMake. Place the built library in `third_party/skia/lib/{Debug,Release}/`.

On Linux, a build script is provided:

```bash
# Install prerequisites (Debian/Ubuntu)
sudo apt install build-essential clang python3 ninja-build \
                 libfreetype-dev libfontconfig-dev libgl-dev \
                 libjpeg-dev libpng-dev libwebp-dev

# Build Skia (clones source, syncs deps, builds, and installs libskia.a)
cd third_party/skia
./build_skia_linux.sh           # Release only
./build_skia_linux.sh Debug     # Debug only
./build_skia_linux.sh all       # Both
```

On macOS, an equivalent script is provided — Skia is built with CoreText as
the font backend (freetype/fontconfig disabled):

```bash
cd third_party/skia
./build_skia_mac.sh             # Release only
./build_skia_mac.sh Debug       # Debug only
./build_skia_mac.sh all         # Both
```

On Windows, build Skia separately with `gn`/`ninja` and place `skia.lib` in the same location.

## Build

```bash
# Configure
cmake -B build

# Debug build
cmake --build build --config Debug

# Release build
cmake --build build --config Release
```

On Windows this uses the Visual Studio multi-config generator. On Linux and macOS it defaults to a single-config generator; set `CMAKE_BUILD_TYPE` at configure time or pass `-G Ninja`.

## Running

bro ships no apps of its own — the launcher and starter apps live in the sibling [broworkshop](https://github.com/wlejon/broworkshop) repo. Clone it next to bro and point bro at it:

```bash
cd ..
git clone https://github.com/wlejon/broworkshop
cd bro

# Windows
./build/Release/bro.exe ../broworkshop          # → launcher (default_app)
./build/Release/bro.exe ../broworkshop/games/snake

# Linux / macOS
./build/bro ../broworkshop
./build/bro ../broworkshop/games/snake
```
