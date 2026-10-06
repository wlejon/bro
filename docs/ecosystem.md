# The bro ecosystem

bro is one repository among many. This page lists all of them: what each one is for, what it builds against, and where it runs. It also describes the dependency convention they share. The same list in machine-readable form is [`scripts/repos.txt`](../scripts/repos.txt), which `scripts/repo-status.sh`, `scripts/repo-status.ps1` and `tests/run_tests.sh` read. Change both together.

**Where it is going.** The goal is a cross-platform desktop environment, on Windows and Linux at least, with apps written in HTML/CSS/JS on the bro runtime. On Linux bro drives the screen itself. The current milestone is a terminal in bro good enough to run Claude Code: the `<terminal>` element ([terminal-api.js](terminal-api.js)) and [broterm](https://github.com/wlejon/broterm), the first app of the desktop environment. What is done and what is open: [desktop-roadmap.md](desktop-roadmap.md).

All repositories are at `github.com/wlejon/<name>` under the MIT license unless noted, and each is checked out beside bro at `../<name>`.

## The layers

```
apps          broworkshop · broterm · ffmpeg-bro            (tools: broparity)
                 │ run on / link
runtime       bro ──────────────────────────────┐
                 │ links                        │ runs all JavaScript through
engine libs   htmlayout brokit broaudio ...     bronze ── brass
terminal libs bropty brosearch brothemes bromux
                                                 (bro does not link these yet)
desktop libs  brovfs brosys brocas brocred broapps brothumb brodisplays
              brocompositor brokeys broa11y broconf broseat brodmabuf browl broportal
```

## Runtime and compiler

| Repo | Role | Depends on | Platforms |
|------|------|------------|-----------|
| [bro](https://github.com/wlejon/bro) | The app runtime: HTML/CSS/JS apps on a GPU (Vulkan 1.3) engine with 3D, physics, audio, a native terminal element and on-device AI | every engine and terminal library, bronze, brass | Windows, Linux, macOS |
| [bronze](https://github.com/wlejon/bronze) | JavaScript compiler and shared runtime: AOT to native modules, plus in-process JIT. bro runs every app's script through it | brass | Windows x64, Linux x64, macOS arm64 |
| [brass](https://github.com/wlejon/brass) | Code-generation backend and runtime for GC'd dynamic languages: MIR, JIT/AOT for x86-64 and AArch64, moving GC | none | Windows, Linux, macOS |

## Engine libraries (linked by bro)

Each has a JavaScript binding, `<name>_api` under its own `src/api/`, that bro mounts (bromath and htmlayout have none). Those bindings need bronze and brass beside the library.

| Repo | Role | Depends on | bro gate |
|------|------|------------|----------|
| [bromath](https://github.com/wlejon/bromath) | Header-only math: vectors, quaternions, matrices, transforms, colour, curves, RNG/hash, spatial hash | none | always |
| [htmlayout](https://github.com/wlejon/htmlayout) | HTML5 parsing, CSS cascade and selectors, block/inline/flex/grid layout, hit testing | none | always |
| [brokit](https://github.com/wlejon/brokit) | Web-standard and Node-style system APIs: fetch, streams, storage, fs, crypto, child_process | bromath, broimage (codecs), bronze, brass | always |
| [broimage](https://github.com/wlejon/broimage) | Image decode/encode, geometric and colour ops, composable typed-buffer kernels, ML preprocessing | bromath, brotensor (optional), bronze, brass | always |
| [broaudio](https://github.com/wlejon/broaudio) | Real-time audio engine: synthesis, effects, spatial mixing, MIDI, lock-free bus routing on SDL3 | bromath, bronze, brass | `BRO_WITH_AUDIO` |
| [bromesh](https://github.com/wlejon/bromesh) | Mesh generation, CSG, simplification, rigging, glTF/FBX/STL I/O | bromath, bronze, brass | `BRO_WITH_3D` |
| [broflora](https://github.com/wlejon/broflora) | Plant ecosystem simulation emitting branch and foliage geometry | bromath, bromesh, bronze, brass | `BRO_WITH_FLORA` |
| [brotensor](https://github.com/wlejon/brotensor) | One tensor type, device-neutral ops including training; CPU always, CUDA / Metal / Vulkan optional | bronze, brass | `BRO_WITH_TENSOR` |
| [brogameagent](https://github.com/wlejon/brogameagent) | Game AI: navigation, steering, perception, MCTS planners, an autograd-free NN stack | bromath, brotensor (NN half), bronze, brass | `BRO_WITH_GAMEAI` |
| [brolm](https://github.com/wlejon/brolm) | Text-model inference: tokenizers, CLIP/T5 encoders, LLMs from safetensors and GGUF | bromath, brotensor, broimage, bronze, brass | `BRO_WITH_LM` |
| [brodiffusion](https://github.com/wlejon/brodiffusion) | Diffusion inference: text-to-image, image-to-3D, text-to-motion, terrain | bromath, brotensor, brolm, broimage, brovisionml, bronze, brass | `BRO_WITH_DIFFUSION` |
| [brosoundml](https://github.com/wlejon/brosoundml) | Audio-ML inference: TTS, STT, diarization, RAVE, keyword spotting, wake words | bromath, brotensor, broimage (for brolm), brolm, broaudio, bronze, brass | `BRO_WITH_SOUNDML` |
| [brovisionml](https://github.com/wlejon/brovisionml) | Vision-ML inference: segmentation, depth, normals, matting, pose, ControlNet annotators | bromath, brotensor, broimage, bronze, brass | `BRO_WITH_VISION` |

All of them build and test on Windows, Linux and macOS.

## Terminal libraries (linked by bro)

These have no JavaScript binding and do not depend on bro or bronze. bro links them under `BRO_WITH_TERMINAL` for the `<terminal>` element.

| Repo | Role | Depends on | Platforms |
|------|------|------------|-----------|
| [bropty](https://github.com/wlejon/bropty) | Headless terminal: VT/xterm emulation, scrollback, reflow, inline images, Kitty keyboard protocol, PTY and ConPTY plumbing | brosearch | Windows, Linux, macOS |
| [brosearch](https://github.com/wlejon/brosearch) | Search: fzf-compatible fuzzy matching, rg-compatible file walking with gitignore rules, a linear-time Unicode regex engine for grep | none | Windows, Linux, macOS |
| [brothemes](https://github.com/wlejon/brothemes) | Colour schemes: import/export of terminal and editor theme formats, Oklab/Oklch, WCAG and APCA contrast. bro uses it for the terminal's minimum contrast | none | Windows, Linux, macOS |
| [bromux](https://github.com/wlejon/bromux) | Terminal multiplexer as a library: a server owns PTY sessions and their emulators, clients attach locally or over ssh. Backs persistent terminal sessions; optional in bro | bropty, brosearch, broimage (optional) | Windows, Linux, macOS |

## Desktop-environment libraries (not linked by bro yet)

Standalone C++20 libraries for the desktop environment. None depends on bro or bronze, and none has a JavaScript binding yet. Each has its own CMake and ctest suite.

| Repo | Role | Depends on | Platforms |
|------|------|------------|-----------|
| [brovfs](https://github.com/wlejon/brovfs) | File operations for a file manager: scanning, copy/move with undo, trash, volumes, file watching, MIME types | none | Windows, Linux, macOS |
| [brosys](https://github.com/wlejon/brosys) | System services: power, audio devices, network, notifications, the system tray | none | Windows, Linux, macOS |
| [brocas](https://github.com/wlejon/brocas) | Content-addressed storage: BLAKE3, FastCDC chunking, Merkle DAG manifests, a sync wire protocol | none | Windows, Linux, macOS |
| [brocred](https://github.com/wlejon/brocred) | Credentials: OS secret stores, lock-screen password verification, biometric capability detection | none | Windows, Linux, macOS |
| [broapps](https://github.com/wlejon/broapps) | Application catalog: installed apps, file associations, recent items, icons, scoped process launch | brovfs | Windows, Linux, macOS |
| [brothumb](https://github.com/wlejon/brothumb) | Thumbnails: the freedesktop cache, native OS extractors, PDF first pages, built-in generators | brovfs, broimage, bromath | Windows, Linux, macOS |
| [brodisplays](https://github.com/wlejon/brodisplays) | Display configuration: enumeration, modes, scale, EDID, HDR, gamma and night light, hot-plug, test-then-revert | none | Windows, macOS, Linux (X11 and Wayland) |
| [brocompositor](https://github.com/wlejon/brocompositor) | Window management and compositing: a portable WM core, a shell over DWM on Windows, window tracking on macOS, a wlroots Wayland compositor on Linux whose output the host renders | brodisplays | Windows, Linux, macOS |
| [brokeys](https://github.com/wlejon/brokeys) | Keybindings: chords and sequences, VS Code-style `when` clauses, layout-aware matching, VS Code import/export | brosearch | Windows, Linux, macOS |
| broa11y | Accessibility tree with AT-SPI 2, UI Automation and NSAccessibility bridges | none | Windows, Linux, macOS |
| broconf | Desktop settings: a typed, schema'd store that notifies across processes | none | Windows, Linux, macOS |
| broseat | Session and seat management: libseat / logind device access, systemd user session, XDG autostart, inhibitors | none | Linux |
| brodmabuf | GPU buffer sharing: DMA-BUF and DRM formats, GBM allocation, Vulkan external memory, DRM sync objects | none | Linux |
| browl | Wayland shell-protocol client: layer shell, session lock, foreign toplevels, screencopy, idle inhibit | none | Linux |
| broportal | xdg-desktop-portal backend: file chooser, screenshot, screencast, remote desktop, settings, global shortcuts | none | Linux |

broa11y, broconf, broseat, brodmabuf, browl and broportal are not on GitHub yet; their links will be `github.com/wlejon/<name>` once they are.

## Apps and tools

| Repo | Role | Depends on | Platforms |
|------|------|------------|-----------|
| [broworkshop](https://github.com/wlejon/broworkshop) | The launcher and starter apps (games, tools, demos, AI labs) that show what the engine does. Run by bro, not built against it | bro | Windows, Linux, macOS |
| [broterm](https://github.com/wlejon/broterm) | The terminal app: tabs, splits, profiles, shell integration, inline images. Its own executable linking the bro engine; the first app of the desktop environment | bro | Windows, Linux, macOS |
| [ffmpeg-bro](https://github.com/wlejon/ffmpeg-bro) | A GUI for ffmpeg: in-process playback, a timeline, a filtergraph editor, exports. GPLv3: it links bro, and bro never links GPL code | bro | Windows, Linux, macOS |
| [broparity](https://github.com/wlejon/broparity) | Rendering parity between bro and Chromium: pixel and layout-tree diffs over small HTML cases ([live report](https://wlejon.github.io/broparity/)) | bro (`bro-headless`) | Windows, Linux, macOS |

## The dependency convention

Every repo here that builds against another resolves it the same way, in this order:

1. **An existing target wins.** If a superbuild such as bro has already added `bropty`, bromux's lookup finds that target and adds nothing. That way a library is configured once per build, and the first loader picks its options. bro is the first loader of brotensor, brosearch and the others for this reason.
2. **The sibling checkout**, at `../<name>` beside the top-level project, overridable with `-D<NAME>_DIR=<path>`. This is the development layout: you edit the standalone repo and every consumer builds from it, with no copy to keep in sync.
3. **The `third_party/<name>` submodule** of the top-level project. This is what CI and a fresh `git clone --recursive` use.

The submodule fallback is **flat**: a repo's own `third_party/` carries every ecosystem repo it needs, transitive ones included, side by side rather than nested. bromux carries bropty *and* brosearch; brothumb carries brovfs, broimage and bromath. Submodule URLs are `https://github.com/wlejon/<name>.git`, so a recursive clone works without SSH keys.

**bronze and brass are the exception.** A library with a JavaScript binding needs `../bronze` and `../brass` checked out beside it and has no submodule for either; its CI checks them out with bronze's `checkout-toolchain` action. bro itself does carry `third_party/bronze` and `third_party/brass`.

**Submodule pointers move at the end of a session, not per commit.** The repo owner records them in one bro commit with `scripts/repo-status.sh --sync`. [multi-repo-workflow.md](multi-repo-workflow.md) has bro's side in detail: the configure order, the feature gates, the `<name>_api` bindings, and the status/pull/sync/push tool.

## Working across the repos

```bash
scripts/repo-status.sh            # every repo in scripts/repos.txt: branch, dirty, ahead/behind,
                                  # then bro's submodule pointers against the ../<name> HEADs
scripts/repo-status.sh --verbose  # also list changed files
pwsh scripts/repo-status.ps1      # the same tool on Windows (-ListFiles, -Pull, -Sync, -Push)
```

A repo that isn't checked out is listed as such and skipped. A repo tracking an upstream that isn't on `origin` shows it in brackets.
