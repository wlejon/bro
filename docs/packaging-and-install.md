# Desktop Packaging, Installation, and Session Registration

This document specifies the packaging architecture, filesystem layout, session registration, and update mechanics for the bro desktop environment on Linux and Windows.

---

## 1. Overview and Architecture

The bro desktop environment consists of:
1. **Engine runtime and compositor (`bro`, `bro-headless`, `bro-server`):** C++20 executables providing the Vulkan 1.3 presentation pipeline, Wayland compositor, KMS direct display output, and JS execution realm.
2. **System UI (`system/`):** Engine-level panels (splash, nav, menu, perf, inspector, settings overlays) mounted at `/system`.
3. **Desktop shell applications (`apps/`):** Privileged desktop components (panel/taskbar, application launcher, notification daemon, file manager, settings manager, session lock / greeter, portal backend).
4. **Wayland session registration:** `.desktop` session entries for display managers (GDM, SDDM, LightDM) to launch `bro --drm`.

---

## 2. Linux Desktop Integration

### Directory Layout

When installed system-wide (e.g. `PREFIX=/usr` or `/usr/local`), bro establishes the following canonical hierarchy:

| Filesystem Path | Purpose | Permissions |
|---|---|---|
| `$PREFIX/bin/bro` | Primary desktop environment & application runtime | `0755` |
| `$PREFIX/bin/bro-headless` | Headless execution, test harness, and script runner | `0755` |
| `$PREFIX/bin/bro-server` | Headless headless service & worker runner | `0755` |
| `$PREFIX/share/bro/system/` | Built-in system UI panels and runtime assets | `0755` |
| `$PREFIX/share/bro/apps/` | **Trusted shell applications directory** | `0755` (root owned) |
| `$PREFIX/share/wayland-sessions/bro.desktop` | Wayland session entry for display managers | `0644` |
| `$PREFIX/share/applications/bro.desktop` | Application menu launcher entry | `0644` |
| `$PREFIX/share/icons/hicolor/256x256/apps/bro.png` | Desktop application icon | `0644` |
| `$PREFIX/lib/systemd/user/bro-session.target` | Systemd user session integration | `0644` |

### Display Manager Session Registration

Display managers compliant with the FreeDesktop Wayland session specification look in `/usr/share/wayland-sessions/` for available compositor sessions.

The registered file (`/usr/share/wayland-sessions/bro.desktop`):
```ini
[Desktop Entry]
Name=bro
Comment=The bro desktop environment
Exec=bro --drm
TryExec=bro
Type=Application
DesktopNames=bro
```

When a user selects "bro" at the display manager login prompt:
1. The display manager allocates a seat through `logind` / `systemd-logind`.
2. It launches `bro --drm` with user session environment variables (`XDG_SESSION_TYPE=wayland`, `XDG_CURRENT_DESKTOP=bro`).
3. `bro` initializes KMS atomic modesetting, claims the DRM master, sets up `libinput`, and starts the `WaylandCompositor`.
4. `bro` runs autostart applications via `broseat::AutostartService` and launches the configured desktop shell apps from `/usr/share/bro/apps`.

### Systemd User Session Integration

`bro-session.target` binds to `graphical-session.target`:
```ini
[Unit]
Description=bro Wayland session
BindsTo=graphical-session.target
Wants=graphical-session-pre.target
After=graphical-session-pre.target
```
This ensures standard user-level background services (PipeWire, bluetooth-applet, network agents) cleanly start when bro activates and cleanly stop upon session logout.

---

## 3. Trusted Shell Location Fulfillment

As specified in `docs/desktop-trust.md`, privileged desktop namespaces (`bro.compositor`, `bro.wl`, `bro.displays`, `bro.seat`, `bro.sys`, `bro.cred`, `bro.portal`) are strictly restricted.

The engine verifies the canonical location of an application using `isTrustedAppLocation()` in `src/engine/desktop_trust.cpp`:
- On Linux, `/usr/share/bro/apps`, `/usr/local/share/bro/apps`, `/opt/bro/apps`, and `<exe_dir>/apps` are verified trusted prefixes.
- Shell apps installed into `/usr/share/bro/apps/<app-name>` declaring `"shell": true` in `bro.json` automatically pass verification:
  - `evaluateDesktopTrust()` assigns `info.isTrusted = true` and `info.isShell = true`.
  - All privileged APIs are mounted with `available: true`.
- User-downloaded ordinary apps residing in `~/.local/share/applications` or user home folders cannot elevate privileges even if they declare `"shell": true`.

---

## 4. Windows Desktop Integration

### Directory Layout

On Windows, bro is installed to `%ProgramFiles%\bro`:

| Path | Purpose |
|---|---|
| `%ProgramFiles%\bro\bro.exe` | Application runtime |
| `%ProgramFiles%\bro\bro-headless.exe` | Headless execution |
| `%ProgramFiles%\bro\system\` | System UI assets |
| `%ProgramFiles%\bro\apps\` | **Trusted shell applications directory** |

### Start Menu & Shell Association

The Windows installer registers:
- `bro.exe` in the system `PATH`.
- Start menu shortcut under `Start Menu\Programs\Bro`.
- URL protocol handler `bro://` pointing to `bro.exe "%1"`.
- Single-instance IPC named pipe `\\.\pipe\bro-single-instance-<hash>` for seamless CLI-to-window argument forwarding.

---

## 5. Updates and Rollouts

1. **Engine Updates:**
   - Linux: Managed via system package managers (`pacman`, `apt`, `rpm`) or atomic binary replacement in `/usr/bin/`.
   - Windows: In-place update using MSI/InnoSetup installer.
2. **Shell Apps Updates:**
   - Atomic directory replacement: `apps/<app>.new` staged and atomically renamed to `apps/<app>`.
   - In-flight applications reload using `location.reload()` or the devloop watcher without tearing down the display server.
