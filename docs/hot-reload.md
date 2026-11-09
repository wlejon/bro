# Hot reload

A windowed app whose `<script>` tags bro compiles in-process (a folder without an `app.dll`) reloads when its source changes.

```
bro ../broworkshop/games/torque     # boots, then watches the folder
```

## Triggers

- A `.js` / `.mjs` / `.cjs` / `.html` / `.htm` / `.css` / `.bk` file changes under the app dir or the project's `/lib` (not in a dot directory). Changes settle for 300 ms first, so a multi-step save or a checkout is one reload. JSON, assets and settings files never trigger one.
- The `system_reload_app` action (default **F5**, rebindable; [settings.md](settings.md)).
- `location.reload()`.

A change of stylesheets alone (every changed file a `.css`) does not reload: the page's linked sheets are read again and swapped into its cascade, `@import`ed sheets included, and the page restyles with its DOM and script state as they were (the log says `Stylesheets changed: restyled the running page`). A `<style>` element's text is the page's own and stays. A page with an `<iframe>` reloads instead, since a frame's sheets come from the app dir too.

## Keeping state across a reload

A reload starts a fresh page in a fresh realm, so nothing in the old page's heap survives. The old page's promise jobs already queued run first, and its Workers end with it (a reply one sent just before is dropped), so none of its code runs after it is gone. Before it goes, the window gets `pagehide` (any reload: the watcher, F5, `location.reload()`), and `sessionStorage` outlives the reload, as in a browser: an app puts what the user is in the middle of there and reads it back as it boots.

```js
addEventListener('pagehide', () => {
    sessionStorage.setItem('draft', JSON.stringify({ path: current.path, scroll: list.scrollTop }));
});
const draft = JSON.parse(sessionStorage.getItem('draft') || 'null');
if (draft) { openPath(draft.path); list.scrollTop = draft.scroll; }
```

`sessionStorage` lasts as long as the process (it is not written to disk); `localStorage` is for what should outlive it.

A `<terminal>`'s shell would end with its element; `terminal.park(key)` in the `pagehide` handler hands the running session to the engine, and `terminal.adopt(key)` on a terminal of the new page takes it back: the same process, shell state, screen and scrollback, its events going to the new element from then on. Note the keys in `sessionStorage`. A parked session the new page does not adopt within two frames and two seconds of booting is ended ([terminal-api.js](terminal-api.js), "Across a reload").

The page gets `pagehide` once more when the app quits (the window closed, `bro.quit()`, Ctrl+C, the end of a headless script), as a closed tab's does, while everything it reaches still runs. What must outlive the process is written there synchronously (`fs.writeFileSync`): the process exits after the handlers return, so a promise they start does not finish.

## Taking a change in place

Before a watcher reload, the window gets a cancelable `sourcechange` event whose `detail.files` names the changed files from the app dir (`kernels/relief.bk`; a file of the project's lib as `/lib/...`). `preventDefault()` keeps the running page, so an app can swap the change in itself: new kernel sources through `bro.kernel.reload`, hook sources through `scene.reloadShaders` ([kernel-api.js](kernel-api.js), [scene-api.js](scene-api.js)), and whatever it made from the changed kernels made again. A change of unknown files (a burst the watcher dropped, the folder replaced) reloads without the event; F5 and `location.reload()` never fire it.

```js
window.addEventListener('sourcechange', (e) => {
    if (!e.detail.files.every((f) => f.endsWith('.bk'))) return;   // a script changed: reload
    e.preventDefault();
    const read = (f) => require('fs').readFileSync(bro.resolvePath('/app/' + f), 'utf-8');
    const sources = Object.fromEntries(e.detail.files.map((f) => [f, read(f)]));
    bro.kernel.reload(sources);
    scene.reloadShaders(sources);
});
```

An app with many `.bk` files can take them this way: a `reload(files)` of its own names each by file and as a module, then rebuilds whatever the changed kernels feed (a planet's rivers, tiles, clouds, grass and trees, say).

A watcher reload is held back while the tree looks incomplete (index.html missing, a script it names missing, or a static import naming a file or export that does not exist). The page stays up and the log says why: `Not reloading yet, the source looks incomplete: js/shell.js imports NotificationCenter from js/notify.js, which does not export it`. F5 and `location.reload()` are never held back.

A reload that fails (index.html unreadable, a script throws at top level) cannot restore the old page: the error shows in a red banner, or as an error page when nothing loaded. Fix the source and save.

Unchanged script units load from the [code cache](code-cache.md); while a unit compiles the window keeps drawing the static markup ([compile-progress.md](compile-progress.md)).

## Turning it off

- `"watch": false` in `bro.json`.
- `BRO_WATCH=0` (`BRO_WATCH=1` forces it on over the manifest).

An app that is not watched also gets **F5** for itself: `system_reload_app` is defined but unbound, so the key reaches the page as an ordinary `keydown` (a file manager's refresh, say). An app or user can still bind the action to a key in [settings](settings.md).

It is never on for `bro-headless`, `bro-server`, or an app with an `app.dll`.

## Scene shaders without a rebuild

The 3D scene's built-in shaders (`src/scene/vulkan/shaders/**/*.bk`) are embedded in the binary and compiled to SPIR-V in process by brass. For a shader edit loop, read them from the source tree at start-up instead:

```
BRO_SCENE_SHADER_SOURCE=1 bro-headless ../broworkshop/demos/lighting-demo -e "screenshot('out.png')"
BRO_SCENE_SHADER_SOURCE=D:/work/shaders bro ...     # a directory other than the build's tree
```

`1` reads the tree this binary was built from; any other value is a directory. Each `.bk` file is read from that directory when brass compiles the scene's shaders and custom module hooks (`bro_vulkan_scene_test` checks every stage). It is read once per process: edit, then run again. The persistent pipeline cache is keyed by the driver on the SPIR-V itself, so an edited shader never picks up a stale pipeline.

## JIT tier

`BRO_JIT_TIER` pins one tier for the whole process, for debugging: `0` interpreter only, `1` every function baseline-compiled up front, `2` the whole program optimized before it runs, `auto` (default) tiered.
