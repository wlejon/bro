# Hot reload

A windowed app whose `<script>` tags bro compiles in-process (a folder without an `app.dll`) reloads when its source changes.

```
bro ../broworkshop/games/torque     # boots, then watches the folder
```

## Triggers

- A `.js` / `.mjs` / `.cjs` / `.html` / `.htm` / `.css` file changes under the app dir or the project's `/lib` (not in a dot directory). Changes settle for 300 ms first, so a multi-step save or a checkout is one reload. JSON, assets and settings files never trigger one.
- The `system_reload_app` action (default **F5**, rebindable; [settings.md](settings.md)).
- `location.reload()`.

App state does not survive a reload.

A watcher reload is held back while the tree looks incomplete (index.html missing, a script it names missing, or a static import naming a file or export that does not exist). The page stays up and the log says why: `Not reloading yet, the source looks incomplete: js/shell.js imports NotificationCenter from js/notify.js, which does not export it`. F5 and `location.reload()` are never held back.

A reload that fails (index.html unreadable, a script throws at top level) cannot restore the old page: the error shows in a red banner, or as an error page when nothing loaded. Fix the source and save.

Unchanged script units load from the [code cache](code-cache.md); while a unit compiles the window keeps drawing the static markup ([compile-progress.md](compile-progress.md)).

## Turning it off

- `"watch": false` in `bro.json`.
- `BRO_WATCH=0` (`BRO_WATCH=1` forces it on over the manifest).

It is never on for `bro-headless`, `bro-server`, or an app with an `app.dll`.

## JIT tier

`BRO_JIT_TIER` pins one tier for the whole process, for debugging: `0` interpreter only, `1` every function baseline-compiled up front, `2` the whole program optimized before it runs, `auto` (default) tiered.
