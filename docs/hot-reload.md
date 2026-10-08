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

Edits settle for 300 ms before the reload runs, so an editor that saves in
several steps causes one reload, not three, and a git checkout or a build
step that copies a UI into place causes one reload of the finished tree.
Saves, settings files, JSON, assets and anything under `.git` never trigger
one: the watcher is for the app's source, and an app that writes into its own
folder at run time does not reload itself.

A watcher-triggered reload is preflighted first (`checkAppSourceConsistency`,
`src/engine/source_preflight.cpp`): index.html must be there, every script it
names must exist, and the static import graph of its module scripts must be
whole, every relative import naming a file that exists and exports what is
imported from it. A tree caught half-way through a checkout fails that, and
the reload is held back: the running page stays up, the log says why
(`Not reloading yet, the source looks incomplete: js/shell.js imports
NotificationCenter from js/notify.js, which does not export it`), and the next
change looks again. F5 and `location.reload()` are never held back.

If the reload does go ahead and fails (index.html cannot be loaded, or a
script of the new page throws at its top level), the previous page cannot be
brought back (its realm is torn down before the new one is built), so the
failure is logged and, in a window or under DRM, shown in a red banner over
whatever the new page drew, or as an error page when nothing loaded at all,
rather than leaving a blank window. Fix the source and save to reload again.

The watcher survives the operations that replace a tree wholesale: a burst
bigger than its event ring counts as a change, and when the watched folder
itself is removed and recreated (a deploy step that replaces it), a new
watcher goes on it once it is back.

The reload is `Engine::performAppReload`: the DOM, canvases, WebGL and scene
contexts, timers, expandos and the realm's globals are torn down, the old
program's GC roots are retired (bronze `unloadModule`, so the previous heap
graph dies rather than leaking per reload), the realm's module registry is
cleared, and `index.html` and its scripts are loaded and compiled again. App
state does not survive; that is what a reload is.

The registry is what makes a later compilation unit bind the module instances
the page already evaluated (bronze `runtime/module_registry.h`). Kept across a
reload, it made the new page's compile treat every file the old page imported
as external and bind the old instance, so an edited module never loaded and a
renamed export read `undefined` (a restructured shell came back blank with
`undefined is not a constructor`).

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

Each script unit a reload compiles consults the on-disk code cache
(`docs/code-cache.md`): a unit whose files did not change is loaded, not
compiled, and the one you edited is recompiled whole (bronze infers types over
a unit's entire module graph). While a unit compiles, the window keeps drawing
the page's static markup (`docs/compile-progress.md`).

## Turning the watcher off

- `"watch": false` in the app's `bro.json`.
- `BRO_WATCH=0` in the environment (`BRO_WATCH=1` forces it on where the
  manifest turned it off).

It is never on for `bro-headless` (a test's driver owns its reloads), for
`bro-server`, or for an app that carries an `app.dll` (that app is rebuilt
and relaunched by its own build).

## Where the pieces live

- `src/engine/app_reload.cpp` — the watcher (brokit's `FsWatcher`, the same
  one behind `fs.watch`), the settle timer, `requestAppReload` and
  `performAppReload`.
- `src/engine/source_preflight.cpp` — the consistency check a watcher reload
  runs first (tested by `bro_source_preflight_test`).
- `src/bronze_host/eval_jit.cpp` — the in-process compiles, and the
  `BRO_JIT_TIER` override (`applyJitTierOverride`, applied when an `Engine`
  starts).
- bronze `src/eval` and `BrassTieredEngine` — the tiers themselves.
