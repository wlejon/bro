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
| `permissions` | The capabilities the app may ask for (below). Declaring one grants nothing. |

The engine reads bro.json with a real JSON parser for these keys. A key of the
wrong type is skipped with a warning, with two exceptions that fail the launch
with a message saying what is wrong: a bro.json that is not a JSON object, and
a `permissions` that is not a list of capability names (a misspelt capability
would otherwise be denied without anyone learning why). The id `bro` is bro's
own and no app's.

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
`--new-instance`, `--notification <args>`, `--notification-activated`,
`--grant <caps>`), and only before a `--`. The `--` itself is passed through,
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
platform: the id is found beside the calling app in its project first, else
as `bro <id>` finds it (the install roots), and the stock bro starts it. The
project's copy is the one built and changed with the caller; an installed
app's siblings are the install root's other apps, so it finds those either
way. A single-instance target that is running gets the `instance` event above
rather than a second window. `bro.app.find(id)` says where it would be found.
See [app-api.js](app-api.js).

`bro-headless` claims the channel only with `--single-instance`, so parallel
test runs of one app never hand off to each other.

## Permissions

An app reaches outside itself only through capabilities granted to it, one
app at a time. The aim is first to contain mistakes: an app that deletes the
wrong folder, or starts the wrong process, can only do so if it was given the
means. Nothing assumes an app is friendly. The catalogue is one table,
`src/engine/capabilities.cpp`:

| Capability | Gates | Kind |
|---|---|---|
| `files` | files beyond the app's own (below), through `fs`, sqlite, `bro.vfs`, `bro.search` and `bro.thumb` alike; `bro.apps`' default apps and recent files | ask |
| `net` | `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, `net`, `http`'s servers and clients, `WebSocketServer`, `bro.models`' downloads | ask |
| `spawn` | `child_process`, `bro.app.spawn`, `bro.app.open`, `bro.apps.launch`, `<terminal>` | ask |
| `mic` | `getUserMedia`, `bro.mic`, `bro.listen`, system loopback capture | ask |
| `wl`, `displays`, `clip`, `pulse`, `remote` | the `bro.<name>` namespace | ask |
| `sys.power`, `sys.audio`, `sys.network`, `sys.bluetooth`, `sys.notifications`, `sys.tray`, `sys.processes` | that member of `bro.sys` (`sys.processes` also `bro.sys.listProcesses`/`killProcess`) | ask |
| `shell` | the desktop shell's role (input routing, the DRM compositor host; `Engine::isShellApp()`) | shell |
| `compositor`, `seat`, `cred`, `portal` | the `bro.<name>` namespace | shell |
| `permissions` | `bro.permissions`, every app's grants ([permissions-api.js](permissions-api.js)) | shell |

Everything else (the DOM, canvas, WebGL, the scene, audio output, timers,
`bro.conf`, the app's own directories) needs nothing.

**Declaring.** `permissions` in bro.json lists what the app may ask for.
Declaring grants nothing; a capability the app did not declare is denied
without anyone being asked; a name that is not a capability fails the launch.

**Asking.** An *ask* capability the app declared and the user has not decided
is asked for the first time it is used on the main thread: a native dialog
names the app and what it tried, and the answer is kept. Used from a Worker
first, it is refused with a message saying to ask on the main thread
(`bro.app.requestPermission`). `bro-headless` answers with `setDialogAnswer`
(docs/headless.md); a run that never chose an answer is refused and nothing is
kept. A gated namespace the app declared but has not been granted is a stub
that asks on first touch, and is the live namespace from then on if allowed.
A refused namespace is a stub: `bro.remote.available === false`,
`bro.remote.reason` says why, and any call throws that reason. A refused
native throws an error naming the capability and the path, command or URL.

**Shell capabilities** are never asked for and never granted by the user's
store or dev mode: only the host executable grants them. helm's main grants
its UI every capability.

**Files.** Without `files` an app writes only its own config, data, cache and
log directories ([Per-app directories](#per-app-directories-and-the-log)),
the OS temp directory, and what the user picked; it reads anywhere outside the
user's home but, under the home, only those, its own folder, bro's resources
and `/lib`, and the model directories (`bro.models.dirs`). With `files` it
reads and writes anywhere. A file or folder the user picks in a file dialog
(open, save, folder, sync or async) or drops on the window is the app's to
read and write from then on (a folder with everything in it), kept with its
grants. Paths are resolved (`..`, links) before deciding. No app writes bro's
own state (the grant store, the install roots, bro's executable and resource
directories), with `files` or without.

**Identity.** An app run from an install root (a folder named by its id,
[below](#installing-and-how-launchers-find-apps)) is the install record
`install:<id>`; any other is its canonical folder, `path:<dir>`. The id in
bro.json alone identifies nothing, since any app can claim one.

**The grant store** is `grants.json` in bro's config directory:
`$XDG_CONFIG_HOME/bro` (`~/.config/bro`), `%APPDATA%\bro`,
`~/Library/Application Support/bro`, or `<dir>/bro` under `BRO_APP_HOME=<dir>`.
It maps each identity to its decisions and picked paths, and holds one global
`devMode` flag. **Dev mode** grants every app every ask capability it declares,
over the user's denials, never a shell capability, and is logged loudly at
every launch.

**Managing.**

```
bro --permissions [<id|dir>]        # the store, every app (or one) and its decisions
bro --grant-app <id|dir> <cap>      # allow
bro --revoke-app <id|dir> <cap>     # deny
bro --dev-mode on|off
```

A host grants capabilities outright for one run with
`--grant <cap>[,<cap>...]` on `bro` or `bro-headless` (`--grant declared` is
everything bro.json declares), logged loudly: what tests and development use.
Inside an app, `bro.app.permissions` reports `{declared, granted, denied,
undecided}` and `bro.app.requestPermission(name)` asks now
([app-api.js](app-api.js)); the shell manages every app through
`bro.permissions`.

**Not gated yet:** `process.env`; the natives of other siblings that take a
file path of their own (mesh, image, model and media loaders and encoders,
screenshots), which read and write without the `fs` policy; and a granted
`spawn`, which runs programs with the user's full rights. [app-isolation.md](app-isolation.md) is where
this goes next.

## Installing, and how launchers find apps

Installed folder apps live in one directory per app, named by id:

| | user | system |
|---|---|---|
| Linux | `$XDG_DATA_HOME/bro/apps/<id>` (`~/.local/share/bro/apps`) | `$XDG_DATA_DIRS/bro/apps/<id>` (`/usr/local/share/bro/apps`, `/usr/share/bro/apps`) |
| Windows | `%LOCALAPPDATA%\bro\apps\<id>` | `%ProgramFiles%\bro\apps\<id>` |
| macOS | `~/Library/Application Support/bro/apps/<id>` | `/Library/Application Support/bro/apps/<id>` |

Under `BRO_APP_HOME=<dir>` the one install root, user and system alike, is
`<dir>/bro/apps`: a test or scratch profile neither finds the machine's
installed apps nor installs over them.

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

What it is a seam for, later: the natives modules the app uses, a startup
heap snapshot, packed assets, and per-app release downloads. Each is another
input to the cache key and another part of the executable; none of them
changes how it is named, registered or launched.

## Ahead-of-time scripts

Beside the executable sits its AOT directory, `<executable>.aot`
(`helm-calculator.aot`): the page's scripts compiled ahead of time into
shared libraries the executable loads, so a launch runs machine code from a
file mapped like any other library instead of compiling the page.

**Built at install.** `bro --install` and `bro --build-exe` run
`bro-headless <app> --aot-build <dir>` after stamping the executable. That
headless engine sets up the page's realm exactly as a launch does (host
globals, natives, module roots) and, instead of running each script unit (the
classic scripts together, then each module script), compiles it through
bronze's evaluator, the same front end and IL the JIT compiles, to a module.
The module image is written by brass's own PE / ELF writer: **no compiler or
linker is needed on the machine**, on Windows or Linux. A unit whose module is
still current is kept; the build's own log is `build.log` in the directory, and
`bro --exe-info` prints its summary as `aot.*` lines. A unit that fails to
compile is reported and simply runs on the JIT. `BRO_APP_AOT=0` skips the
build, and makes a launch ignore the directory (for comparing the two).

**Checked at every launch.** Each unit has a record (`<unit>.bzm`) beside its
module (`<unit>-<stamp>.dll` / `.so`) naming everything the compile read: the
unit's text and compile options (the key the [code cache](code-cache.md)
uses: host globals, natives manifest, module registry, environment), the
digest of every source file and how each import resolved, and the bronze
runtime ABI. Before a unit runs, the record is checked against the unit as it
is now and the module's own ABI stamp against the engine's. Any difference
(an edited script or import, another engine build, another host-global list)
and the unit compiles in process as a folder launch would, the log says why
(`aot: the ahead-of-time module for ... is out of date (...)`), and a
`bro --build-exe` starts in the background, so the next launch has a current
module. A module never runs a program other than the one the JIT would
compile.

Compiled code is a first-class citizen of the runtime: each module registers
its code ranges and stack maps when it loads, so stack traces name its
functions and lines, the GC walks its frames, and `bro.profiler` reports its
functions with tier `aot`. `eval`, `new Function` and `import()` of code not in
a module still compile in process. `bro.app.startup.page.codeCache` is `'aot'`
when the page's first unit ran from its module.

**The folder is unaffected.** `bro <dir>` never looks at an AOT directory: the
edit loop, hot reload and the code cache work as before.

**Where it can move.** The images are position-independent and name only the
engine's runtime library and `$ORIGIN` (Windows: the loader's own search), so
they carry no build-machine paths of their own. They do name the app folder:
module URLs, `import.meta.url` and stack-trace file names are the absolute
paths the page had at build time, and the record checks them. An AOT
directory therefore belongs with an app folder at the path it was built for
(the installed copy, which is what `--install` builds against); a release
artifact that ships one prebuilt needs app-relative names first.

macOS app executables are not built yet (apps are bundles there), so no AOT
directory is either; the writer handles Mach-O, and the plan is the same.

## Startup cost

`bro <dir>` compiles the page's scripts in process at boot (bronze, tiered
JIT). The compiled code is cached on disk ([code-cache.md](code-cache.md)), so
the second and later launches skip the compile. An installed app's executable
runs its page from modules compiled at install ("Ahead-of-time scripts",
above), and shipping an `app.dll` beside `index.html` skips the compile for a
folder too.

Every launch logs its time to the first presented frame, measured from just
before `main()`: `app <id>: first frame N ms after start` (in the app's log
file).
