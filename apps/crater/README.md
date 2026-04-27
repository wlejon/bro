# Crater

Turn-based 2–6 player artillery on destructible terrain. Authoritative
server + thin rendering client over `bro.net`.

## What this app demonstrates

Crater is the **reference implementation** of the shared
[`apps/lib/`](../lib/README.md) game kernel. Every reusable module is
exercised here:

| Module        | Used for                                                       |
|---------------|----------------------------------------------------------------|
| `GameLoop`    | dt-clamped render/tick loop (`client.js`)                      |
| `Input`       | named, rebindable keyboard actions + held / edge-triggered     |
| `SFX`         | menu beeps, fire/hit/die/win stingers                          |
| `Storage`     | persisted name, server address, (future: settings)             |
| `Hud`         | text updates, toast for turn-skip notifications                |
| `Screens`     | title / lobby / pause / gameover / howto state machine         |
| `NetRoom`     | client-side lobby + turn messaging over `bro.net`              |

On the server side, `server.js` inlines the small bits of `NetRoom` it
needs (framing + connection lifecycle). A real multi-repo setup could
share these via a loader; for now the comment at the top of `server.js`
marks the duplicated block.

## Architecture

```
        ┌─────────────┐      hello         ┌───────────────┐
        │  client.js  │ ─────────────────▶ │   server.js   │
        │             │ ◀──── state ────── │ (authoritative│
        │  renders    │ ─── ready / bot ─▶ │  heightmap +  │
        │  scene +    │ ◀──── match ────── │  ballistic    │
        │  HUD; sends │ ───── fire ──────▶ │  sim)         │
        │  aim intent │ ◀──── shot ─────── │               │
        └─────────────┘      over          └───────────────┘
             ▲
             │ same seeded heightmap, same crater math (shared.js)
             ▼
      all clients stay in lockstep without per-frame sync
```

The server owns:
- player roster + ready-state (lobby)
- `Float32Array(COLS)` heightmap
- tank positions, HP, turn order
- projectile simulation (`simulateShot`)
- bot decision making

Clients keep a local copy of the heightmap purely for rendering. After
each shot, the server broadcasts a `craterCols` diff (only the columns
that changed) and clients apply it verbatim via `applyCraterDiff`.

## Running

From the launcher (preferred — server is auto-spawned):
1. Launcher lists "Crater".
2. Open it — the launcher spins up `server.js` as a child process.
3. Click **Connect** (defaults to `127.0.0.1:27100`).
4. Share your machine's IP with friends so they can join.

Standalone:
```
# terminal 1
./build/Debug/bro-headless.exe apps/crater server.js

# terminal 2 (and 3, and 4…)
./build/Debug/bro.exe apps/crater
```

## Controls

- **← / →**: flip aim direction
- **↑ / ↓**: raise / lower angle
- **Q / E**: power down / up
- **Space**: fire
- **Esc**: pause menu

All keys rebind through the standard `bro.settings` actions UI when
hosted inside `bro.exe`.

## Physics constants

Defined once in `shared.js` and mirrored atop `server.js`. Change both
together — client and server must agree on gravity, muzzle velocity,
crater radius, and blast damage curve for simulations to stay in sync.
