# System panels

bro ships a handful of engine-level UI overlays — the menu bar, the perf HUD, the preferences modal and its settings tabs, the startup splash screen, and the DOM inspector. These are **system panels**: HTML files rendered through the same layout/CSS/Skia pipeline as an app's own document, but each with its own `dom::Document` and its own `JSContext` on the shared QuickJS runtime.

This doc is the single reference for how that layer works and how to author or override a panel. It replaces scattered mentions in `docs/settings.md` and `docs/menu-api.js`, which now link here instead of describing `__bro` inline.

> **Not to be confused with:** `system/inspector.html` (this doc) is a visual DOM-tree overlay panel toggled from the View menu. `docs/inspect.md`'s `inspect()`/`inspectTree()`/`computedStyle()` globals are a completely different, headless-only scripting API for querying layout from a `bro-headless` script. Same-ish name, unrelated code paths.

## How panels are discovered

At startup, `Engine::initSystemPanels()` scans two directories, in order:

1. `system/` — the engine-shipped panels (this repo's `system/` directory).
2. `<appDir>/system/` — the app's own override directory, if it has one.

Both scans match panels by relative path (`menu.html`, `settings/graphics.html`, etc.). If the app directory provides a file at the same relative path as an engine one, the app's version wins — same mechanism apps use to add new settings tabs (drop a file at `<appDir>/system/settings/gameplay.html`) or replace a built-in panel outright (drop a file at `<appDir>/system/menu.html`).

**Exception:** a subdirectory containing its own `bro.json` (e.g. `system/projects/`, the built-in project manager — see [projects.md](projects.md)) is treated as a self-contained app, not a panel, and is skipped entirely by this scan.

## Built-in panels

| Panel | File | Visibility flag | Shown via |
|---|---|---|---|
| Menu bar | `system/menu.html` | `MenuBar::visible` | `bro.menu.show()` (app-facing, opt-in; hidden by default) |
| Perf HUD | `system/perf.html` | overlay toggle | `system_toggle_perf` action (default `F8`) |
| Preferences modal | `system/nav.html` + `system/settings/{graphics,audio,input}.html` | `Engine::isSystemVisible()` | `system_toggle_settings` action, or the `__system.preferences` menu item |
| Splash screen | `system/splash.html` | `Engine::splashVisible_` | shown automatically at startup if `splashEnabled_`; dismisses itself |
| DOM inspector | `system/inspector.html` | `Engine::inspector().visible` | the `__system.inspector` menu item |

## Panel JS context: what's available, what isn't

Each panel gets a full `JSContext` sharing the engine's QuickJS runtime, with the same DOM/Canvas 2D bindings as an app document (`document`, `console`, timers, `requestAnimationFrame`, Canvas 2D). Notably:

- **`bro.settings.*`** is available (it's installed on every context via `SettingsBindings`), so panels can read/write settings directly — see [settings.md](settings.md).
- **`bro.menu.*`** (the app-facing menu tree *mutation* API — `set`, `addItem`, `updateItem`, `on`, etc., documented in [menu-api.js](menu-api.js)) is **not** available here. It's installed only on the app's own `JSContext`. Panels read/dispatch the menu through `__bro.menu` instead (below) — a much narrower, render-only surface.
- **ES modules are not supported.** Each panel is a separate `JSContext`, so `<script type="module">` is skipped with a `LOG_WARN` rather than misevaluated. Use classic scripts (inline or `<script src="...">`) and share code via plain functions attached to `window`.

### Lifecycle hooks

A panel's script can define any of these on `window`; the engine calls them when relevant:

| Hook | Called when |
|---|---|
| `window.__onResize` | The viewport or window size changes |
| `window.__onMenuChanged` | The menu tree is mutated (via `bro.menu.*` from app JS) |
| `window.__onInspectorChanged` | Inspector state changes (selection, dock, size, picker mode) |
| `window.__onDismiss` | Sent to the splash panel when the engine decides it's time to swirl away |
| `window.__onPanelsReady` | Fired once, after every system panel has finished loading (used by `nav.html` to build tabs, since settings panels may still be loading when `nav.html` itself loads) |

## `<script src>` support

Panels support both inline `<script>` bodies and `<script src="...">`, resolved relative to the panel's own directory (via the same `AppLoader::resolvePath`/`loadFile` used for app documents). This is how the settings/nav panels share layout code — see `PanelLayout` below.

Put shared helpers in `system/lib/` and reference them by **relative path** (`lib/panel-runtime.js` from `nav.html`; `../lib/panel-runtime.js` from `settings/*.html`) rather than through the `/system` asset mount. The mount resolves to `<appDir>/system` (or the project-root `system`), which is a *different* directory than the `system` scan root — they usually coincide but aren't guaranteed to, so a relative path sidesteps the ambiguity.

## `PanelLayout` — shared modal geometry

`system/lib/panel-runtime.js` exposes `window.PanelLayout`, used by `nav.html` and all three settings tabs so the preferences modal's card/sidebar/content geometry is defined exactly once:

```js
PanelLayout.CARD_W   // 720
PanelLayout.CARD_H   // 520
PanelLayout.SIDEBAR_W // 180
PanelLayout.HEADER_H  // 44

PanelLayout.cardRect(vp)     // { left, top, width, height } — the whole modal card
PanelLayout.contentRect(vp)  // { left, top, width, height } — inside the card, right of the sidebar, below the header

PanelLayout.positionCard(cardEl, backdropEl)  // nav.html: position the card + size the backdrop
PanelLayout.positionContent(panelEl)          // settings/*.html: position this tab's content

PanelLayout.onResize(fn)  // call fn() now, then wire it as window.__onResize
```

Before this helper existed, every settings-panel file hand-copied `CARD_W`/`CARD_H`/`SIDEBAR_W`/`HEADER_H` and its own positioning math. A panel authored today should load `panel-runtime.js` and call `PanelLayout.onResize(...)` instead of re-deriving these constants — see any file under `system/settings/` for the pattern.

## `__bro` reference

`__bro` is the engine-to-panel bridge, installed per-panel by `Engine::installBroObject` (`src/engine/system_panels.cpp`). It's grouped by concern:

### `__bro.perf` / `__bro.viewport`

Plain data objects (not namespaced further): `perf.{fps, frameTime, js, layout, raster, gpu, draw}`, `viewport.{width, height}`, refreshed by the engine each frame.

### `__bro.menu`

| Function | Description |
|---|---|
| `__bro.menu.getTree()` | The current menu tree, parsed from `MenuBar::toJSON()` |
| `__bro.menu.click(id)` | Dispatch a menu action by id (routes through `Engine::triggerMenuAction`) |
| `__bro.menu.getHeight()` | The authoritative bar height in px (`MenuBar::height`) — also drives `Engine::contentInsets()`, so `menu.html` sizes `#menu-bar` from this instead of a hardcoded value |

### `__bro.settingsUI`

The preferences-modal chrome — distinct from `bro.settings.*` (the settings *values* API, also available in this context since it's installed on every panel).

| Function | Description |
|---|---|
| `__bro.settingsUI.show(name)` | Switch the active settings tab |
| `__bro.settingsUI.getAllPanels()` | Every panel with a tab label, any group — `[{name, tabLabel}]` |
| `__bro.settingsUI.getActivePanel()` | The currently active panel name |
| `__bro.settingsUI.toggle()` | Open/close the preferences modal |
| `__bro.settingsUI.isVisible()` | Whether the modal is open |
| `__bro.settingsUI.getSettingsPanels()` | Panels in the `"settings"` group only — `[{name, label}]`; what `nav.html` builds tabs from |
| `__bro.settingsUI.getViewport()` | `{width, height, contentTop}` — current viewport metrics, consumed by `PanelLayout` |

### `__bro.splash`

| Function | Description |
|---|---|
| `__bro.splash.dismiss()` | Called by `splash.html` once its swirl-away animation finishes; hides the splash panel |

### `__bro.inspector`

| Function | Description |
|---|---|
| `__bro.inspector.getLayout()` | `{visible, dock, width, height, pickerMode, viewportWidth, viewportHeight, menuTop}` |
| `__bro.inspector.getAppTree(maxDepth?)` | Full app-document tree from `documentElement`, or `maxDepth` levels deep; resets the per-fetch node-id map |
| `__bro.inspector.getAppChildren(nodeId)` | One level of children for a given node id |
| `__bro.inspector.select(nodeId)` | Select a node by id |
| `__bro.inspector.setDock("right"\|"bottom")` | Dock the panel |
| `__bro.inspector.setSize(px)` | Resize the panel along its docked axis |
| `__bro.inspector.setPickerMode(bool)` | Toggle click-to-pick-a-node mode |
| `__bro.inspector.getSelected()` | `{id, tag}` for the current selection, or `null` |
| `__bro.inspector.toggle()` | Same effect as the View → Inspector menu item |

## Testing panels

There's no automated test suite for this layer — coverage is manual, build + `bro.exe`/`bro-headless.exe` + `screenshot()`. Two things to know if you're scripting a check:

- **`bro-headless` suppresses the menu bar** regardless of `MenuBar::visible`, so screenshots stay consistent with a no-menu viewport — you can't screenshot-verify the menu bar from a headless script.
- **System-panel DOMs are not reachable from the app-level headless globals.** `document.querySelector`/`inspectTree()` only see the app's own document — each panel is a separate `Document`/`JSContext`. To drive a panel from a headless script, use screen-coordinate `click(x, y)` (routed through the same input path as a real click) rather than DOM queries, and `screenshot()` to verify the result visually.
