# Settings System

Persistent, layered settings system for graphics, audio, and input configuration. The engine provides sensible defaults; apps can override them; users can override those. User settings persist across sessions.

## JS API: `bro.settings`

### Reading settings

| Function | Description |
|----------|-------------|
| `bro.settings.get(key)` | Get a single setting value (typed: boolean, number, or string) |
| `bro.settings.getAll()` | Get all settings as `{graphics: {...}, audio: {...}, input: {...}}` |
| `bro.settings.getAll(category)` | Get all settings in a category (`"graphics"`, `"audio"`, or `"input"`) |

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

## Action binding system

Define named actions with default key bindings. Users can rebind them. Key presses dispatch `"action"` events on the document.

### Defining and rebinding

| Function | Description |
|----------|-------------|
| `bro.settings.defineAction(name, keys)` | Define an action with default key bindings (app-level, not persisted) |
| `bro.settings.rebindAction(name, keys)` | Rebind an action (user-level, persisted) |
| `bro.settings.getActionKeys(name)` | Get the current key bindings for an action |
| `bro.settings.getKeyAction(key)` | Get the action bound to a key (or `null`) |
| `bro.settings.getActions()` | Get all defined actions as `[{action, keys}, ...]` |

Keys use the [Web KeyboardEvent.key](https://developer.mozilla.org/en-US/docs/Web/API/KeyboardEvent/key/Key_Values) values — the same strings you see in `event.key` from `keydown` listeners. For example: `" "` (space), `"ArrowUp"`, `"a"`, `"Enter"`, `"Shift"`.

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
| `phase` | `"down"` on key press, `"up"` on key release |
| `key` | The web key value that triggered the action |

Action events fire after the standard `keydown`/`keyup` event. Both events propagate independently.

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

<script>
(function() {
    // Card layout constants — match system/nav.html so the panel lines up
    // with the modal's content region.
    var CARD_W = 720, CARD_H = 520, SIDEBAR_W = 180, HEADER_H = 44;
    var panel = document.getElementById('panel');
    function positionPanel() {
        var vp = __bro.getViewport();
        var cl = Math.max(0, Math.floor((vp.width - CARD_W) / 2));
        var ct = Math.max(0, Math.floor((vp.height - CARD_H) / 2));
        panel.style.setProperty('left', (cl + SIDEBAR_W) + 'px');
        panel.style.setProperty('top', (ct + HEADER_H) + 'px');
        panel.style.setProperty('width', (CARD_W - SIDEBAR_W) + 'px');
        panel.style.setProperty('height', (CARD_H - HEADER_H) + 'px');
    }
    positionPanel();
    window.__onResize = positionPanel;

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

The tab label comes from the `<title>` element; the file's stem (`gameplay`) becomes the panel id. Use `bro.settings.set(...)` for any key — custom app keys are persisted to `.bro_settings.json` alongside engine keys and survive restarts.

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
