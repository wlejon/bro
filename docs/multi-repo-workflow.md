# Multi-Repo Workflow: bro + sibling libraries

bro depends on fourteen sibling libraries. Each has a standalone repo at `../<name>` and a git submodule fallback under `third_party/`.

A fifteenth sibling, **[broworkshop](https://github.com/wlejon/broworkshop)** at `../broworkshop`, is **not** a library. It's the apps tree (launcher, games, tools, demos, AI). It has no CMake hook or submodule fallback; bro just runs it via `bro ../broworkshop` or `bro ../broworkshop/bro.json`. See the [Apps tree](#apps-tree) section below.

| Library | Standalone repo | Submodule fallback |
|---------|----------------|-------------------|
| **bromath** | `../bromath` | `third_party/bromath` |
| **qjsbind** | `../qjsbind` | `third_party/qjsbind` |
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
│       ├── brotensor/            # submodule (CI / fallback)
│       ├── brogameagent/         # submodule (CI / fallback)
│       ├── brolm/                # submodule (CI / fallback)
│       ├── brodiffusion/         # submodule (CI / fallback)
│       ├── broimage/             # submodule (CI / fallback)
│       ├── brosoundml/           # submodule (CI / fallback)
│       └── brovisionml/          # submodule (CI / fallback)
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
├── broimage/                     # standalone repo (preferred for dev)
├── brosoundml/                   # standalone repo (preferred for dev)
├── brovisionml/                  # standalone repo (preferred for dev)
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

Most siblings are added **conditionally**, behind the modular-build flags (see [build-options.md](build-options.md)). `bromath`, `qjsbind`, `brokit`, `htmlayout`, `broaudio`, and `broimage` are unconditional; the rest are gated:

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

With a gate off, the sibling is never added and the JS bindings it backs compile out to a `{ available: false }` stub. The flags auto-resolve their prerequisites (`_bro_require` in the top-level `CMakeLists.txt`), so e.g. `BRO_WITH_DIFFUSION=ON` forces `BRO_WITH_LM` and `BRO_WITH_TENSOR` on.

### brotensor

**bro is the first loader of brotensor**: `third_party/CMakeLists.txt` adds it early, before broimage. This is deliberate: each sibling's own fallback resolution assumes it is the standalone CMake root, so when nested under bro it looks for `../brotensor` / `third_party/brotensor` relative to the wrong directory and misses the submodule (this is what broke the full-profile nightly configure). Loading it once from bro, with bro's correct paths, makes every sibling's `if(NOT TARGET brotensor)` guard trip instead.

brotensor is the foundation of the ML stack: it owns the unified `brotensor::Tensor` type (one type, runtime `Device` tag), the device-neutral op family, and the full training surface (forward + backward ops, losses, optimizers) that every other ML sibling composes on. Its backend is fixed at the **first** `add_subdirectory()`, which is why bro's top-level `CMakeLists.txt` forwards the GPU choice into the `BROTENSOR_WITH_CUDA` / `_WITH_METAL` cache vars **before** `add_subdirectory(third_party)`. The CPU backend is always built; CUDA and Metal are additive and opt-in via `-DBRO_WITH_TENSOR_CUDA=ON` / `-DBRO_WITH_TENSOR_METAL=ON`. With a GPU backend selected, brotensor publishes the `BROTENSOR_HAS_CUDA` / `_HAS_METAL` / `_HAS_GPU` defines, which propagate to bro's `tensor_bindings.cpp`; without one, brotensor still builds CPU-only and `BROTENSOR_HAS_GPU` stays undefined.

bro also forwards `BROIMAGE_WITH_TENSOR` (from `BRO_WITH_TENSOR`), `BROGAMEAGENT_WITH_NN` (from `BRO_WITH_GAMEAI_NN`), and `BROGAMEAGENT_WITH_CUDA` / `_WITH_METAL` (from the tensor backend choice) in the same block. Those are internal plumbing, configure with the `BRO_WITH_*` flags, not the sibling ones.

**brolm** (language/text-model inference, tokenizers, text encoders, and generative text / vision-language / translation models) depends on `bromath` + `brotensor`. Because it also provides the text encoders **brodiffusion** consumes, bro's `third_party/CMakeLists.txt` adds it **after the siblings that have already loaded brotensor/bromath** and **before brodiffusion**. brolm guards both deps with `if(NOT TARGET ...)`, reusing the already-loaded targets.

**brodiffusion** (diffusion / flow-matching generative inference, `bro.diffusion` JS bindings) depends on `bromath` + `brotensor` + `brolm`. Its `third_party/CMakeLists.txt` block **must be added after brolm's** (it consumes brolm's text encoders). brodiffusion's CMake guards those deps with `if(NOT TARGET ...)` so it reuses the targets bro already added. Its CPU FP32 path is always built, so whenever `BRO_WITH_DIFFUSION` is on the binding is real regardless of GPU backend; `-DBRO_WITH_TENSOR_CUDA=ON` additionally compiles brodiffusion's fused CUDA kernels.

**broimage** (image decode/encode + composable kernels) is the single home for image work that used to be duplicated across the stack: brokit's `bro.image` JS kernels, bro's HTML `Image` decode, the model siblings' host-side resize + normalize, and pixel preprocessing for the generative siblings. It depends on `bromath` and, when `BRO_WITH_TENSOR` is on, `brotensor` (the tensor adapter forwards `image_normalize` / `image_u8_to_f32_nhwc_to_nchw` to brotensor when the destination is a GPU `Tensor`), bro forwards that choice as `BROIMAGE_WITH_TENSOR`. bro's `third_party/CMakeLists.txt` adds broimage **before brokit** because brokit's image kernels link against `broimage::broimage`, and **after** brotensor so it reuses that target. broimage also vendors its own `stb_image` static lib (with a `if(NOT TARGET stb_image)` guard); bro's own `stb_image` declaration is guarded to match, so either load order works.

**brosoundml** (audio-ML model inference, speech-to-text, text-to-speech, speaker diarization, neural codec, and the streaming wake / keyword / sensor listening stack) depends on `brotensor` for its audio op family (FFT/STFT, 1D conv, vocoder/codec activations, resampling, AR sampling) and on `brolm` for shared text tokenizers. Its `third_party/CMakeLists.txt` block **must be added after brolm's**: a brosoundml tokenizer target links against a brolm tokenizer target, and brosoundml guards `bromath`/`brotensor` with `if(NOT TARGET ...)` so it reuses the already-loaded targets. It backs bro's audio-ML JS bindings (`bro.stt`, `bro.tts`, `bro.wake`, and the rest of the listening stack). Like brodiffusion its CPU path is always built, so with `BRO_WITH_SOUNDML` on the bindings are real; `-DBRO_WITH_TENSOR_CUDA=ON` additionally compiles its fused CUDA kernels.

**brovisionml** (vision-model inference, segmentation, depth, surface normals, background removal, image backbones, the ControlNet conditioning annotators, and generative image models) depends on `bromath` + `brotensor` + `broimage`. By the time its `third_party/CMakeLists.txt` block is added (after brodiffusion) all three are already targets, and brovisionml guards all three with `if(NOT TARGET ...)`. It backs the `bro.vision` JS bindings, image in (ImageBitmap / ImageData) → ImageBitmap + typed-array out. Like brodiffusion its CPU path is always built, so with `BRO_WITH_VISION` on the binding is real; it ships its own CUDA kernels gated on `BROTENSOR_WITH_CUDA`, so `-DBRO_WITH_TENSOR_CUDA=ON` compiles them automatically.

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

# Note: bromesh tests must be built in Release, meshoptimizer's Debug
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

Same shape for `bromath`, `qjsbind`, `htmlayout`, `broaudio`, `bromesh`, `broflora`, `brotensor`, `brogameagent`, `brolm`, `brodiffusion`, `broimage`, `brosoundml`, and `brovisionml`.

## Status, pull, sync across all sixteen repos

`scripts/repo-status.ps1` (Windows) and `scripts/repo-status.sh` (Linux/macOS) are the same tool in two ports. Run either from anywhere; both resolve paths from the script location.

```powershell
pwsh scripts/repo-status.ps1              # working-tree state + submodule-pointer drift
pwsh scripts/repo-status.ps1 -ListFiles   # also list changed files in dirty repos
pwsh scripts/repo-status.ps1 -Pull        # fast-forward everything first, then report
pwsh scripts/repo-status.ps1 -Pull -Sync  # ...and bump bro's stale pointers + commit
```

```bash
scripts/repo-status.sh              # -v / --verbose, -p / --pull, -s / --sync
scripts/repo-status.sh --pull
```

**`-Pull` / `--pull`** fast-forwards bro, broworkshop, and every sibling onto its upstream before the report, so what you read reflects the remotes rather than whatever you last fetched. Use it after a round of merges lands on GitHub (dependabot, PRs merged from the web) to bring the whole tree forward in one shot. It is deliberately conservative:

- `--ff-only`, so a repo that has diverged from its upstream is reported and skipped, never merged or rebased. Resolve those by hand.
- `-c pull.rebase=false`, because a repo configured to rebase on pull refuses outright when the tree is dirty — even for a pure fast-forward. Forcing the merge backend removes that false failure without ever allowing a real merge.
- `--no-recurse-submodules`, so pulling bro never drags `third_party/<name>` checkouts along. Pointer moves are `-Sync`'s job.
- Detached HEADs and branches with no upstream are reported and skipped.

**`-Sync` / `--sync`** then bumps bro's stale submodule pointers to the standalone HEADs and records them in a single bro commit. It only acts where the standalone is ahead of (or diverged from) the recorded pointer; a sibling whose standalone is *behind* bro is left alone, since that one needs a pull, not a bump. Note the ordering `-Pull -Sync` implies: pull first so the pointers you record are the real remote HEADs.

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
    -DBRODIFFUSION_DIR=/path/to/brodiffusion \
    -DBROIMAGE_DIR=/path/to/broimage \
    -DBROSOUNDML_DIR=/path/to/brosoundml \
    -DBROVISIONML_DIR=/path/to/brovisionml
```

Setting any `*_DIR` to a nonexistent path forces the submodule fallback:

```bash
cmake -B build -DBROMATH_DIR=none -DQJSBIND_DIR=none -DBROKIT_DIR=none \
               -DHTMLAYOUT_DIR=none -DBROAUDIO_DIR=none -DBROMESH_DIR=none \
               -DBROFLORA_DIR=none -DBROTENSOR_DIR=none -DBROGAMEAGENT_DIR=none \
               -DBROLM_DIR=none -DBRODIFFUSION_DIR=none -DBROIMAGE_DIR=none \
               -DBROSOUNDML_DIR=none -DBROVISIONML_DIR=none
```

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
