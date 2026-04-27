# apps/lib — shared game kernel

Reusable modules for bro arcade apps. Each file defines a single global
namespace (IIFE style — same as `camera.js`) and has no cross-file
dependencies unless noted. Include only the ones you need:

There is no manifest or bundler — every app's `index.html` lists the
modules it wants directly. Order matters when one file uses another's
global (e.g. `screens.js` after the SFX wrapper, `storage.js` before
any per-app `storage.js` that wraps it).

```html
<script src="../lib/loop.js"></script>
<script src="../lib/canvas.js"></script>
<script src="../lib/math.js"></script>
<script src="../lib/input.js"></script>
<script src="../lib/audio.js"></script>
<script src="../lib/storage.js"></script>
<script src="../lib/fx.js"></script>
<script src="../lib/particles.js"></script>
<script src="../lib/hud.js"></script>
<script src="../lib/screens.js"></script>
<script src="../lib/netroom.js"></script>
```

| Module         | Global      | Purpose                                                          |
|----------------|-------------|------------------------------------------------------------------|
| `loop.js`      | `GameLoop`  | `rAF` wrapper, clamped dt, start/stop/pause                      |
| `canvas.js`    | `Canvas`    | `Canvas.w/h/size(ctx, fallback)` — engine-aware size with fallback |
| `math.js`      | `MathX`     | `clamp`, `lerp`, `randRange/Int/Pick`, `vecFromAngle`, `angleNorm` |
| `input.js`     | `Input`     | keyboard + `bro.settings` action bindings, pressed/down          |
| `audio.js`     | `SFX`       | one-shot tones + bus setup; optional, silent if no AudioContext  |
| `storage.js`   | `Storage`   | namespaced JSON persistence + high-score tables (asc or desc)    |
| `fx.js`        | `FX`        | screen shake (`shake`/`shakeOffset`) + DOM toast (`toast`)       |
| `particles.js` | `Particles` | 2D particle pool — `createSystem`/`burst`/`step`/`draw` (sec-based) |
| `hud.js`       | `Hud`       | DOM text/show/hide                                               |
| `screens.js`   | `Screens`   | overlay state machine, menu nav, optional shared bg + HUD toggle |
| `netroom.js`   | `NetRoom`   | lobby + turn helpers over `bro.net`                              |
| `camera.js`    | `Camera`    | 3D orbit/fly camera                                              |
| `camera2d.js`  | `Camera2D`  | 2D follow camera with deadzone + level-bounds clamping           |
| `tilemap.js`   | `Tilemap`   | fixed-size tile grid, atlas blit, AABB queries (`solidAtPx`)     |
| `platformer.js`| `Platformer`| AABB body + tile collision + jump feel (coyote/buffer/cut)       |

Conventions:
- All modules are safe to load without calling `init()` — lazy by default.
- Audio and network modules degrade silently when unavailable (no `AudioContext`, no `bro.net`).
- DOM selectors use IDs the caller provides; no hard-coded element names.
- `Storage.create("myapp")` → scoped `localStorage` prefix, so apps can't collide.

See `apps/crater/` for a complete reference implementation that exercises
every module.
