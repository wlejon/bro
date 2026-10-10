# Project manager

When `bro` is launched with no app argument and no `bro.json`/`index.html` next to the executable, it falls through to the built-in **project manager** at `system/projects/`. This is the default home screen for plain bro releases.

`system/projects/` has its own `bro.json`, so it's a self-contained app, not a [system panel](system-panels.md), the panel scanner skips any `system/<dir>/` that contains one.

## What it does

- Lists projects from a per-user registry, sorted by last opened.
- Click a tile to launch (`bro <projectPath>` in a detached child process).
- **New project**: copies one of the skeletons under `system/skeletons/<name>/` into a folder you pick.
- **Open existing**: pick any folder containing `bro.json` or `index.html`; it's added to the registry.
- **Drag-and-drop**: drop a folder onto the window to register it in place; drop a `.zip` to extract (PowerShell `Expand-Archive` on Windows, `unzip`/`tar` elsewhere) into a destination you pick, then register.
- **Remove** (the × on hover) drops a project from the registry without deleting any files.

## Where state lives

| Platform | Registry path |
|---|---|
| Windows | `%APPDATA%\bro\projects.json` |
| macOS | `~/Library/Application Support/bro/projects.json` |
| Linux | `${XDG_DATA_HOME:-~/.local/share}/bro/projects.json` |

The registry is plain JSON: `{ "projects": [{ "path", "name", "lastOpened" }, ...] }`. Projects sit wherever you create them, the manager only stores paths.

## Skeletons

Found at `system/skeletons/<name>/`. Each is a complete, runnable bro app the new-project flow copies verbatim.

| Skeleton | What it shows |
|---|---|
| `blank` | HTML + CSS + JS only. Input + add button + dynamic list. |
| `canvas2d` | `<canvas>` + `requestAnimationFrame` loop. Draggable circle with a fading trail. |
| `scene3d` | `bro.scene` minimal: camera, lights, rotating cube on a ground plane. |
| `ai` | Local LLM chat via `bro.lm` (`bro.lm` text generation + model picker). |
| `headless-tool` | CLI script meant to be driven by `bro-headless . run.js`. |

To add another skeleton, drop a folder under `system/skeletons/`. Any subdirectory containing a `bro.json` is picked up automatically by the new-project picker.

## Bypassing the manager

The manager only fires when bro can't otherwise resolve an app:

```bash
bro path/to/app                  # explicit app dir
bro path/to/project/bro.json     # explicit project manifest
bro                              # no args + no bro.json next to exe → project manager
```

Placing a `bro.json` or `index.html` next to `bro.exe` short-circuits the manager and runs that instead. Useful for redistributing a bro app as a single zip.

## A project holding several apps

A project root is a folder whose `bro.json` carries project keys (`default_app`, `lib`, `system`). Its `lib` directory (default `lib/`) is mounted at `/lib` and its `system` directory at `/system` for every app inside it, so sibling apps share modules and styles by absolute path (`import { h } from '/lib/dom.js'`). `{"lib": "shared"}` mounts `shared/` instead. [helmapps](https://github.com/wlejon/helmapps) is laid out this way: one project, an app per folder, a kit in `lib/`.

An app launched by its own folder finds its project like this:

1. the project manifest it was launched through, if any;
2. else the nearest ancestor folder whose `bro.json` has project keys, starting from the app's real path, so an app reached through a symlink (`bro --install --link`) still finds the project its checkout lives in;
3. else `BRO_PROJECT_ROOT`, which a bro process sets for everything it starts, but only when the app lies inside that root. The variable reaches a terminal's shells too, so an unrelated app started from one does not take the other project's `/lib`.

`bro --install` (copy) bundles the project's `lib` into the installed copy as `<id>/lib`, so the copy keeps working away from its project; an app with its own `lib/` keeps that instead. See [apps.md](apps.md#installing-and-how-launchers-find-apps).
