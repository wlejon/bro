# Plan: System Settings UI Panels

## Goal

Build interactive settings UI panels within the system overlay (F8), organized as `system/settings/graphics/index.html`, `system/settings/audio/index.html`, `system/settings/input/index.html`. Apps can override any panel by placing their own version at `settings/graphics/index.html` in their app directory.

## Current State

### SystemOverlay architecture (src/engine/system_overlay.h/.cpp)
- Panels are subdirectories under `system/` with `index.html`, auto-discovered by `loadPanels()`
- Each panel gets its own JSContext with DOM bindings + `__bro` object (perf data, viewport)
- Panels render to a CPU Skia surface, uploaded to GL texture, composited as fullscreen overlay
- `SystemRenderer` (CPU Skia) handles all panel rendering
- Panel JS gets: `brokit::api::installConsole()`, `js::Timers::install()`, `js::DomBindings::install()`, `__bro` object
- **No mouse/keyboard input** — panels are display-only, no event forwarding
- **All panels toggle together** with F8 — no per-panel visibility or tabs
- Panel loading: scan dirs, extract `<style>` via regex, parse HTML, create JSContext, layout, extract+eval `<script>` blocks
- `installBroObject()` creates `__bro.perf` and `__bro.viewport` on each panel's JSContext

### Settings system (just built)
- `Settings` class in `src/engine/settings.h/.cpp` — three-layer priority, persistence to `.bro_settings.json`
- JS API: `bro.settings.*` (get/set/getAll/setDefault/reset, action bindings, display modes)
- Installed via `SettingsBindings::install(ctx, settings, window)` in `src/js/settings_bindings.cpp`
- Runtime changes fire a callback in Engine that calls SDL/broaudio APIs

### Key files
- `src/engine/system_overlay.h` — SystemOverlay class, Panel struct, SystemRenderer class
- `src/engine/system_overlay.cpp` — loadPanels(), installBroObject(), render(), tick(), toggle()
- `src/engine/engine.cpp:~566` — SystemOverlay construction + loadPanels("system")
- `src/engine/engine.cpp:~1450` — Overlay compositing in render loop
- `src/engine/engine.cpp:~1285` — Overlay tick in main loop
- `src/engine/input_handling.cpp:~952` — F8 toggle, no event forwarding
- `system/perf/index.html` — Reference panel (300px wide, absolute top-right, polls `__bro.perf`)
- `src/js/settings_bindings.cpp` — Settings JS bindings (need to install on overlay JSContexts too)

## Design

### 1. Panel organization

```
system/
  perf/index.html          (existing — performance monitor)
  settings/
    graphics/index.html    (new — resolution, fullscreen, vsync, fps cap)
    audio/index.html       (new — master/music/sfx volume, mute)
    input/index.html       (new — key bindings, scroll speed, double-click)
```

Each settings subcategory is its own panel with its own JSContext, just like perf.

### 2. App overrides

When loading panels, check the app directory first. If `<appDir>/settings/graphics/index.html` exists, use it instead of `system/settings/graphics/index.html`. This gives apps full control over individual settings panels.

Implementation: in `loadPanels()`, after discovering system panels, check for app-dir overrides. Need to pass `appDir` to SystemOverlay (or do the override resolution in Engine before calling loadPanels).

### 3. Mouse event forwarding to overlay

When the overlay is visible, mouse events should be forwarded to overlay panel DOMs:

- In `handleMouseDown/Up/Move`, check `systemOverlay_->isVisible()` first
- Hit-test overlay panel DOMs (each panel has its own Document)
- If hit, dispatch MouseEvent to the overlay panel's DOM element and **consume** the event (don't pass to app)
- If no hit (clicked on transparent area), pass through to app as normal
- Overlay panels need their own `dispatchDomEvent()` calls using the panel's JSContext

Key challenge: overlay panels use a separate JSContext from the app, so `js::dispatchDomEvent()` needs the panel's context, not the app's.

Implementation approach:
- Add `SystemOverlay::handleMouseDown(x, y, button) -> bool` (returns true if consumed)
- Internally: iterate panels, hit-test each panel's Document, dispatch event if hit
- In Engine's input handlers: call overlay handler first, skip app handling if consumed

### 4. Tab/panel navigation

The overlay needs a way to switch between panels (perf, settings/graphics, settings/audio, settings/input). Options:

**Option A: Tab bar panel** — A dedicated `system/nav/index.html` that renders tabs at the top. Clicking a tab shows/hides other panels. Requires inter-panel communication.

**Option B: Engine-managed tabs** — The overlay itself manages tab state. Each panel declares its tab label. The overlay renders a tab bar natively (or via a special nav panel) and controls which panel is visible.

**Option C: Keyboard cycling** — F8 cycles through panels (perf -> graphics -> audio -> input -> hidden). Simple, no mouse needed for navigation.

**Recommended: Option B** — Engine manages tab visibility. Add to Panel struct: `std::string tabLabel; bool active = false;`. SystemOverlay renders a simple tab bar (could be its own panel or hardcoded). Tab clicks switch `active` flag. Only active panels render their content. This keeps panels independent.

Actually, simplest practical approach: **a single settings panel** that has its own internal tabs (graphics/audio/input sections), plus the existing perf panel. F8 toggles overlay, overlay shows perf + settings side by side (or stacked). Settings panel handles its own tab switching internally via JS.

Wait — the user specifically wants `system/settings/graphics/index.html` etc. as separate panels for override-ability. So we need per-panel visibility.

**Revised approach**: 
- Add a `system/nav/index.html` that renders a tab bar
- Nav panel communicates with SystemOverlay via `__bro.nav` object
- `__bro.nav.activePanel` tracks which panel is shown
- `__bro.nav.panels` lists available panels with labels
- Nav panel has click handlers that set `__bro.nav.activePanel`
- SystemOverlay checks `__bro.nav.activePanel` before rendering each content panel
- Simpler alternative: just expose `__bro.showPanel(name)` as a C function bound on the panel context

### 5. Settings data bridge to overlay panels

Overlay panels need to read/write settings. Two options:

**Option A**: Install `SettingsBindings` on each panel's JSContext (reuse existing code)
- Pro: Full API available, same code path
- Con: Need to pass Settings* and Window* to each panel context

**Option B**: Bridge via `__bro.settings` as a simpler read/write interface
- Pro: Lighter weight
- Con: Duplicates API surface

**Recommended: Option A** — Just call `SettingsBindings::install(panel.jsCtx, settings, window)` during panel loading. Panels then use the same `bro.settings.*` API that apps use. Requires passing Settings* and Window* to SystemOverlay.

### 6. Detailed implementation plan

#### Phase 1: Mouse event forwarding

1. Add to `SystemOverlay`:
   ```cpp
   bool handleMouseDown(float x, float y, int button);
   bool handleMouseUp(float x, float y, int button);
   bool handleMouseMove(float x, float y);
   ```
   Each method: iterate panels, perform hit test on panel Document, dispatch event if hit, return true if consumed.

2. In `Engine::handleMouseDown/Up/Move` (input_handling.cpp): check `systemOverlay_->isVisible()` and call overlay handler first. If it returns true, return early (don't dispatch to app).

3. Need to track hover state per-panel for mouseenter/mouseleave.

#### Phase 2: Panel visibility control

1. Add to Panel struct: `std::string tabLabel; bool active = true;`
2. Perf panel: always active when overlay visible
3. Settings panels: only one active at a time
4. Add `SystemOverlay::showPanel(name)` and `SystemOverlay::getActivePanel()`
5. Expose via `__bro.showPanel(name)` C function on panel contexts
6. Skip inactive panels in `render()` and `tick()`

#### Phase 3: Navigation panel

Create `system/nav/index.html`:
- Horizontal tab bar at top of screen
- Tabs: Perf | Graphics | Audio | Input
- Click handler calls `__bro.showPanel('settings/graphics')` etc.
- Styled as a thin bar, always visible when overlay is open
- Nav panel is always active (never hidden)

#### Phase 4: Settings bindings in overlay

1. Pass `Settings*` and `Window*` to SystemOverlay constructor (or add a method)
2. In `loadPanels()` or `installBroObject()`, call `SettingsBindings::install(panel.jsCtx, settings_, window_)` for each panel
3. Panels can now use `bro.settings.get/set/getAll/reset` etc.

#### Phase 5: Graphics settings panel

Create `system/settings/graphics/index.html`:
- Resolution dropdown (populated from `bro.settings.getDisplayModes()`)
- Fullscreen toggle (checkbox)
- VSync toggle (checkbox)
- FPS cap slider/dropdown
- "Apply" button that calls `bro.settings.set()` for changed values
- "Reset to defaults" button that calls `bro.settings.reset('graphics')`
- Style: dark theme matching perf panel aesthetic

#### Phase 6: Audio settings panel

Create `system/settings/audio/index.html`:
- Master volume slider (0-100%)
- Music volume slider
- SFX volume slider
- Mute toggle checkbox
- Live feedback (sliders update immediately via `bro.settings.set()`)

#### Phase 7: Input settings panel

Create `system/settings/input/index.html`:
- Scroll speed slider
- Double-click threshold slider
- Action bindings list (if any defined by app)
  - Each action shows current keys
  - "Rebind" button enters capture mode (next keypress sets new binding)
- Key capture requires keyboard event forwarding to overlay (similar to mouse forwarding)

#### Phase 8: App override resolution

In `loadPanels()` (or a wrapper), for each system panel path:
1. Check if `<appDir>/<relative_path>/index.html` exists
2. If yes, load from app dir instead of system dir
3. Need to pass appDir to SystemOverlay

### 7. Files to create

| File | Purpose |
|------|---------|
| `system/nav/index.html` | Tab bar navigation panel |
| `system/settings/graphics/index.html` | Graphics settings UI |
| `system/settings/audio/index.html` | Audio settings UI |
| `system/settings/input/index.html` | Input/keybinding settings UI |

### 8. Files to modify

| File | Change |
|------|--------|
| `src/engine/system_overlay.h` | Add mouse handlers, panel visibility, Settings*/Window* members |
| `src/engine/system_overlay.cpp` | Mouse forwarding, panel visibility, settings bindings install, app override resolution, `__bro.showPanel()` |
| `src/engine/engine.cpp` | Pass Settings*/Window* to SystemOverlay, pass appDir for overrides |
| `src/engine/input_handling.cpp` | Forward mouse/keyboard to overlay when visible |

### 9. Visual layout concept

When overlay is visible (F8):
```
+--[Perf]--[Graphics]--[Audio]--[Input]------------------+  <- nav bar (top)
|                                                          |
|  +-- Active Panel Content (e.g. Graphics) --------+     |
|  |                                                 |     |
|  |  Resolution:  [2560x1440 v]                     |     |
|  |  Fullscreen:  [ ] Off                           |     |
|  |  VSync:       [x] On                            |     |
|  |  FPS Cap:     [Uncapped v]                      |     |
|  |                                                 |     |
|  |  [Reset to Defaults]                            |     |
|  +-------------------------------------------------+     |
|                                                          |
+----------------------------------------------------------+
```

Perf panel could stay as a small widget (top-right) visible alongside settings, or become a full tab. Up to preference.

### 10. Open questions for implementation

1. Should perf stay as a top-right widget always visible, or become a tab?
2. Should settings panels position themselves (absolute, like perf does), or should the overlay manage layout?
3. Keyboard event forwarding for input rebinding — how should "capture mode" work?
4. Should nav bar be a panel or rendered by SystemOverlay directly?
