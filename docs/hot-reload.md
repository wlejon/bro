# Hot reload: the edit loop

Run an app from its folder, edit a source file, and the app reloads with the
new code a fraction of a second later. This is what bro does for a windowed
app whose `<script>` tags it compiles in-process (a folder without an
`app.dll`); nothing to install, nothing to configure.

```
bro ../broworkshop/games/torque     # boots, then watches the folder
```

## What triggers a reload

| Trigger | Kind | Tier the app recompiles in |
|---|---|---|
| A `.js` / `.mjs` / `.cjs` / `.html` / `.htm` / `.css` file changes anywhere under the app dir or the project's `/lib` (not inside a dot directory) | Dev | baseline |
| `system_reload_app` action (default **F5**; rebindable like any engine action, see `docs/settings.md`) | Dev | baseline |
| `location.reload()` from the page | Navigation | whatever boot used |

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

## The two compile tiers

bronze compiles in-process through brass. Its optimizer is the reason a
compiled app runs as fast as it does, and also most of what a compile costs:
for torque (146 KB of JS) the optimized compile is ~3 s and the baseline
one — same code, same semantics, optimizer skipped — ~0.5 s.

- **Boot** and `location.reload()` compile optimized. What runs is what
  ships.
- The first **Dev** reload switches the session to the baseline tier, and it
  stays there: once you are iterating, every reload is fast, and the code you
  are looking at has consistent performance from one save to the next. Expect
  it to run slower than the optimized build (inlining, GVN, bounds-check
  elimination and the loop optimizer are all off). Relaunch to get the
  optimized build back.

`BRO_JIT_TIER=baseline` or `BRO_JIT_TIER=optimized` in the environment pins
the tier for the whole process, boot included. `baseline` is the one to use
when launch time matters more than frame time (a headless test that compiles
a lot of code, a quick look at an app); `optimized` when you want to reload
and measure.

## Turning the watcher off

- `"watch": false` in the app's `bro.json`.
- `BRO_WATCH=0` in the environment (`BRO_WATCH=1` forces it on where the
  manifest turned it off).

It is never on for `bro-headless` (a test's driver owns its reloads), for
`bro-server`, or for an app that carries an `app.dll` (that app is rebuilt
and relaunched by its own build).

## Where the pieces live

- `src/engine/app_reload.cpp` — the watcher (brokit's `FsWatcher`, the same
  one behind `fs.watch`), the settle timer, `requestAppReload(kind)` and the
  tier choice (`Engine::jitOptimize`).
- `src/bronze_host/eval_jit.cpp` — passes the tier to bronze as
  `EvalOptions::optimize`.
- bronze `BrassBackend::setOptimize` / `BRONZE_NO_OPT=1` — the tier itself;
  `BRONZE_NO_OPT` forces the baseline tier for every bronze compile in a
  process, which is how the oracle suite is run against it.
