# Desktop Trust Model and Privileged Surfaces

This document specifies the security boundary, trust tiers, and privileged namespace enforcement for bro desktop applications.

---

## 1. Problem and Threat Model

Historically in bro, every application was fully trusted native code with uniform runtime access. In a desktop environment, however, applications fall into two fundamentally distinct roles:

1. **Ordinary applications** (e.g. text editors, media players, terminal emulators, utilities, games).
2. **Desktop shell applications** (e.g. system panel, launcher, taskbar, settings, lock screen, greeter, portal backend, window compositor).

Desktop substrate libraries provide capabilities that must never be accessible to arbitrary unvetted applications:
- **Authentication and secrets (`bro.cred`):** PAM authentication, login passwords, biometric sensors, Secret Service master credentials, PolicyKit authorization prompts.
- **Session and hardware control (`bro.seat`):** VT switching, session locking, idle inhibition, forced logout.
- **Display management (`bro.displays`):** Mode setting, physical resolution/refresh alterations, multi-monitor topology, night light LUT modification, display brightness.
- **System services host (`bro.sys`):** StatusNotifierItem/tray host, desktop notification daemon host, system power management (suspend/reboot/shutdown), Wi-Fi connection with raw secrets, Bluetooth pairing.
- **Window management and compositor (`bro.compositor`, `bro.wl`):** Foreign toplevel management, moving and resizing other applications' windows, workspace manipulation, arbitrary screen capture and screencopy.
- **Desktop portals backend (`bro.portal`):** Registering as the system portal handler to intercept and respond to file-chooser, screenshot, or screencast requests from other applications.

If an ordinary app had direct access to these APIs, any untrusted script or third-party package could capture keystrokes, intercept credentials, or silently capture the screen.

---

## 2. Trust Tiers

The bro engine establishes two explicit tiers:

### Tier 1: Ordinary Applications
- Granted standard application-facing APIs (`bro.vfs`, `bro.apps`, `bro.keys`, `bro.themes`, `bro.search`, `bro.thumb`, `bro.conf` for its own app keys).
- Granted application-local window APIs (`bro.window` for its own title, opacity, fullscreen, progress, notifications, system tray icon, and global hotkeys).
- Privileged desktop namespaces are completely unavailable:
  - `bro.<ns>.available === false`
  - Attempting to invoke or access functions on a privileged namespace throws an immediate `Error`:
    `"bro.<ns> is unavailable: this build was compiled without trusted shell declaration in bro.json"`
- Any system-wide privileged action (such as opening arbitrary files or capturing a window) must be requested via user-mediated OS portals or bro-hosted portal dialogs.

### Tier 2: Shell Applications
- Dedicated desktop components that form the desktop environment itself.
- Must explicitly declare their requested privileges in `bro.json`:
  ```json
  {
      "name": "system-panel",
      "shell": true
  }
  ```
  or selectively:
  ```json
  {
      "name": "settings-manager",
      "privileged": ["displays", "sys", "conf"]
  }
  ```
- **Prerequisite:** Privileges are ONLY granted if the application is installed in a trusted desktop location verified by the engine. If an app outside a trusted location requests privileges, the engine rejects the request with a warning log and mounts unavailable stubs instead.

---

## 3. Trusted Install Locations

To ensure unprivileged user downloads cannot escalate privileges simply by authoring `"shell": true` in `bro.json`, the engine evaluates the canonical filesystem path of the application directory against designated system and desktop prefixes.

### Linux
- System desktop packages:
  - `/usr/share/bro/apps`
  - `/usr/share/bro/system`
  - `/usr/local/share/bro/apps`
  - `/opt/bro/apps`
- Engine-bundled system directories:
  - `<executable_dir>/apps`
  - `<executable_dir>/system`
  - `<resource_dir>/apps`
  - `<resource_dir>/system`
- Project root system directories:
  - `<project_root>/system`
  - `<project_root>/desktop`

### Windows
- Machine-wide install prefixes:
  - `%ProgramFiles%\bro\apps`
  - `%ProgramFiles%\bro\system`
  - `%ProgramData%\bro\apps`
  - `%ProgramData%\bro\system`
  - `%LOCALAPPDATA%\bro\system`
- Engine-relative prefixes:
  - `<executable_dir>\apps`
  - `<executable_dir>\system`

### macOS
- System and application bundle prefixes:
  - `/Library/Application Support/bro/apps`
  - `/Library/Application Support/bro/system`
  - `/Applications/bro.app/Contents/Resources/apps`
  - `/usr/local/share/bro`
  - `/opt/bro/apps`
- Engine-relative prefixes:
  - `<executable_dir>/apps`
  - `<executable_dir>/system`

### Development & Testing Overrides
For development, automated test suites, and CI environments where root/administrator installation is impractical:
- `BRO_TRUSTED=1` (or `BRO_TRUSTED=true`): Unconditionally treats the launched app as trusted (useful in headless integration tests).
- `BRO_TRUSTED_APP_DIR=<path1>[:<path2>...]` (colon-delimited on POSIX, semicolon-delimited on Windows): Explicitly adds directories to the trusted prefix list.

---

## 4. Code Signing & Integrity

1. **Path Canonicalization:** All path checks use `std::filesystem::weakly_canonical` to resolve symlinks and `..` traversals, preventing symlink substitution attacks into trusted locations.
2. **Operating System Boundaries:**
   - On Linux, system directories like `/usr/share/bro` and `/opt/bro` are root-owned and protected by POSIX filesystem permissions. Ordinary user processes cannot place files there without sudo/polkit authorization.
   - On Windows, `%ProgramFiles%` is protected by UAC and administrator ACLs.
   - On macOS, `/Library/Application Support/bro` requires admin rights, and bundle resources in `/Applications/bro.app` are protected by macOS Gatekeeper and code signature verification (`codesign`).

---

## 5. User Mediation for Ordinary Apps

Ordinary apps that need access to capabilities managed by shell apps interact via asynchronous user-mediated protocols:
- **File Chooser:** Handled via XDG Desktop Portal (`org.freedesktop.portal.FileChooser`) on Linux, `IFileDialog` on Windows, and `NSOpenPanel` on macOS. The app only receives file descriptors or paths that the user explicitly selected in the file dialog.
- **Screen Sharing / Capture:** Mediated through the Screencast portal (`org.freedesktop.portal.ScreenCast`), where the desktop environment prompts the user to select which display or window to share.
- **Credentials & Authentication:** Handled through system PolicyKit agents or OS credential prompts. Secret storage for ordinary apps is isolated and sandboxed by their declared application ID.
