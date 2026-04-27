# Stompworld

Side-scrolling platformer scaffold. Run, jump, stomp, reach the flag.

This app is the first one in `apps/` to express bro's 2D scrolling-world
capability — tilemap rendering, follow camera, AABB-vs-tile collision,
sprite-frame animation. It also seeds three new modules in `apps/lib/`
(`tilemap.js`, `camera2d.js`, `platformer.js`) so future side-scrollers
inherit the plumbing.

## Files

| File         | Role                                                         |
|--------------|--------------------------------------------------------------|
| `bro.json`   | App manifest (800×576 viewport)                              |
| `index.html` | Canvas, HUD, screen overlays, script load order              |
| `style.css`  | Pixel-art friendly styles (`image-rendering: pixelated`)     |
| `art.js`     | Code-driven pixel art — tile/sprite recipes painted via fillRect |
| `level.js`   | World 1-1 ASCII layout + entity spawn extraction             |
| `app.js`     | Main loop, screens state machine, gameplay rules             |

## Controls

- **A / D** or **&larr; / &rarr;** — run
- **Space** (or **W** / **&uarr;**) — jump (hold for higher arc)
- **Esc** / **P** — pause

## How the art is built

Every tile, hero frame, stomper frame, and the flag is described as an
ASCII grid + small palette in `art.js`. At first use, those recipes are
stamped via `fillRect` into offscreen `<canvas>` atlases — one for the
tileset, one for the hero sheet, one for the stomper sheet, one for the
flag. The game then blits from those atlases each frame with
`ctx.drawImage(atlas, sx, sy, sw, sh, dx, dy, dw, dh)`. Same pattern as
artstation; the recipes could be lifted into `apps/artstation/assets/`
later to also produce PNG/WebM/GIF outputs without changing the game.

## Adding a level

`level.js` is a single ASCII array. Edit `ROWS`, save, reload. Tile
chars and entity chars are documented at the top of the file. Solid
tiles are listed in `SOLID_IDS` (passed to `Tilemap.create`).

## Known limitations / future work

- Single hand-authored level; no level transitions yet.
- No coins, no power-ups, no multiple lives indicator beyond a counter.
- Stomper AI is the simplest possible (walk, reverse on wall, don't fall
  off ledges).
- No coyote-time tuning sweep — values in `app.js` are NES-ish defaults.
- Background is a flat sky gradient; no parallax layers yet.
- No music; just SFX stings.

## Smoke test (headless)

```bash
bro-headless apps/stompworld -e "
  advanceTime(50);
  document.querySelector('.menu-item.selected').click();
  advanceTime(50);
  keyDown(100); advanceTime(800);   // 'd' key
  keyDown(32);  advanceTime(150); keyUp(32);  // space
  keyUp(100);
  flush();
  screenshot('out.png');
  console.log('player x:', __SW.Game.player.x);
"
```

`window.__SW = { Game, S, Art }` is exposed for headless inspection.
