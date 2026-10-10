# The bro ecosystem

Every repository in the family: what each one is for, what it builds against, and where it runs. The same list in machine-readable form is [`scripts/repos.txt`](../scripts/repos.txt), which `scripts/repo-status.sh`, `scripts/repo-status.ps1` and `tests/run_tests.sh` read. Change both together.

**Where it is going.** The goal is a cross-platform desktop environment, on Windows and Linux at least, with apps written in HTML/CSS/JS on the bro runtime. On Linux bro drives the screen itself.

All repositories are at `github.com/wlejon/<name>` under the MIT license unless noted, and each is checked out beside bro at `../<name>`.

## The layers

```
apps          broworkshop · helm · helmapps · ffmpeg-bro    (tools: broparity)
                 │ run on / link
runtime       bro ──────────────────────────────┐
                 │ links                        │ runs all JavaScript through
engine libs   htmlayout brokit broaudio ...     bronze ── brass
terminal libs bropty brosearch brothemes bromux brolink
desktop libs  brovfs brosys brocas brocred broapps brothumb brodisplays
              brocompositor brokeys broa11y broconf broseat brodmabuf brodbus browl
              broportal brodecor broclip brompris bropulse broime brovideo broremote
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
| [broaudio](https://github.com/wlejon/broaudio) | Real-time audio engine: synthesis, effects, spatial mixing, MIDI, lock-free bus routing; device I/O on native PipeWire (Linux) or SDL3 | bromath, bronze, brass | `BRO_WITH_AUDIO` |
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

bro links them under `BRO_WITH_TERMINAL` for the `<terminal>` element. bropty, bromux and brolink have no JavaScript binding and do not depend on bro or bronze; brosearch and brothemes also build a `<name>_api` binding, mounted as `bro.search` (`BRO_WITH_SEARCH`) and `bro.themes` (`BRO_WITH_THEMES`).

| Repo | Role | Depends on | Platforms |
|------|------|------------|-----------|
| [bropty](https://github.com/wlejon/bropty) | Headless terminal: VT/xterm emulation, scrollback, reflow, inline images, Kitty keyboard protocol, PTY and ConPTY plumbing | brosearch | Windows, Linux, macOS |
| [brosearch](https://github.com/wlejon/brosearch) | Search: fzf-compatible fuzzy matching, rg-compatible file walking with gitignore rules, a linear-time Unicode regex engine for grep | none | Windows, Linux, macOS |
| [brothemes](https://github.com/wlejon/brothemes) | Colour schemes: import/export of terminal and editor theme formats, Oklab/Oklch, WCAG and APCA contrast. bro uses it for the terminal's minimum contrast | none | Windows, Linux, macOS |
| [bromux](https://github.com/wlejon/bromux) | Terminal multiplexer as a library: a server owns PTY sessions and their emulators, clients attach locally or over ssh. Backs persistent terminal sessions; optional in bro | bropty, brosearch, brolink, broimage (optional) | Windows, Linux, macOS |
| [brolink](https://github.com/wlejon/brolink) | Local IPC and ssh transport, platform APIs only: length-prefixed framing and LE/LEB128 wire helpers, a local listener and connector with peer-credential checks (AF_UNIX on Linux and macOS, named pipes with a user-only DACL on Windows), an event loop, byte streams over local connections, a spawned child's stdio (how ssh is run) and our own, the `proxy` relay (binary-clean `--pty` mode too), runtime paths, and lanes: a session as a bundle of connections joined with a one-time CSPRNG token. The transport under bromux, broremote and bro's agent control channel (`bro-ctl`) | none | Windows, Linux, macOS |

## Desktop-environment libraries (linked by bro)

Standalone C++20 libraries for the desktop environment. bro mounts their JavaScript bindings (`<name>_api`) under feature flags. Each has its own CMake and ctest suite.

| Repo | Role | Depends on | Platforms | bro gate |
|------|------|------------|-----------|----------|
| [brovfs](https://github.com/wlejon/brovfs) | File operations for a file manager: scanning, copy/move with undo, trash, volumes, file watching, MIME types | none | Windows, Linux, macOS | `BRO_WITH_VFS` |
| [brosys](https://github.com/wlejon/brosys) | System services: power, audio devices, network, notifications, the system tray | brodbus | Windows, Linux, macOS | `BRO_WITH_SYS` |
| [brocas](https://github.com/wlejon/brocas) | Content-addressed storage: BLAKE3, FastCDC chunking, Merkle DAG manifests, a sync wire protocol | none | Windows, Linux, macOS | — |
| [brocred](https://github.com/wlejon/brocred) | Credentials: OS secret stores, lock-screen password verification, biometric capability detection | brodbus | Windows, Linux, macOS | `BRO_WITH_CRED` |
| [broapps](https://github.com/wlejon/broapps) | Application catalog: installed apps, file associations, recent items, icons, scoped process launch | brovfs | Windows, Linux, macOS | `BRO_WITH_APPS` |
| [brothumb](https://github.com/wlejon/brothumb) | Thumbnails: the freedesktop cache, native OS extractors, PDF first pages, built-in generators | brovfs, broimage, bromath | Windows, Linux, macOS | `BRO_WITH_THUMB` |
| [brodisplays](https://github.com/wlejon/brodisplays) | Display configuration: enumeration, modes, scale, EDID, HDR, gamma and night light, hot-plug, test-then-revert | none | Windows, macOS, Linux (X11 and Wayland) | `BRO_WITH_DISPLAYS` |
| [brocompositor](https://github.com/wlejon/brocompositor) | Window management and compositing: a portable WM core, a shell over DWM on Windows, window tracking on macOS, a wlroots Wayland compositor on Linux whose output the host renders | brodisplays | Windows, Linux, macOS | `BRO_WITH_COMPOSITOR` |
| [brokeys](https://github.com/wlejon/brokeys) | Keybindings: chords and sequences, VS Code-style `when` clauses, layout-aware matching, VS Code import/export | brosearch | Windows, Linux, macOS | `BRO_WITH_KEYS` |
| [broa11y](https://github.com/wlejon/broa11y) | Accessibility tree with AT-SPI 2, UI Automation and NSAccessibility bridges | none | Windows, Linux, macOS | `BRO_WITH_A11Y` |
| [broconf](https://github.com/wlejon/broconf) | Desktop settings: a typed, schema'd store that notifies across processes | none | Windows, Linux, macOS | `BRO_WITH_CONF` |
| [broseat](https://github.com/wlejon/broseat) | Session and seat management: libseat / logind device access, systemd user session, XDG autostart, inhibitors | brodbus | Linux | `BRO_WITH_SEAT` |
| [brodmabuf](https://github.com/wlejon/brodmabuf) | GPU buffer sharing: DMA-BUF and DRM formats, GBM allocation, Vulkan external memory, DRM sync objects | none | Linux | — |
| [brodbus](https://github.com/wlejon/brodbus) | D-Bus layer: connection setup with private bus Hello, signal matching, property caching, message containers, and test fixtures | none | Linux (Windows and macOS build stubs that report unavailable) | — |
| [browl](https://github.com/wlejon/browl) | Wayland shell-protocol client: layer shell, session lock, foreign toplevels, screencopy, idle inhibit | none | Linux | `BRO_WITH_WL` |
| [broportal](https://github.com/wlejon/broportal) | xdg-desktop-portal backend: file chooser, screenshot, screencast, remote desktop, settings, global shortcuts | brodbus | Linux | `BRO_WITH_PORTAL` |
| [brodecor](https://github.com/wlejon/brodecor) | Window decorations: SSD frames, captions, buttons, 9-slice blur drop shadows, and hit-testing | bromath | Windows, Linux, macOS | `BRO_WITH_DECOR` |
| [broclip](https://github.com/wlejon/broclip) | Clipboard history and data control: ring store, MIME stream caching; Wayland data-control client, Win32 clipboard, macOS pasteboard | none | Windows, Linux, macOS | `BRO_WITH_CLIP` |
| [brompris](https://github.com/wlejon/brompris) | Media player controller: MPRIS2 on Linux | brodbus | Linux (Windows and macOS build, with no backend yet: no players) | `BRO_WITH_MPRIS` |
| [bropulse](https://github.com/wlejon/bropulse) | Audio routing and policy: native PipeWire 0.3 stream graph and PulseAudio fallback | none | Linux (Windows and macOS build the graph and policy, with no audio server) | `BRO_WITH_PULSE` |
| [broime](https://github.com/wlejon/broime) | Input methods: compose key sequences, dead keys, candidate popup placement, dictionary prefix trie | brosearch | Windows, Linux, macOS | `BRO_WITH_IME` |
| [brovideo](https://github.com/wlejon/brovideo) | Hardware video encode and decode through platform APIs only: a VA-API encoder (H.264, HEVC, AV1; dmabuf with its acquire fence, or CPU frames), a Media Foundation decoder (CPU NV12 or D3D11 textures), the Raw codec, and a capability probe. broremote's codec layer, with no remoting concepts, so `<video>`, `VideoEncoder` and screen recording can use it later | brodmabuf (Linux) | Linux (encode), Windows (decode) | `BRO_WITH_REMOTE` |
| [broremote](https://github.com/wlejon/broremote) | Remote sessions: a server the host feeds composited frames (dmabuf or CPU) and drains input from, encoding through brovideo, and the viewer's session with no window (`ViewerSession`, shown by bro's `<remoteview>`; `broremote probe` for scripted checks) that reaches it locally or over ssh. Backs `bro.remote` and `helm --remote` | brovideo, brolink (its JS binding: bronze, brass) | Linux (hosting with VA-API), Windows (viewer; Raw-only hosting for development) | `BRO_WITH_REMOTE` |

## Apps and tools

| Repo | Role | Depends on | Platforms |
|------|------|------------|-----------|
| [broworkshop](https://github.com/wlejon/broworkshop) | The launcher and starter apps (games, tools, demos, AI labs) that show what the engine does. Run by bro, not built against it | bro | Windows, Linux, macOS |
| [helmapps](https://github.com/wlejon/helmapps) | The helm desktop's core apps, one bro folder app each (no native code, no build step), starting with helmterm, the terminal over bro's `<terminal>` element | bro (run by the stock `bro`) | Windows, Linux, macOS |
| [helm](https://github.com/wlejon/helm) | The desktop environment shell: top panel, status popups, spotlight fuzzy launcher, notification center, and session lock screen. Standalone executable linking bro_engine | bro | Windows, Linux, macOS |
| [ffmpeg-bro](https://github.com/wlejon/ffmpeg-bro) | A GUI for ffmpeg: in-process playback, a timeline, a filtergraph editor, exports. GPLv3: it links bro, and bro never links GPL code | bro | Windows, Linux, macOS |
| [broparity](https://github.com/wlejon/broparity) | Rendering parity between bro and Chromium: pixel and layout-tree diffs over small HTML cases ([live report](https://wlejon.github.io/broparity/)) | bro (`bro-headless`) | Windows, Linux, macOS |

How dependencies resolve, how to push, and the release lock: [multi-repo-workflow.md](multi-repo-workflow.md).
