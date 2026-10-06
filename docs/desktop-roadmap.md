# Desktop environment roadmap

**North star:** a cross-platform desktop environment, as complete as we can manage, built on bro: Windows and Linux at least. On Linux, bro drives the screen itself (DRM/KMS, libinput, logind, XWayland). Desktop apps are their own repos; broterm is the first.

**Current milestone:** a terminal in bro good enough to run Claude Code (`<terminal>` in bro and [broterm](https://github.com/wlejon/broterm)).

Every repo is listed in [ecosystem.md](ecosystem.md). This page records where things stand and what is left. Last updated 2026-10-06.

## Done

- **bro `<terminal>`** is a native replaced element painted by bro, on its own compositor layer, over bropty. It covers keys, IME and paste; mouse selection and reporting; scrollback, search and links; OSC title/cwd/bell/notification/progress/52/133/99/22 events; inline images (kitty, sixel, iTerm2); the foreground process; an `activity` event; the effective palette; runtime scrollback and cursor options; and persistent sessions through bromux (protocol 2.1 carries the same extras). API: [terminal-api.js](terminal-api.js).
- **broterm** has tabs and splits, profiles, settings, a command palette and keybindings. It also has find, links, a clipboard-read policy and paste safety. Shell integration for pwsh, Windows PowerShell, bash, zsh and fish provides command marks, prompt navigation, durations, re-run, a sticky header and long-command notifications. Titles follow the foreground process, and persistent sessions reattach on restart. 36 tests pass on Windows and Linux.
- **Desktop substrate libraries**, all public with CI:
  - Linux session: broseat, brodmabuf, browl
  - Display and shell: brodisplays, brocompositor
  - Files and apps: brovfs, broapps, brothumb, brocas
  - System and credentials: brosys, brocred
  - Look and input: brothemes, brokeys, broa11y
  - Settings and portals: broconf, broportal
- **Organization:** ecosystem index, `scripts/repos.txt`, repo-status scripts over every repo, the flat `third_party` submodule convention with https URLs, and a submodule-fallback CI job in each library.

## Open: terminal milestone

1. **broterm adopts the latest element APIs:** the `activity` event replaces its 500 ms background-tab poll. Also adopt `palette`, `cwd` as a path, `options.scrollback` and `options.cursorStyle`.
2. **`bro.window`** needs desktop notifications, taskbar progress, flash, the bell sound and focus. It also needs title, opacity, fullscreen, multi-window, single-instance, a global hotkey and a tray icon. broterm's executable stands in for some of these today.
3. **Acceptance:** run Claude Code in broterm on Windows and Linux, and fix what breaks.
4. **macOS:** port broterm's tests. The macOS bindings use Cmd, and bash 3.2 lacks PS0. 11 of 36 fail and are informational in CI.
5. **Known limits:**
   - Some Windows conhost builds drop terminal replies (documented in bropty).
   - Persistent sessions ignore the client's scrollback limit and image quota.
   - Only iTerm2 images survive ConPTY.

## Open: CI and hygiene

**Red CI on main (2026-10-06):**

| Repo | Failure |
|---|---|
| bro | Linux and macOS have failed since at least 1b7018d2. On Linux, SDL3 static fails to link `Wayland_bootstrap` (minimal and app). |
| broterm | ui-commands fails "the mark opens a menu" on Linux. |
| broaudio | Coverage: the `test_partitioned_convolver` realtime-ratio assert is timing-sensitive on the runner. |
| browl | `test_sway_shell` fails on configure serial and popup reposition (CI sway version). |

**CodeQL:**
- Fix these: the image-size multiplications in bropty `frame_images.cpp` and bro clipmap; the world-writable files in bromux `tee.cpp` and `tools/bromux.cpp`; and bromux's dangerous-function alert.
- Dismiss the path-injection alerts in file and tool code as by design (brosearch, brovfs, broapps, brothumb, brocred, brodisplays), with reasons.

**Remaining README pass:** terminal and desktop libraries. Engine libraries are done.

**Self-hosted Linux runner for hardware jobs:** real-GPU Vulkan, seat and DRM. Put it on jserve, which has Docker; halo has none. Use ephemeral containers, run on pushes to main only, never on fork PRs. Hosted runners stay for Windows and macOS.

**Agents:** verify locally first, on halo or a Docker image matching the runner on jserve. Check CI once per push instead of polling. Several agents polling the GitHub API exhaust its rate limit.

## Open: foundational pieces for the desktop

1. **JS bindings for the desktop libraries.** None of brosys, brovfs, brodisplays, broapps, brothumb, brocred, broconf, brothemes, brokeys, broa11y or broportal has a `<name>_api` yet, so bro apps cannot use them. The binding convention is the engine siblings' `src/api/` plus `installSiblingApis`.
2. **bro on Linux as the compositor:** a nested compositor first (client buffers via brodmabuf, with fences), then bro owning the screen (composited KMS output, input through broseat and libinput), then browl shell surfaces, then XWayland. A prototype (`722ea9a0`) is on halo, on branch `halo-substrate-prototype`. It is reference only: DesktopPlatform is not wired into the engine, fences are ignored, and only direct scanout works.
3. **An accessibility tree out of bro:** the DOM has to become a broa11y tree (UIA, AT-SPI, NSAccessibility).
4. **One D-Bus layer:** brosys, brocred, broseat and broportal each carry their own sd-bus code. Unify them in one library.
5. **Shell apps on top:**
   - panel, launcher and taskbar (broapps, brocompositor's foreign-toplevel)
   - notification centre (brosys)
   - file manager (brovfs, brothumb)
   - settings (broconf, brodisplays, brothemes)
   - lock screen and greeter (session lock, brocred authentication)
6. **brocompositor gap audit** against what a daily-driver session needs: protocols, multi-monitor, HiDPI, damage, input methods, clipboard and drag-and-drop across clients.
7. **Session and app model:** launching and tracking DE apps, single-instance and IPC between them, autostart, and permissions (portal-style) for apps that are not trusted.
