# Folder apps: the manifest, the runtime, installing

A folder holding a `bro.json` and an `index.html` is a desktop application.
The stock `bro` runs it (`bro <dir> [args...]`), and the folder carries no
native code, no build and no pins. When an app needs something native, that
capability goes into bro (or into a sibling library exposed as `bro.<x>`),
never into the app. An ahead-of-time `app.dll` beside `index.html` is an
optional packaging speed-up, not a requirement.

Each app runs as its own bro process. On Linux under helm it is an ordinary
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

## The command line

```
bro [bro flags] <app-dir | app-id> [app arguments...]
```

Everything after the app directory is the app's: `bro.app.argv`. bro takes
out only its own flags (`--no-gpu`, `--no-splash`, `--splash`, `--drm`,
`--new-instance`), and only before a `--`. The `--` itself is passed through,
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
bro --install <app-dir> [--link] [--system] [--exec <bro>]
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
the link. On Linux it also writes a FreeDesktop desktop entry,
`$XDG_DATA_HOME/applications/<id>.desktop` (`--system`:
`/usr/local/share/applications`). Any launcher reading the standard
directories lists the app: `bro.apps`, helm's launcher, a GNOME or KDE menu.
None needs a special case. The entry looks like this:

```ini
[Desktop Entry]
Type=Application
Name=Notes
Exec=/usr/bin/bro /home/j/.local/share/bro/apps/org.example.Notes %F
TryExec=/usr/bin/bro
StartupWMClass=org.example.Notes
Categories=Utility;TextEditor;
MimeType=text/plain;text/markdown;
SingleMainWindow=true
Actions=new-window;

[Desktop Action new-window]
Name=New Window
Exec=/usr/bin/bro /home/j/.local/share/bro/apps/org.example.Notes --new-window
```

`--exec` names the bro to put in `Exec=`. The default is the bro running the
command. `StartupWMClass` is the id, which is also the window's app_id, so a
launcher matches running windows to their entry.

## Startup cost

`bro <dir>` compiles the page's scripts in process at boot (bronze, tiered
JIT). The compiled code is cached on disk ([code-cache.md](code-cache.md)), so
the second and later launches skip the compile. Shipping an `app.dll` beside
`index.html` skips it on the first launch too.

Every launch logs its time to the first presented frame, measured from just
before `main()`: `app <id>: first frame N ms after start` (in the app's log
file).
