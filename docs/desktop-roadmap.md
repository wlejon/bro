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
- **Nested Wayland compositor in bro**: `brocompositor` + `brodmabuf` Vulkan image import with explicit sync and fences compositing into `VulkanPresenter`. Headless test client screenshot oracle verified.
- **bro owns the screen (DRM/KMS compositor output)**: KMS atomic modesetting through `brodmabuf` and `KmsDirectPresenter`, scanout buffers imported into Vulkan for composited presentation, seat management via `broseat`/`logind`, input event dispatch via `libinput`, and VT switching handling (pause & resume) with `--drm` CLI flag support. Verified with native test `bro_drm_screen_test`.
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

1. **bro on Linux as the compositor:** Milestones 1, 2, and 3 (nested compositor, bro owning the screen via KMS/broseat/libinput, and shell surfaces/window management/layer-shell/session-lock) are complete. Open: Milestone 4 (XWayland), and Milestone 5 (daily-driver gaps).
2. **An accessibility tree out of bro:** the engine-side DOM export into `broa11y::Tree` is implemented and verified. Platform-specific AT-SPI/UIA/NSAccessibility testing remains.
3. **Shell apps on top:**
   - panel, launcher and taskbar (broapps, brocompositor's foreign-toplevel)
   - notification centre (brosys)
   - file manager (brovfs, brothumb)
   - settings (broconf, brodisplays, brothemes)
   - lock screen and greeter (session lock, brocred authentication)
4. **brocompositor gap audit** against what a daily-driver session needs: protocols, multi-monitor, HiDPI, damage, input methods, clipboard and drag-and-drop across clients.
5. **Session and app model:** launching and tracking DE apps, single-instance and IPC between them, autostart, and permissions (portal-style) for apps that are not trusted.
