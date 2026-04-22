# apps/lib — shared game kernel

Reusable modules for bro arcade apps. Each file defines a single global
namespace (IIFE style — same as `camera.js`) and has no cross-file
dependencies unless noted. Include only the ones you need:

```html
<script src="../lib/loop.js"></script>
<script src="../lib/input.js"></script>
<script src="../lib/audio.js"></script>
<script src="../lib/storage.js"></script>
<script src="../lib/hud.js"></script>
<script src="../lib/screens.js"></script>
<script src="../lib/netroom.js"></script>
```

| Module      | Global     | Purpose                                                      |
|-------------|------------|--------------------------------------------------------------|
| `loop.js`   | `GameLoop` | `rAF` wrapper, clamped dt, start/stop/pause                  |
| `input.js`  | `Input`    | keyboard + `bro.settings` action bindings, pressed/down      |
| `audio.js`  | `SFX`      | one-shot tones + bus setup; optional, silent if no AudioContext |
| `storage.js`| `Storage`  | namespaced JSON persistence + high-score tables              |
| `hud.js`    | `Hud`      | DOM text/show/hide, toast, action-text overlay               |
| `screens.js`| `Screens`  | overlay state machine, menu navigation (kbd + mouse)         |
| `netroom.js`| `NetRoom`  | lobby + turn helpers over `bro.net`                          |
| `camera.js` | `Camera`   | 3D orbit/fly camera (already existed)                        |

Conventions:
- All modules are safe to load without calling `init()` — lazy by default.
- Audio and network modules degrade silently when unavailable (no `AudioContext`, no `bro.net`).
- DOM selectors use IDs the caller provides; no hard-coded element names.
- `Storage.create("myapp")` → scoped `localStorage` prefix, so apps can't collide.

See `apps/crater/` for a complete reference implementation that exercises
every module.
