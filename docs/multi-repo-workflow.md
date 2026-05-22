# Multi-Repo Workflow: bro + sibling libraries

bro depends on eleven sibling libraries. Each has a standalone repo at `../<name>` and a git submodule fallback under `third_party/`.

An eleventh sibling, **[broworkshop](https://github.com/wlejon/broworkshop)** at `../broworkshop`, is **not** a library — it's the apps tree (launcher, games, tools, demos, AI). It has no CMake hook or submodule fallback; bro just runs it via `bro ../broworkshop` or `bro ../broworkshop/bro.json`. See the [Apps tree](#apps-tree) section below.

| Library | Standalone repo | Submodule fallback |
|---------|----------------|-------------------|
| **bromath** | `../bromath` | `third_party/bromath` |
| **qjsbind** | `../qjsbind` | `third_party/qjsbind` |
| **brokit** | `../brokit` | `third_party/brokit` |
| **htmlayout** | `../htmlayout` | `third_party/htmlayout` |
| **broaudio** | `../broaudio` | `third_party/broaudio` |
| **bromesh** | `../bromesh` | `third_party/bromesh` |
| **broflora** | `../broflora` | `third_party/broflora` |
| **brotensor** | `../brotensor` | `third_party/brotensor` (transitive via brogameagent) |
| **brogameagent** | `../brogameagent` | `third_party/brogameagent` |
| **brolm** | `../brolm` | `third_party/brolm` |
| **brodiffusion** | `../brodiffusion` | `third_party/brodiffusion` |

## Directory Layout

```
D:/projects/
├── bro/                          # main project
│   └── third_party/
│       ├── bromath/              # submodule (CI / fallback)
│       ├── qjsbind/              # submodule (CI / fallback)
│       ├── brokit/               # submodule (CI / fallback)
│       ├── htmlayout/            # submodule (CI / fallback)
│       ├── broaudio/             # submodule (CI / fallback)
│       ├── bromesh/              # submodule (CI / fallback)
│       ├── broflora/             # submodule (CI / fallback)
│       ├── brotensor/            # submodule (CI / fallback, resolved via brogameagent)
│       ├── brogameagent/         # submodule (CI / fallback)
│       ├── brolm/                # submodule (CI / fallback)
│       └── brodiffusion/         # submodule (CI / fallback)
├── bromath/                      # standalone repo (preferred for dev)
├── qjsbind/                      # standalone repo (preferred for dev)
├── brokit/                       # standalone repo (preferred for dev)
├── htmlayout/                    # standalone repo (preferred for dev)
├── broaudio/                     # standalone repo (preferred for dev)
├── bromesh/                      # standalone repo (preferred for dev)
├── broflora/                     # standalone repo (preferred for dev)
├── brotensor/                    # standalone repo (preferred for dev)
├── brogameagent/                 # standalone repo (preferred for dev)
├── brolm/                        # standalone repo (preferred for dev)
├── brodiffusion/                 # standalone repo (preferred for dev)
└── broworkshop/                  # apps tree (launcher + games/tools/demos/ai)
```

## How It Works

bro's CMake auto-detects standalone repos at `../<name>`. If found, it builds from there directly — **no submodule copy involved**. This means:

- **Edit once** — only touch files in the standalone repo
- **One build** — `cmake --build build` in bro compiles every sibling from its standalone source
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

The same pattern is used for bromath, qjsbind, htmlayout, broaudio, bromesh, broflora, brogameagent, brolm, and brodiffusion.

Note: bromath is pulled in transitively by several siblings (bromesh, brogameagent, etc.). bro's `third_party/CMakeLists.txt` guards the `add_subdirectory` with `if(NOT TARGET bromath)` so the first loader wins — overriding `BROMATH_DIR` only takes effect if bro is the first to add it.

**brotensor is resolved transitively by brogameagent**, not by bro directly. brotensor is a *hard* dependency of brogameagent — it owns the unified `brotensor::Tensor` type (one type, runtime `Device` tag) and the CPU op backend, so brogameagent's CMake always adds `../brotensor` (or falls back to `third_party/brotensor`). The CPU backend is always built; the CUDA and Metal backends are additive and opt-in. When `-DBROGAMEAGENT_WITH_CUDA=ON` (or `_WITH_METAL=ON`) is set, brogameagent forces the matching `BROTENSOR_WITH_*` backend; brotensor then owns the CUDA / OBJCXX language enables and the `BROTENSOR_HAS_CUDA` / `_HAS_METAL` / `_HAS_GPU` defines, which propagate to bro's `tensor_bindings.cpp`. Without a GPU backend selected, brotensor still builds (CPU-only), `BROTENSOR_HAS_GPU` stays undefined, and the `bro.tensor` JS bindings compile out to a `{ available: false }` stub.

**brolm** (tokenizers + transformer text encoders — CLIP, T5) depends on `bromath` + `brotensor`. It is the text frontend **brodiffusion** depends on, so bro's `third_party/CMakeLists.txt` adds it **after brogameagent** (which has already loaded brotensor/bromath transitively) and **before brodiffusion**. brolm guards both deps with `if(NOT TARGET ...)`, reusing the targets brogameagent added.

**brodiffusion** (diffusion-model inference, `bro.diffusion` JS bindings) depends on `bromath` + `brotensor` + `brolm`. Its `third_party/CMakeLists.txt` block **must be added after brogameagent's and after brolm's**: brogameagent loads brotensor/bromath transitively and sets the `BROTENSOR_WITH_CUDA/_METAL` cache vars, and brodiffusion's CMake guards those deps with `if(NOT TARGET ...)` so it reuses those targets and reads those cache vars. Unlike `bro.tensor`, `bro.diffusion` is **not** gated on a GPU backend — brodiffusion's CPU FP32 path is always built, so the binding is always real. A GPU build (`-DBROGAMEAGENT_WITH_CUDA=ON`) additionally compiles brodiffusion's fused CUDA kernels.

## Day-to-Day Development

### 1. Edit a library

Edit files only in the standalone repo (e.g. `D:/projects/brokit/src/...`, `D:/projects/bromesh/src/...`).

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

# Note: bromesh tests must be built in Release — meshoptimizer's Debug
# assertions trigger a modal abort() dialog on Windows.
cd D:/projects/bromesh && cmake --build build --config Release
./build/tests/Release/bromesh_test.exe
```

### 3. Commit the library

```bash
cd D:/projects/brokit
git add src/api/new_api.cpp
git commit -m "Add new API"
```

### 4. Sync submodule and commit bro

Update the submodule pointer so CI and fresh clones pick up the change:

```bash
cd D:/projects/bro/third_party/brokit
git fetch ../../../brokit main
git checkout FETCH_HEAD

cd D:/projects/bro
git add third_party/brokit
git commit -m "Update brokit: add new API"
```

Same shape for `bromath`, `qjsbind`, `htmlayout`, `broaudio`, `bromesh`, `broflora`, `brotensor`, `brogameagent`, `brolm`, and `brodiffusion`.

## Overriding Paths

To point at a different location for any sibling:

```bash
cmake -B build \
    -DBROMATH_DIR=/path/to/bromath \
    -DQJSBIND_DIR=/path/to/qjsbind \
    -DBROKIT_DIR=/path/to/brokit \
    -DHTMLAYOUT_DIR=/path/to/htmlayout \
    -DBROAUDIO_DIR=/path/to/broaudio \
    -DBROMESH_DIR=/path/to/bromesh \
    -DBROFLORA_DIR=/path/to/broflora \
    -DBROTENSOR_DIR=/path/to/brotensor \
    -DBROGAMEAGENT_DIR=/path/to/brogameagent \
    -DBROLM_DIR=/path/to/brolm \
    -DBRODIFFUSION_DIR=/path/to/brodiffusion
```

Setting any `*_DIR` to a nonexistent path forces the submodule fallback:

```bash
cmake -B build -DBROMATH_DIR=none -DQJSBIND_DIR=none -DBROKIT_DIR=none \
               -DHTMLAYOUT_DIR=none -DBROAUDIO_DIR=none -DBROMESH_DIR=none \
               -DBROFLORA_DIR=none -DBROTENSOR_DIR=none -DBROGAMEAGENT_DIR=none \
               -DBROLM_DIR=none -DBRODIFFUSION_DIR=none
```

## Apps tree

Apps live in [broworkshop](https://github.com/wlejon/broworkshop) at `../broworkshop` — a sibling repo, not a CMake dependency. Its layout:

```
broworkshop/
├── bro.json                  # project manifest (default_app, lib, system)
├── lib/                      # shared JS modules — apps load via "/lib/foo.js"
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
