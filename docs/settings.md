# Settings System

Persistent, layered settings system for graphics, audio, input, and appearance configuration. The engine provides sensible defaults; apps can override them; users can override those. User settings persist across sessions.

## JS API: `bro.settings`

### Reading settings

| Function | Description |
|----------|-------------|
| `bro.settings.get(key)` | Get a single setting value (typed: boolean, number, or string) |
| `bro.settings.getAll()` | Get all settings as `{graphics: {...}, audio: {...}, input: {...}}` |
| `bro.settings.getAll(category)` | Get all settings in a category (`"graphics"`, `"audio"`, `"input"`, or `"appearance"`) |

```js
bro.settings.get("graphics.vsync")         // true
bro.settings.get("audio.masterVolume")      // 1.0
bro.settings.get("graphics.width")          // 1920

let all = bro.settings.getAll("audio");
// { masterVolume: 1, musicVolume: 1, sfxVolume: 1, muted: false }
```

### Writing settings

| Function | Description |
|----------|-------------|
| `bro.settings.set(key, value)` | Set a user override (persisted, takes effect immediately) |
| `bro.settings.setDefault(key, value)` | Set an app-level default (not persisted, overridden by user settings) |

```js
// User overrides — persisted to .bro_settings.json, applied at runtime
bro.settings.set("graphics.fullscreen", true);
bro.settings.set("audio.masterVolume", 0.7);
bro.settings.set("graphics.vsync", false);

// App defaults — not persisted, lower priority than user overrides
bro.settings.setDefault("graphics.width", 1280);
bro.settings.setDefault("graphics.height", 720);
```

### Resetting

| Function | Description |
|----------|-------------|
| `bro.settings.reset(category)` | Clear user overrides for a category, reverting to app/engine defaults |
| `bro.settings.reset()` | Clear all user overrides |

```js
bro.settings.reset("audio");    // audio reverts to defaults
bro.settings.reset();           // everything reverts to defaults
```

### Display modes

| Function | Description |
|----------|-------------|
| `bro.settings.getDisplayModes()` | Enumerate available fullscreen resolutions and refresh rates |

```js
let modes = bro.settings.getDisplayModes();
// [{ width: 2560, height: 1440, refreshRate: 144 },
//  { width: 1920, height: 1080, refreshRate: 60 }, ...]
```

## Settings keys

### Graphics (`graphics.*`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `graphics.width` | int | 1920 | Window width in pixels |
| `graphics.height` | int | 1080 | Window height in pixels |
| `graphics.fullscreen` | bool | false | Fullscreen mode |
| `graphics.vsync` | bool | true | Vertical sync (adaptive preferred, standard fallback) |
| `graphics.resizable` | bool | true | Whether the window can be resized |
| `graphics.maxFrameIntervalMs` | number | 8.0 | Layout/raster throttle in ms (0 = uncapped) |
| `graphics.maxFps` | number | 0 | Present-rate cap independent of vsync (0 = uncapped). Also settable via the `maxFps` manifest key. The window is always clamped to 30 fps while it lacks input focus, regardless of this value. |

### Audio (`audio.*`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `audio.masterVolume` | float | 1.0 | Master volume (0.0 - 1.0) |
| `audio.musicVolume` | float | 1.0 | Music volume (0.0 - 1.0) |
| `audio.sfxVolume` | float | 1.0 | Sound effects volume (0.0 - 1.0) |
| `audio.muted` | bool | false | Master mute toggle |

### Input (`input.*`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `input.scrollSpeed` | float | 48.0 | Pixels per mouse wheel tick |
| `input.doubleClickThresholdMs` | number | 500.0 | Max time between clicks for double-click (ms) |
| `input.doubleClickDistancePx` | float | 5.0 | Max movement between clicks for double-click (px) |
| `input.overlayToggleKey` | int | 1073741889 | SDL keycode for system overlay toggle (F8) |

### Appearance (`appearance.*`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `appearance.colorScheme` | string | `"system"` | Color scheme fed to CSS `@media (prefers-color-scheme: ...)`: `"system"` follows the OS theme (live — an OS theme flip restyles running apps), `"light"`/`"dark"` force a scheme. Applies to the app document, iframes, and system panels. Changing it at runtime re-evaluates every `@media` block and restyles immediately — which also makes it the headless-test hook for deterministic dark/light rendering. |

```js
// App prefers dark unless the user says otherwise
bro.settings.setDefault("appearance.colorScheme", "dark");

// User forces light regardless of the OS theme (persisted)
bro.settings.set("appearance.colorScheme", "light");
```

## Action binding system

Define named actions with default key bindings. Users can rebind them. Key presses dispatch `"action"` events on the document.

### Defining and rebinding

| Function | Description |
|----------|-------------|
| `bro.settings.defineAction(name, keys [, options])` | Define an action with default bindings (app-level, not persisted). `options.deadzone` sets the axis-binding deadzone for this action (default 0.1) |
| `bro.settings.rebindAction(name, keys)` | Rebind an action (user-level, persisted) |
| `bro.settings.getActionKeys(name)` | Get the current key bindings for an action |
| `bro.settings.getKeyAction(key)` | Get the action bound to a key (or `null`) |
| `bro.settings.getActions()` | Get all defined actions as `[{action, keys}, ...]` |

A binding string is one of (all forms mix freely in one action, and are
accepted everywhere binding strings are — `defineAction` defaults,
`rebindAction`, and persisted rebinds):

| Form | Examples | Meaning |
|------|----------|---------|
| Web `KeyboardEvent.key` value | `" "`, `"ArrowUp"`, `"a"`, `"Enter"` | Keyboard key ([key values](https://developer.mozilla.org/en-US/docs/Web/API/KeyboardEvent/key/Key_Values) — the strings you see in `event.key`) |
| `"mouse:<button>"` | `"mouse:left"`, `"mouse:right"`, `"mouse:x2"` | Mouse button: `left`, `middle`, `right`, `x1` (back), `x2` (forward) |
| `"gamepad:<button>"` | `"gamepad:south"`, `"gamepad:lefttrigger"` | Gamepad button (standard-layout names; triggers press past 0.1) |
| `"gamepad:<axis><+/->"` | `"gamepad:leftx+"`, `"gamepad:righty-"` | Stick axis direction: `leftx`, `lefty`, `rightx`, `righty`, each with `+` or `-` |

```js
// App defines actions with default bindings
bro.settings.defineAction("jump", [" ", "ArrowUp"]);
bro.settings.defineAction("attack", ["j", "x"]);
bro.settings.defineAction("move_left", ["a", "ArrowLeft"]);

// User rebinds (persisted across sessions)
bro.settings.rebindAction("jump", [" ", "w"]);

// Query bindings
bro.settings.getActionKeys("jump");    // [" ", "w"]
bro.settings.getKeyAction(" ");        // "jump"
bro.settings.getKeyAction("z");        // null

bro.settings.getActions();
// [{action: "jump", keys: [" ", "w"]},
//  {action: "attack", keys: ["j", "x"]},
//  {action: "move_left", keys: ["a", "ArrowLeft"]}]
```

### Gamepad bindings

Gamepad buttons participate in the same binding system via `"gamepad:<name>"`
strings, using the standard-layout button names (matching SDL's mapping-string
fields): `south`, `east`, `west`, `north`, `leftshoulder`, `rightshoulder`,
`lefttrigger`, `righttrigger`, `back`, `start`, `leftstick`, `rightstick`,
`dpup`, `dpdown`, `dpleft`, `dpright`, `guide`. Keyboard and gamepad bindings
mix freely in one action:

```js
bro.settings.defineAction("jump", [" ", "gamepad:south"]);
bro.settings.rebindAction("jump", ["w", "gamepad:north"]);   // persisted

bro.settings.getKeyAction("gamepad:south");  // null (after the rebind)
bro.settings.getKeyAction("gamepad:north");  // "jump"
```

A bound button's press/release edges dispatch the same `"action"` events as
keys (the analog triggers count as pressed past 0.1). `detail.key` carries the
`"gamepad:<name>"` string and `detail.gamepad` the controller's slot index.
See [gamepad-api.js](gamepad-api.js) for the full Gamepad API.

### Mouse-button bindings

`"mouse:left"`, `"mouse:middle"`, `"mouse:right"`, `"mouse:x1"` (back), and
`"mouse:x2"` (forward) bind mouse buttons. The action `"down"` fires when the
press reaches the app layer (presses consumed by engine overlays or system
panels never start an action — the same rule as keyboard actions, which never
fire for consumed keydowns); once a `"down"` fired, the matching `"up"` is
always dispatched on release regardless of what consumes it, so down/up pairs
stay balanced.

```js
bro.settings.defineAction("fire", ["mouse:left", "gamepad:righttrigger"]);
bro.settings.defineAction("aim", ["mouse:right"]);
```

### Gamepad-axis bindings

`"gamepad:<axis><+/->"` binds one direction of a stick axis (`leftx`, `lefty`,
`rightx`, `righty`) as a pressable input: it counts as pressed once the
deflection along that direction crosses the action's **deadzone** (default
0.1, matching the trigger convention; per-action override via
`defineAction(name, keys, { deadzone })`).

Release uses **hysteresis**: a pressed axis binding releases only when the
deflection falls below `deadzone * 0.75` (a 25% release margin), so jitter
right at the threshold can't spam down/up pairs.

```js
// WASD + left stick, with a wider stick deadzone for this action
bro.settings.defineAction("move_right", ["d", "gamepad:leftx+"], { deadzone: 0.25 });
bro.settings.defineAction("move_left",  ["a", "gamepad:leftx-"], { deadzone: 0.25 });
```

### Action events

When a key bound to an action is pressed or released, an `"action"` event is dispatched on `document.body` with a `detail` object:

```js
document.addEventListener("action", (e) => {
    console.log(e.detail.action);  // "jump"
    console.log(e.detail.phase);   // "down" or "up"
    console.log(e.detail.key);     // " " (the actual key pressed)

    if (e.detail.action === "jump" && e.detail.phase === "down") {
        player.jump();
    }
});
```

| `detail` property | Description |
|--------------------|-------------|
| `action` | The action name (e.g. `"jump"`) |
| `phase` | `"down"` on press, `"up"` on release |
| `key` | The binding string that triggered the action (`" "`, `"mouse:left"`, `"gamepad:leftx+"`, ...) |
| `strength` | The binding's contribution at the edge: 1/0 for keys and mouse buttons, the analog value for triggers, the deadzone-rescaled deflection for axis bindings (0 on `"up"`) |
| `gamepad` | Controller slot index — present only for gamepad-originated events |

Keyboard action events fire after the standard `keydown`/`keyup` event. Both events propagate independently.

### Polled action state

For per-frame game loops, poll instead of (or alongside) listening:

| Function | Description |
|----------|-------------|
| `bro.settings.getActionStrength(name)` | Current analog strength 0..1: the max over all of the action's bindings. Keyboard keys and mouse buttons contribute 0/1, gamepad buttons their analog value (triggers are analog), axis bindings their deadzone-rescaled deflection `(m - deadzone) / (1 - deadzone)` |
| `bro.settings.isActionPressed(name)` | Polled pressed state. Axis bindings use the same hysteresis latch that drives their `"action"` events, so polling always agrees with the down/up stream |

```js
bro.settings.defineAction("move_right", ["d", "gamepad:leftx+"]);

function tick() {
    const v = bro.settings.getActionStrength("move_right");  // analog on stick,
    player.x += v * speed * dt;                              // 0/1 on the key
    if (bro.settings.isActionPressed("jump")) { /* ... */ }
}
```

Unlike `"action"` events (edges only), `getActionStrength` tracks analog
changes continuously — a trigger held at 0.9 reads 0.9 even though no new
event fired since it crossed the press threshold.

## Priority system

Settings are resolved with three layers (later wins):

1. **Engine defaults** — hardcoded values (see tables above)
2. **App overrides** — set via `bro.json` or `bro.settings.setDefault()` at runtime
3. **User overrides** — set via `bro.settings.set()`, persisted to `.bro_settings.json`

When a user override exists, it takes priority. When it doesn't, the app override is used. When neither exists, the engine default is used. `bro.settings.reset()` clears user overrides, reverting to app/engine defaults.

## Runtime behavior

Settings changed via `bro.settings.set()` take effect immediately:

| Setting | Runtime effect |
|---------|---------------|
| `graphics.fullscreen` | Toggles fullscreen via SDL |
| `graphics.vsync` | Changes swap interval |
| `graphics.width/height` | Resizes window (windowed mode only) |
| `graphics.resizable` | Toggles window resizability |
| `graphics.maxFrameIntervalMs` | Changes render throttle |
| `audio.masterVolume` | Adjusts master gain |
| `audio.muted` | Mutes/unmutes audio |
| `input.*` | Updates input behavior immediately |
| Action bindings | Lookup tables rebuilt immediately |

## Persistence

User settings are stored in `.bro_settings.json` next to the executable. The file uses a flat key-value JSON format:

```json
{
  "graphics.vsync": "false",
  "audio.masterVolume": "0.7",
  "input.bindings.jump": " ,w"
}
```

This file is engine-global (shared across all apps). It is written automatically on every `bro.settings.set()` or `bro.settings.rebindAction()` call. It is read at engine startup.

## Preferences modal

The engine ships a standard Preferences dialog reachable via **Edit → Preferences** or however the app rebinds `system_toggle_settings`. It's a modal: backdrop click or **ESC** dismisses it.

Default panels are `Graphics`, `Audio`, and `Input`, covering the keys in the tables above. Apps extend the dialog by dropping HTML files into `<app-dir>/system/settings/`. The engine scans that directory at startup and the nav auto-populates tabs from whatever it finds — no registration step.

See [system-panels.md](system-panels.md) for the full system-panel authoring surface (the `__bro` bridge, panel lifecycle hooks, `<script src>` support). The rest of this section covers just the settings-specific piece: getting a new tab's content region positioned correctly.

### Adding an app panel

Create `<app-dir>/system/settings/gameplay.html`:

```html
<!DOCTYPE html>
<html>
<head>
<title>Gameplay</title>
<style>
html, body { margin: 0; padding: 0; background: transparent;
             font-family: Consolas, monospace; font-size: 12px; color: #e0e0e0; }
#panel { position: absolute; background: #1a1a1a; padding: 24px 28px; box-sizing: border-box; }
h1 { margin: 0 0 20px 0; font-size: 16px; color: #e0e0e0; }
.row { margin-bottom: 14px; }
.label { display: inline-block; width: 140px; color: #999; }
</style>
</head>
<body>
<div id="panel">
    <h1>Gameplay</h1>
    <div class="row"><span class="label">Difficulty</span>
        <select id="difficulty">
            <option value="easy">Easy</option>
            <option value="normal">Normal</option>
            <option value="hard">Hard</option>
        </select>
    </div>
</div>

<script src="../lib/panel-runtime.js"></script>
<script>
(function() {
    var panel = document.getElementById('panel');
    PanelLayout.onResize(function() {
        PanelLayout.positionContent(panel);
    });

    var sel = document.getElementById('difficulty');
    sel.value = bro.settings.get('game.difficulty') || 'normal';
    sel.addEventListener('change', function() {
        bro.settings.set('game.difficulty', sel.value);
    });
})();
</script>
</body>
</html>
```

The tab label comes from the `<title>` element; the file's stem (`gameplay`) becomes the panel id. Use `bro.settings.set(...)` for any key — custom app keys are persisted to `.bro_settings.json` alongside engine keys and survive restarts. `PanelLayout` (`system/lib/panel-runtime.js`) is what keeps this tab's content region aligned with the modal shell — see [system-panels.md](system-panels.md#panellayout--shared-modal-geometry).

## bro.json integration

Settings from `bro.json` flow into the app override layer. These keys are supported:

```json
{
    "app": ".",
    "title": "My App",
    "width": 1200,
    "height": 800,
    "vsync": false,
    "resizable": true,
    "maxFps": 120,
    "scrollSpeed": 60,
    "doubleClickThreshold": 400,
    "doubleClickDistance": 8
}
```

User overrides from `.bro_settings.json` take priority over `bro.json` values.

Additionally, `bro.json` carries **startup-only window-management keys** — these
configure the window at creation and are *not* settings (no user-override layer,
no persistence; runtime control is `bro.window.*` — see
[window-api.js](window-api.js)):

```json
{
    "borderless": true,
    "alwaysOnTop": true,
    "minWidth": 320,
    "minHeight": 240,
    "maxWidth": 1600,
    "maxHeight": 900,
    "windowX": 100,
    "windowY": 100,
    "display": 1
}
```

`windowX`/`windowY` (both required together; negative values are legal on
multi-monitor desktops) win over `display` (a display *index* to center the
window on). Positioning keys are skipped in headless mode.
