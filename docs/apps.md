# Folder apps: the manifest, the runtime, installing

A folder holding a `bro.json` and an `index.html` is a desktop application.
The stock `bro` runs it (`bro <dir> [args...]`), and the folder carries no
native code, no build and no pins. When an app needs something native, that
capability goes into bro (or into a sibling library exposed as `bro.<x>`),
never into the app. An ahead-of-time `app.dll` beside `index.html` is an
optional packaging speed-up, not a requirement.

Each app runs as its own bro process. An installed app's process is named for
the app (`helm-calculator`), started by its own small executable, and every
bro process maps the one engine library, libbro ([The app's
executable](#the-apps-executable)). On Linux under helm it is an ordinary
Wayland client of the compositor.

This page covers the manifest keys that make a folder an application. The
window keys (`title`, `width`, `height`, `minWidth`, `resizable`, `splash`,
`watch`, ...) are listed by `bro --help`; project manifests (`default_app`,
`lib`, `system`) are in [projects.md](projects.md). What the running app sees
is [app-api.js](app-api.js) (`bro.app`).

## The manifest

```json
{
    "id": "org.example.Notes",
    "name": "Notes",
    "version": "1.4.0",
    "description": "Plain-text notes",
    "icon": "assets/icon.png",
    "categories": ["Utility", "TextEditor"],
    "keywords": ["text", "notes"],
    "singleInstance": true,
    "fileTypes": [
        { "name": "Text", "mimeTypes": ["text/plain", "text/markdown"], "extensions": ["txt", "md"] }
    ],
    "actions": [
        { "id": "new-window", "name": "New Window", "args": ["--new-window"] }
    ],
    "permissions": [],

    "width": 900, "height": 640, "minWidth": 360, "minHeight": 240,
    "splash": false, "watch": false
}
```

| Key | Meaning |
|---|---|
| `id` | Reverse-DNS (`org.example.Notes`) or a slug (`notes`): `[A-Za-z0-9._-]`, no leading/trailing `.`, no `..`. It becomes the window's Wayland `app_id` / X11 `WM_CLASS` / Windows AppUserModelID, the desktop entry's name (`<id>.desktop`), the single-instance channel and the key of the app's directories. Without one, the folder's name is the id, and the app keeps the old anonymous behaviour (bro.log in the working directory, settings beside the executable). |
| `name`, `version`, `description` | Shown by launchers (desktop entry `Name`, `Comment`). `name` is also the window's title until the page has one (below). |
| `icon` | Path relative to the app folder: an SVG or any bitmap bro decodes. It is the window icon (title bar and taskbar on Windows, `_NET_WM_ICON` on X11, `xdg-toplevel-icon` where a Wayland compositor offers it), and the desktop entry names its absolute path, which is how a Wayland shell finds it through the window's `app_id`. |
| `categories`, `keywords` | FreeDesktop menu categories and search keywords. |
| `singleInstance` | A launch while the app runs hands its argv and working directory to the running instance and exits (below). |
| `display` | `false` hides the app from launchers (`NoDisplay=true`): a helper started by another app. |
| `fileTypes` | What the app opens: the desktop entry's `MimeType=`, and `%F` in its `Exec=` so the opened files arrive as arguments. |
| `actions` | Launcher actions: `[Desktop Action <id>]` entries that run the app with `args`. `id` is `[A-Za-z0-9-]`. |
| `permissions` | Privileged namespaces the app asks for (below). `privileged` is the older spelling and still read. |
| `shell` | A desktop shell component (panel, launcher, compositor host): asks for every privileged namespace (below). |

The engine reads bro.json with a real JSON parser for these keys. A key of the
wrong type is skipped with a warning; it never fails the launch.

**Window title.** The window opens titled with `name`, then follows the page's
title: its `<title>` once it loads, and every later `document.title = ...`
(an empty title shows `name` again). `bro.window.setTitle()` sets it directly.
The window key `title` is a fixed title instead: it wins over both and the
window ignores `document.title`. Without `name` or `title` the window is "Bro".

**Transparent, click-through windows.** Two window keys make an overlay: a
layer over other applications that draws annotations or widgets and lets the
rest of the desktop be used through it.

| Key | Meaning |
|---|---|
| `transparent` | The window composites with per-pixel alpha over whatever is behind it: where the page paints nothing (a transparent `html` and `body` background) the desktop shows through, and translucent CSS (rgba, `opacity`, shadows) blends with it. Fixed when the window is created. `bro.window.transparent` reads whether it took (it needs the GPU presentation and a window system that offers it). |
| `clickThrough` | Pointer input over a point the page would not hit (no element there, or `pointer-events: none` all the way down; `<html>` and `<body>` never count) goes to whatever lies beneath the window, other applications included; over an element, the window takes it as usual. Steer it with CSS alone. `bro.window.clickThrough` turns it on and off at run time. |

```json
{
    "name": "Pointer Notes", "width": 480, "height": 320,
    "transparent": true, "clickThrough": true, "borderless": true, "alwaysOnTop": true
}
```

```html
<html style="background: transparent">
<body style="margin: 0; background: transparent">
  <!-- drawn over the desktop, never in the way -->
  <div style="position: absolute; left: 20px; top: 20px; width: 200px; height: 120px;
              border: 3px solid rgba(255, 64, 64, .9); border-radius: 12px; pointer-events: none"></div>
  <!-- takes clicks -->
  <button style="position: absolute; right: 16px; bottom: 16px" onclick="bro.quit()">Close</button>
</body>
</html>
```

How each window system does it, and what it costs, is in
[window-api.js](window-api.js) under `bro.window.clickThrough`.

## The command line

```
bro [bro flags] <app-dir | app-id> [app arguments...]
```

Everything after the app directory is the app's: `bro.app.argv`. bro takes
out only its own flags (`--no-gpu`, `--no-splash`, `--splash`, `--drm`,
`--new-instance`, `--notification <args>`, `--notification-activated`), and
only before a `--`. The `--` itself is passed through,
so `bro ~/apps/term -- vim notes.txt` gives the app `["--", "vim",
"notes.txt"]`. `bro.app.cwd` is the directory the app was started from.

Under `bro-headless <dir> [script.js] -- args...`, argv is what follows `--`.

An installed app can be named by its id instead of its path (`bro
org.example.Notes`). bro looks it up in the install roots below.

## Per-app directories and the log

Keyed by the id, created on first read:

| | Linux | Windows | macOS |
|---|---|---|---|
| `bro.app.configDir` | `$XDG_CONFIG_HOME/<id>` | `%APPDATA%\<id>` | `~/Library/Application Support/<id>` |
| `bro.app.dataDir` | `$XDG_DATA_HOME/<id>` | `%APPDATA%\<id>\data` | `~/Library/Application Support/<id>/data` |
| `bro.app.cacheDir` | `$XDG_CACHE_HOME/<id>` | `%LOCALAPPDATA%\<id>\cache` | `~/Library/Caches/<id>` |
| log | `$XDG_STATE_HOME/<id>/<id>.log` | `%LOCALAPPDATA%\<id>\<id>.log` | `~/Library/Logs/<id>/<id>.log` |

`BRO_APP_HOME=<dir>` puts all four under `<dir>/{config,data,cache,log}`. Tests
and portable installs use it to keep an app away from the user's own state,
the desktop settings it keeps through `bro.conf` included: under
`BRO_APP_HOME` those live in `<dir>/config/settings.ini`, not the user's own
store, and no session bus is listened to.

An app with a declared `id` also keeps bro's own per-app settings (window
geometry, engine preferences; [settings.md](settings.md)) in
`<configDir>/bro_settings.json` rather than beside the executable. Its stdout
and stderr go to the log (`bro.app.logFile`). A second instance that finds the
log taken (`--new-instance`, or an app without `singleInstance`) writes
`<id>-<pid>.log` beside it. Those are pruned at every launch: a `<id>-<pid>.log`
no running process is writing is deleted, except the newest five, kept for a
look after a crash. (The anonymous `bro.log` / `bro-<pid>.log` in a working
directory follow the same rule.)

## Single instance

With `"singleInstance": true`, the first launch claims a per-user channel named
after the id: a named pipe plus a named mutex on Windows, and a Unix socket
plus an flock'd lock file in `$XDG_RUNTIME_DIR` elsewhere. A later launch
finds the owner, sends it its argv and working directory, and exits 0. It
does this before any window, GPU or script work, so the hand-off costs a
launcher a few milliseconds.

The running instance gets an `instance` event on `bro.app`:

```js
bro.app.addEventListener('instance', (e) => {
    openTab({ args: e.argv, cwd: e.cwd });   // resolve paths against e.cwd
});
```

bro raises the app's window when a launch arrives. Launches that arrive before
the page listens are held and delivered to the first listener.
`bro --new-instance <dir>` (and `bro.app.spawn(args, {newInstance: true})`)
skips the hand-off and opens a second instance. That is how an app does
"New Window".

One app opens another by id with `bro.app.open(id, args)`, on every
platform: the id is found as `bro <id>` finds it (the install roots), else
beside the calling app in its project, and the stock bro starts it. A
single-instance target that is running gets the `instance` event above
rather than a second window. `bro.app.find(id)` says where it would be found.
See [app-api.js](app-api.js).

`bro-headless` claims the channel only with `--single-instance`, so parallel
test runs of one app never hand off to each other.

## Permissions

Privileged namespaces (`compositor`, `wl`, `displays`, `seat`, `sys`, `cred`,
`portal`, `clip`, `pulse`, `remote`) are stubs for an ordinary app:
`bro.remote.available === false`, `bro.remote.reason` says why, and any call
throws that reason. An app gets the namespaces it lists in `permissions`, or
all of them for `"shell": true`, when one of these holds:

- it is installed in a trusted location (`isTrustedAppLocation`,
  `src/engine/desktop_trust.cpp`): `apps/` and `system/` beside the executable
  or in the resource dir; Linux `/usr/share/bro`, `/usr/local/share/bro`,
  `/opt/bro/apps`; Windows `%ProgramFiles%\bro\{apps,system}`,
  `%ProgramData%\bro\{apps,system}`; macOS `/Library/Application
  Support/bro/{apps,system}`, `/Applications/bro.app/Contents/Resources/apps`,
  `/usr/local/share/bro`, `/opt/bro/apps`;
- `BRO_TRUSTED_APP_DIR=<dir>[:<dir>...]` names its directory (`;` on Windows);
- the user's permissions file grants them to its id. The app gets the
  intersection of what it asked for and what was granted:

```json
// ~/.config/bro/permissions.json, %APPDATA%\bro\permissions.json,
// ~/Library/Application Support/bro/permissions.json
{
    "org.example.ScreenShare": ["remote"],
    "org.example.MyShell": ["shell"],
    "org.example.Settings": ["*"]
}
```

`bro.app.permissions` reports `{requested, granted, shell}`.

On a development machine, `{ "*": ["*"] }` in the permissions file, or
`BRO_TRUST_ALL=1` for one process, grants every app what its manifest asks for;
a namespace missing from the manifest is still refused.
`BRO_PERMISSIONS_FILE=<path>` reads that file instead of the user's (the test
suites point it at a file that does not exist).

## Installing, and how launchers find apps

Installed folder apps live in one directory per app, named by id:

| | user | system |
|---|---|---|
| Linux | `$XDG_DATA_HOME/bro/apps/<id>` (`~/.local/share/bro/apps`) | `$XDG_DATA_DIRS/bro/apps/<id>` (`/usr/local/share/bro/apps`, `/usr/share/bro/apps`) |
| Windows | `%LOCALAPPDATA%\bro\apps\<id>` | `%ProgramFiles%\bro\apps\<id>` |
| macOS | `~/Library/Application Support/bro/apps/<id>` | `/Library/Application Support/bro/apps/<id>` |

```
bro --install <app-dir> [--link] [--system] [--exec <bro>] [--no-exe]
bro --build-exe <app-dir | id> [--out <exe>] [--force] [--system] [--quiet]
bro --uninstall <id> [--system]
bro --list-apps
bro --desktop-entry <app-dir> [--exec <bro>]
```

`--install` copies the folder there (leaving out `.git`), replacing a previous
copy atomically. An app inside a project (see
[projects.md](projects.md#a-project-holding-several-apps)) gets the project's
`lib` copied in as `<id>/lib`, so `/lib/...` imports keep resolving; an app
with its own `lib/` keeps it. `--link` symlinks the folder instead, which suits
a checkout you are working in, and the linked app finds its project through
the link. Then it builds the app's own executable (next section) and
registers it with the system:

- **Linux:** a FreeDesktop desktop entry,
  `$XDG_DATA_HOME/applications/<id>.desktop` (`--system`:
  `/usr/local/share/applications`), whose `Exec=` is the executable. Any
  launcher reading the standard directories lists the app: `bro.apps`, helm's
  launcher, a GNOME or KDE menu. None needs a special case.
- **Windows:** a Start menu shortcut,
  `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Bro Apps\<name>.lnk`
  (`--system`: the machine's Programs folder), that starts the executable
  under the app's AppUserModelID (the id). The running window carries the
  same id, so the shortcut, a pinned taskbar button and the window are one
  taskbar group, with the app's icon.

The entry looks like this:

```ini
[Desktop Entry]
Type=Application
Name=Notes
Exec=/home/j/.local/share/bro/bin/org-example-Notes %F
TryExec=/home/j/.local/share/bro/bin/org-example-Notes
StartupWMClass=org.example.Notes
Categories=Utility;TextEditor;
MimeType=text/plain;text/markdown;
SingleMainWindow=true
X-Bro-App=/home/j/.local/share/bro/apps/org.example.Notes
Actions=new-window;

[Desktop Action new-window]
Name=New Window
Exec=/home/j/.local/share/bro/bin/org-example-Notes --new-window
```

`StartupWMClass` is the id, which is also the window's app_id, so a launcher
matches running windows to their entry. With `--no-exe`, or where no
executable can be built (macOS for now, or a bro without `bro-app-stub`),
`Exec=` runs the folder under bro instead: `Exec=/usr/bin/bro
/home/j/.local/share/bro/apps/org.example.Notes %F`. `--exec` names that bro;
the default is the bro running the command. `--desktop-entry` prints this
folder form and writes nothing.

## The app's executable

Every installed app gets a small executable named for it: the id with its dots
made dashes (`helm.calculator` → `helm-calculator`, `helm-calculator.exe`),
in `bin/` beside the install root (`~/.local/share/bro/bin`,
`%LOCALAPPDATA%\bro\bin`). It is what makes the app itself to the system:

| | |
|---|---|
| process name | the executable's name (`ps`, `top`, Task Manager, `pgrep helm-calculator`) |
| window class / app_id | the id (Wayland `app_id`, X11 `WM_CLASS`, Windows AppUserModelID), as for any app with an id |
| icon | Windows: embedded in the executable (every size, from the manifest's icon); Linux: the desktop entry's `Icon=` |
| name and version | Windows: the executable's version resource (`FileDescription`, `ProductVersion`) |
| control endpoint | `<id>-<pid>` ([agent-control.md](agent-control.md)) |

It holds no engine. It loads libbro, the one engine library every bro process
maps (BUILDING.md, "The engine library"), and runs the app in its own
process. `bro.app.executable` is its path (`""` when the app runs as a folder
under bro). Its arguments are all the app's (`bro.app.argv`), apart from
bro's own few flags before a `--`; the folder is built in.

It refers to the installed folder rather than holding the app, so editing the
installed copy needs no rebuild. What it is built from is the app's identity:
`bro.json` and the icon. Its cache key is a hash of those and the engine
build (`bro_engine_version`), and `bro --install` / `bro --build-exe` rebuild
it only when either changed (`--force` rebuilds anyway).

**When it is out of date it still starts.** An executable built against
another engine build (bro was rebuilt or upgraded), or whose app's `bro.json`
or icon changed since, does not run as itself: it starts `bro --build-exe` in
the background to replace itself, and hands this launch to the stock bro
beside the engine as a folder (`bro <app-dir> <args>`), so the app opens
either way. The next launch runs the fresh executable. If the engine library
itself is gone or speaks another entry ABI, the executable runs the folder
under the stock bro its identity names. It never fails to start for want of a
matching engine.

**Development stays a folder.** `bro <dir>` is the edit loop: hot reload, no
build, no install. The executable is for what is installed.

Built on the machine the app is installed on, with no compiler: the
executable is a copy of `bro-app-stub` (shipped beside libbro) with the
app's identity stamped into it, and on Windows its icon and version
resources written with the system's own resource updater. BUILDING.md has how
and why.

What it is a seam for, later: the app's ahead-of-time compiled script
(today an `app.dll` beside `index.html`), the natives modules the app uses,
a startup heap snapshot, packed assets, and per-app release downloads. Each
is another input to the cache key and another part of the executable; none of
them changes how it is named, registered or launched.

## Startup cost

`bro <dir>` compiles the page's scripts in process at boot (bronze, tiered
JIT). The compiled code is cached on disk ([code-cache.md](code-cache.md)), so
the second and later launches skip the compile. Shipping an `app.dll` beside
`index.html` skips it on the first launch too.

Every launch logs its time to the first presented frame, measured from just
before `main()`: `app <id>: first frame N ms after start` (in the app's log
file).
