# Desktop environment roadmap

**North star:** a cross-platform desktop environment, as complete as we can manage, built on bro: Windows and Linux at least. On Linux, bro drives the screen itself (DRM/KMS, libinput, logind, XWayland). Desktop apps are their own repos; broterm is the first.

**Current milestone:** a terminal in bro good enough to run Claude Code (`<terminal>` in bro and [broterm](https://github.com/wlejon/broterm)).

Every repo is listed in [ecosystem.md](ecosystem.md). This page records where things stand and what is left. Last updated 2026-10-06.

## Done

- **bro `<terminal>`** is a native replaced element painted by bro, on its own compositor layer, over bropty. It covers keys, IME and paste; mouse selection and reporting; scrollback, search and links; OSC title/cwd/bell/notification/progress/52/133/99/22 events; inline images (kitty, sixel, iTerm2); the foreground process; an `activity` event; the effective palette; runtime scrollback and cursor options; and persistent sessions through bromux (protocol 2.1 carries the same extras). API: [terminal-api.js](terminal-api.js).
- **broterm** has tabs and splits, profiles, settings, a command palette and keybindings. It also has find, links, a clipboard-read policy and paste safety. Shell integration for pwsh, Windows PowerShell, bash, zsh and fish provides command marks, prompt navigation, durations, re-run, a sticky header and long-command notifications. Titles follow the foreground process, and persistent sessions reattach on restart. 36 tests pass on Windows and Linux.
- **Desktop substrate libraries**, all public with CI:
  - Linux session: broseat, brodmabuf, brodbus, browl
  - Display and shell: brodisplays, brocompositor
  - Files and apps: brovfs, broapps, brothumb, brocas
  - System and credentials: brosys, brocred
  - Look and input: brothemes, brokeys, broa11y
  - Settings and portals: broconf, broportal
- **JS bindings for desktop libraries**: all `<sibling>_api` bindings completed with trust model and host integration.
- **Desktop compositor (Milestones 1–5)**:
  - Milestone 1: Nested Wayland compositor in bro (`brocompositor` + `brodmabuf` Vulkan image import with explicit sync and fences compositing into `VulkanPresenter`).
  - Milestone 2: bro owns the screen (`KmsDirectPresenter` atomic modesetting through DRM/KMS scanout imported into Vulkan, seat management via `broseat`/`logind`, input via `libinput`, VT switching pause/resume, `--drm` CLI mode).
  - Milestone 3: Shell surfaces and window management (`xdg-shell` WM focus/placement/state, `wlr-layer-shell` panels/docks ordering, `ext-session-lock-v1` isolated display).
  - Milestone 4: XWayland lazy lifecycle, X11 client window management, and X11 <-> Wayland clipboard/selection bridging.
  - Milestone 5: Daily-driver gaps (multi-monitor hotplug & layout, fractional scaling via `wp_fractional_scale_manager_v1` / viewporter, per-frame damage tracking, text-input-v3 / input-method-v2, screencopy via `wlr-screencopy-v1` / `ext-image-copy-capture-v1`).
- **Session and app model**: process launch and tracking (`broapps`), cross-platform single-instance with argument forwarding (`bro.window.requestSingleInstance`), autostart (`broseat`), unified D-Bus layer (`brodbus`), and desktop trust boundary permissions.
- **One D-Bus layer (`brodbus`)**: unified C++20 library owning connection setup (with private bus Hello), signal matching, message container serialization, property caching, error mapping, and private test fixtures. Adopted across `broseat`, `brocred`, `brosys`, and `broportal`.
- **[DONE] Comprehensive README pass**: across all 5 terminal libraries (bropty, bromux, brosearch, brothemes, brokeys), 14 desktop libraries (brovfs, brosys, brocas, brocred, broapps, brothumb, brodisplays, brocompositor, broseat, brodmabuf, browl, broa11y, broconf, broportal), and brodbus.
- **Terminal milestone: Claude Code in broterm** is complete:
  - broterm adopted all element APIs (`activity`, `palette`, clean `cwd` paths, `options.scrollback`/`cursorStyle`/`cursorBlink`, and `bracketedPaste`), completely replacing background polling.
  - `bro.window` desktop features (desktop notifications, taskbar progress, flash/attention, system bell sound, focus state & events, title/opacity/fullscreen, single-instance with argument forwarding, global hotkeys, and system tray) implemented in `bro` engine, and broterm's stand-ins retired.
  - Acceptance tests for Claude Code passing in broterm.
  - macOS test port completed with platform `Cmd` bindings and bash < 4.4 DEBUG-trap fallback.
- **CI and CodeQL across ecosystem:** 100% green on `main` across all repositories (`bro`, `broterm`, `bropty`, `bromux`, `broaudio`, `browl`). Fixed bro Linux/macOS headless Vulkan driver/display handling, Windows C++/WinRT version unification between brocred/brocompositor and libremidi, broterm Linux UI commands test timing, broaudio coverage assertion, browl sway shell configure/reposition tests, and closed or dismissed all CodeQL alerts (0 open alerts ecosystem-wide).
- **Organization:** ecosystem index, `scripts/repos.txt`, repo-status scripts over every repo, the flat `third_party` submodule convention with https URLs, and a submodule-fallback CI job in each library.

## Terminal milestone known limits

- Some Windows conhost builds drop terminal replies (documented in bropty).
- Persistent sessions ignore the client's scrollback limit and image quota.
- Only iTerm2 images survive ConPTY.

## Open: CI and hygiene

**README pass:** Completed across all 5 terminal and 14 desktop libraries plus brodbus.

**Self-hosted Linux runner for hardware jobs:** real-GPU Vulkan, seat and DRM. Put it on jserve, which has Docker; halo has none. Use ephemeral containers, run on pushes to main only, never on fork PRs. Hosted runners stay for Windows and macOS.

**Agents:** verify locally first, on halo or a Docker image matching the runner on jserve. Check CI once per push instead of polling. Several agents polling the GitHub API exhaust its rate limit.

## Open: foundational pieces for the desktop

1. **Accessibility tree platform verification:** the engine-side DOM export into `broa11y::Tree` is implemented and verified in bro. Platform-specific AT-SPI/UIA/NSAccessibility screen reader validation remains.
2. **Shell apps on top:**
   - panel, launcher and taskbar (broapps, brocompositor's foreign-toplevel)
   - notification centre (brosys)
   - file manager (brovfs, brothumb)
   - settings (broconf, brodisplays, brothemes)
   - lock screen and greeter (session lock, brocred authentication)
3. **Packaging and install:**
   - desktop installation, package managers, and update pipeline on Windows and Linux
   - Linux session desktop entry registration for display managers and bro greeter
   - installation into trusted desktop locations
