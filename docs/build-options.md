# Build options

The profiles and `BRO_WITH_*` flags. Quickstart: [BUILDING.md](../BUILDING.md).

A flag that is off leaves its sibling unbuilt, compiles its code out behind
`#if BRO_WITH_*`, and installs its JS namespace as the `{ available: false }`
stub. Only `BRO_WITH_NET` (GameNetworkingSockets) and `BRO_WITH_VIDEO`
(libvpx/webm/Opus) need vcpkg; only `BRO_WITH_TENSOR_CUDA` needs the CUDA
toolkit.

## Profiles (presets)

`-DBRO_PROFILE=<name>` seeds the individual flag defaults. Individual
`-DBRO_WITH_*` flags always override the profile.

| Profile | What it is | Needs vcpkg? | Needs CUDA? |
|---|---|---|---|
| `minimal` | HTML/CSS + Canvas2D + WebGL + audio. 2D renderer only. | no | no |
| **`app`** (default) | Full renderer (3D scene graph, physics, audio, core game-AI), terminal, desktop libraries, remote + net/video/steam. No AI tower. | yes | no |
| `full` | Everything except the CUDA sub-lever (still opt-in). | yes | no (opt-in) |

```bash
cmake -B build                                 # app profile (default), needs vcpkg for net/video
cmake -B build -DBRO_PROFILE=full              # everything (adds the AI tower)
cmake -B build -DBRO_PROFILE=app -DBRO_WITH_LM=ON   # app + language models (adds brolm+brotensor)
```

## Flags

### Tier 0: CORE (always on, no flag)

`util · platform · render · svg · layout · dom · canvas · webgl · engine ·
headless` + Skia · SDL (Vulkan) · Vulkan 1.3 (headers + loader; MoltenVK on
macOS) · glslang (in-process GLSL → SPIR-V, built from source) · brokit · htmlayout ·
**broimage (tensor-free)** · brolink (dependency-free; the agent control channel and
`bro-ctl`, [agent-control.md](agent-control.md)). A complete
HTML/CSS + Canvas2D + WebGL runtime with working screenshots and native Vulkan presentation.

### Tier 1: feature groups (brotensor-free)

| Flag | Pulls in | `minimal` | `app` | `full` | Notes |
|---|:--|:--:|:--:|:--:|---|
| `BRO_WITH_3D` | bromesh + scene graph + mesh/rigging/terrain/tile/gizmo subsystems | off | on | on | 3D node types embed `bromesh` by value |
| `BRO_WITH_PHYSICS` | Jolt | off | on | on | header-isolated behind `physics::PhysicsWorld` |
| `BRO_WITH_AUDIO` | broaudio + audio_inference | **on** | on | on | self-contained, no vcpkg; on in every profile because `<audio>`/Web Audio is core HTML |
| `BRO_WITH_GAMEAI` | brogameagent **core** (nav/path/steer/MCTS) | off | on | on | brotensor-free |
| `BRO_WITH_FLORA` | broflora | off | on | on | needs `3D` (bromesh) |
| `BRO_WITH_TEXT_SHAPING` | HarfBuzz + Skia's UAX#9 ICU bidi subset + `modules/skunicode` | **on** | on | on | no vcpkg, no Skia rebuild: compiled from the Skia source bundle. On in *every* profile so there is one text path, not two. Off = 1:1 codepoint→glyph (no ligatures, kerning or Arabic joining). See [third_party/skia/BUNDLE.md](../third_party/skia/BUNDLE.md) |
| `BRO_WITH_WEBP` | libwebp decoder | **on** | on | on | compiled from the Skia source bundle alongside HarfBuzz, so `.webp` decodes the same on every platform. Off = `.webp` does not decode anywhere |
| `BRO_WITH_NET` | GameNetworkingSockets | off | on | on | **needs vcpkg** |
| `BRO_WITH_VIDEO` | libvpx/webm/Opus | off | on | on | **needs vcpkg** |
| `BRO_WITH_STEAM` | none (runtime dlopen) | off | on | on | the stub template |
| `BRO_WITH_TERMINAL` | bropty + brosearch + brothemes + bromux (persistent sessions, over brolink) | off | on | on | no vcpkg; the native `<terminal>` element ([terminal-api.js](terminal-api.js)). Off = `bro.terminal.available === false` and `<terminal>` is an inert box |
| `BRO_WITH_REMOTE` | broremote + brovideo + brolink | off | on | on | `bro.remote` ([remote-api.js](remote-api.js)). Windows and Linux only (off by default on macOS). brovideo's VA-API encoders on Linux when libva is present (with brodmabuf). Off = `bro.remote.available === false` |

### Desktop libraries

Each desktop sibling has its own flag, on in `app` and `full`, off in
`minimal`; off, its namespace is the `{ available: false }` stub. The
privileged ones are further gated at run time ([apps.md](apps.md#permissions)).

| Flag | Library | Namespace | Platforms (default on) |
|---|---|---|---|
| `BRO_WITH_CONF` | broconf | `bro.conf` | all |
| `BRO_WITH_THEMES` | brothemes | `bro.themes` | all |
| `BRO_WITH_KEYS` | brokeys | `bro.keys` | all |
| `BRO_WITH_SEARCH` | brosearch | `bro.search` | all |
| `BRO_WITH_APPS` | broapps | `bro.apps` | all |
| `BRO_WITH_VFS` | brovfs | `bro.vfs` | all |
| `BRO_WITH_THUMB` | brothumb | `bro.thumb` | all |
| `BRO_WITH_CRED` | brocred | `bro.cred` | all |
| `BRO_WITH_SYS` | brosys | `bro.sys` | all |
| `BRO_WITH_DISPLAYS` | brodisplays | `bro.displays` | all |
| `BRO_WITH_COMPOSITOR` | brocompositor | `bro.compositor` | all |
| `BRO_WITH_A11Y` | broa11y | `bro.a11y` | all |
| `BRO_WITH_DECOR` | brodecor | `bro.decor` | all |
| `BRO_WITH_CLIP` | broclip | `bro.clip` | all |
| `BRO_WITH_MPRIS` | brompris | `bro.mpris` | all |
| `BRO_WITH_PULSE` | bropulse | `bro.pulse` | all |
| `BRO_WITH_IME` | broime | `bro.ime` | all |
| `BRO_WITH_SEAT` | broseat | `bro.seat` | Linux |
| `BRO_WITH_WL` | browl | `bro.wl` | Linux |
| `BRO_WITH_PORTAL` | broportal | `bro.portal` | Linux |
| `BRO_WITH_DMABUF` | brodmabuf | (C++ only: dmabuf import, KMS) | Linux |

### Tier 2: the AI tower (brotensor is the base)

| Flag | Pulls in | `full` | Requires |
|---|:--|:--:|---|
| `BRO_WITH_TENSOR` | brotensor (CPU); `bro.gpu`, and `bro.tensor` only with a GPU backend (below) | on | none |
| `BRO_WITH_TENSOR_CUDA` | brotensor CUDA backend | **off** | `TENSOR` + CUDA toolkit |
| `BRO_WITH_TENSOR_METAL` | brotensor Metal backend | **off** | `TENSOR` + macOS |
| `BRO_WITH_TENSOR_VULKAN` | brotensor Vulkan compute backend (hand-written GLSL); the AMD GPU path | **auto/on** (Linux + glslc + Vulkan headers + loader) | `TENSOR` + glslc (shaderc) + Vulkan headers |
| `BRO_WITH_LM` | brolm | on | `TENSOR` |
| `BRO_WITH_DIFFUSION` | brodiffusion | on | `LM` (text encoder) |
| `BRO_WITH_VISION` | brovisionml | on | `TENSOR` |
| `BRO_WITH_SOUNDML` | brosoundml (stt/tts/diar/wake/kws/sense/gesture/rave) | on | `TENSOR` + `LM` (stt tokenizer) + `AUDIO` (mic taps) |
| `BRO_WITH_TRIPOSPLAT` | triposplat pipeline | on | `VISION` + `DIFFUSION` + `3D` |
| `BRO_WITH_GAMEAI_NN` | brogameagent `nn/*` + `learn/*` | on | `GAMEAI` + `TENSOR` |

`BRO_WITH_TENSOR_CUDA` stays **off even in `full`**. brotensor auto-detects
the CUDA toolkit when the flag is off, so a genuinely CUDA-free build also
passes `-DBROTENSOR_WITH_CUDA=OFF -DBROGAMEAGENT_WITH_CUDA=OFF`.

`BRO_WITH_TENSOR_VULKAN` is the AMD GPU path (there is no HIP backend) and can
sit beside CUDA, which then stays the default device. On Linux it is
auto-detected on a build directory's first configure when `glslc`, the Vulkan
headers and the loader are present; `-DBRO_WITH_TENSOR_VULKAN=ON/OFF` decides
it explicitly. Without CUDA or Metal, `bro.gpu.backend === "vulkan"`, and
every ML loader takes `{ device: 'vulkan' }` (alias `'vk'`).

**`bro.tensor` needs one of those GPU backends, and `bro.gpu` does not.** The
tensor surface is built on brotensor's GPU tensor type, which only exists with
CUDA, Metal or Vulkan compiled in, so `BRO_WITH_TENSOR=ON` on its own gives you
`bro.tensor.available === false` and the usual unavailable-namespace error
naming the backend rather than the flag. `bro.gpu` is the runtime probe and
stays real either way, answering `cpu`. Everything else in the tower —
`bro.lm`, `bro.diffusion`, `bro.vision`, the soundml family — has a CPU path
and is real with the flag alone; GPU acceleration is backend-dependent.

### Tier 3: outside the profiles

These three are not seeded by `BRO_PROFILE`; each answers a question the
feature flags do not.

| Flag | Default | What it does |
|---|:--:|---|
| `BRO_BUILD_EXECUTABLES` | ON top-level, **OFF** under `add_subdirectory` | Builds `bro` / `bro-headless` / `bro-server`. An embedder linking `bro_engine` with its own `main` wants the libraries, not a second `bro.exe` in its tree, and gets that without asking. See [embedding.md](embedding.md). |
| `BRO_BUILD_TESTS` | ON top-level, **OFF** under `add_subdirectory` | Builds bro's own C++ tests and smoke tools (`bro_*_test`, `bro_videoinspect`, `bro_videoencodetest`, `bro_mediabackendtest`, `bro_mediaclocktest`, ...). They test bro, not the application embedding it, so an embedder's build never compiles or links them. The JS suite under `tests/` is separate and runs on `bro-headless`. |
| `BRO_WITH_BRONZE` | **ON (mandatory)** | The JavaScript host layer (`src/bronze_host`). The `bronze` compiler is not built by default: `cmake --build build --target bronze-cli` (not `bronze`, which the Visual Studio generator leaves out of the solution). |

## Dependency auto-enable

Enabling a flag force-enables its prerequisites (with a status message), so an
inconsistent combo like `DIFFUSION` without `LM` is impossible. Resolution runs
once, after the profile is applied and before `add_subdirectory(third_party)`
(`_bro_require` in the top-level `CMakeLists.txt`):

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

## Skia: orthogonal but required

Skia is core (dom/canvas/render need it) and is **not** a `BRO_WITH_` flag.
`third_party/skia/{src,include,lib}` are gitignored.

**Auto-fetched** (`third_party/skia/skia.cmake`): on first configure,
`file(DOWNLOAD)` + SHA-256-verify pulls the headers/source bundle and the Release
library, pinned to Skia `chrome/m147`, so lib and headers always match, from
the repo's GitHub releases, and the Release lib is used for all configs (Debug
included). Prebuilt libs are hosted for Windows x64, Linux x64, and macOS arm64;
Intel macOS and a Windows Debug lib still fall back to the `build_skia_*.sh`
scripts. Set `-DBRO_FETCH_SKIA=OFF` to opt out.

## Adding a module later

`-DBRO_WITH_LM=ON` + rebuild reconfigures, compiles only brolm (+ brotensor) and
the host interfaces, and **relinks** `bro`/`bro-headless`. As long as the build
dir is intact, that is incremental, not a from-scratch rebuild.

Caveat on presets: `-DBRO_PROFILE` seeds flag defaults on **first** configure via
`option()`; individual `-D` flags (already in the cache) win. Switching profile on
an existing build dir won't move flags already cached. Clear the specific
`BRO_WITH_*` cache entries (or reconfigure fresh) to re-baseline.
