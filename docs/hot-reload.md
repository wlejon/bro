# Hot reload: the edit loop

Run an app from its folder, edit a source file, and the app reloads with the
new code a fraction of a second later. This is what bro does for a windowed
app whose `<script>` tags it compiles in-process (a folder without an
`app.dll`); nothing to install, nothing to configure.

```
bro ../broworkshop/games/torque     # boots, then watches the folder
```

## What triggers a reload

- A `.js` / `.mjs` / `.cjs` / `.html` / `.htm` / `.css` file changes
  anywhere under the app dir or the project's `/lib` (not inside a dot
  directory).
- The `system_reload_app` action (default **F5**; rebindable like any engine
  action, see `docs/settings.md`).
- `location.reload()` from the page.

Edits settle for 150 ms before the reload runs, so an editor that saves in
several steps causes one reload, not three. Saves, settings files, JSON,
assets and anything under `.git` never trigger one: the watcher is for the
app's source, and an app that writes into its own folder at run time does not
reload itself.

The reload is `Engine::performAppReload`: the DOM, canvases, WebGL and scene
contexts, timers, expandos and the realm's globals are torn down, the old
program's GC roots are retired (bronze `unloadModule`, so the previous heap
graph dies rather than leaking per reload), and `index.html` and its scripts
are loaded and compiled again. App state does not survive; that is what a
reload is.

## How the reloaded code runs

Boot and every reload compile the same way: bronze's tiered default (see
bronze `docs/dynamic-eval.md`). The app's scripts are translated without the
optimizer and start in brass's interpreter, so a reload is running as soon as
the translation is done; functions that get hot are compiled to baseline
code and then optimized in the background, and a hot loop moves into
optimized code while it runs. The optimizer's cost is paid only for the code
that is hot, and it is paid off the frame.

`BRO_JIT_TIER` in the environment pins one tier for the whole process, for
debugging a tier: `0` (interpreter only), `1` (every function
baseline-compiled up front), `2` (the whole program optimized before it
runs) or `auto` (the default). It applies to every `bro` executable and to
programs that embed `bro_engine`.

## Turning the watcher off

- `"watch": false` in the app's `bro.json`.
- `BRO_WATCH=0` in the environment (`BRO_WATCH=1` forces it on where the
  manifest turned it off).

It is never on for `bro-headless` (a test's driver owns its reloads), for
`bro-server`, or for an app that carries an `app.dll` (that app is rebuilt
and relaunched by its own build).

## Where the pieces live

- `src/engine/app_reload.cpp` — the watcher (brokit's `FsWatcher`, the same
  one behind `fs.watch`), the settle timer and `requestAppReload`.
- `src/bronze_host/eval_jit.cpp` — the in-process compiles, and the
  `BRO_JIT_TIER` override (`applyJitTierOverride`, applied when an `Engine`
  starts).
- bronze `src/eval` and `BrassTieredEngine` — the tiers themselves.
