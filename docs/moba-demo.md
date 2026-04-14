# MOBA demo — `apps/moba`

A three-lane classic MOBA built on `bro.scene` + `bro.ai.game` to exercise
the capability/policy layer end-to-end. Primary goals of the demo:

- Prove the **AgentBinding** slot — scene objects drive themselves from a
  JS `think(self, world)` callback, no per-frame plumbing from game code.
- Prove that the same `think` shape scales from **towers** (just attack)
  through **minions** (lane walk + attack) to **heroes** (full action menu),
  differing only by capability set + thinkHz.
- Prove that **world.tick(dt)** auto-ticking via `scene.attachAIWorld`
  removes the manual accumulator from the game loop.

## Files

| File | Purpose |
|---|---|
| `index.html` | Canvas + HUD overlay |
| `bro.json` | App manifest |
| `map.js` | Bounds, lane waypoints, tower/nexus positions, jungle walls |
| `units.js` | Stat blocks for minion_melee / minion_caster / tower / nexus / champion |
| `abilities.js` | `world.registerAbility` for Q/W/R projectiles and heals |
| `ai/minion.js` | `think` fn for minions (attack-in-range → lane walk) |
| `ai/tower.js` | `think` fn for towers (prioritize minions > champions) |
| `ai/hero_bot.js` | Enemy hero `think` fn (kite on low HP, focus low-HP targets) |
| `waves.js` | `setInterval(30_000)` spawner per lane per team |
| `scene_setup.js` | Ortho camera + ground + tower/nexus meshes, `attachAIWorld` |
| `hero.js` | Player hero input routing: click-to-move, Q-cast |
| `main.js` | Bootstrap; ~30 lines |

## Architecture

```
Canvas (scene context, orthographic camera at ~30° tilt)
  │
  ├─ ground plane (40 × 40, grey)
  ├─ lane meshes (top / mid / bot)
  ├─ towers (cylinder per side per lane)
  ├─ nexus (box per side)
  ├─ minion nodes (capsule per active minion)
  └─ hero nodes (capsule per hero)

bro.ai.game.World  (single, shared)
  └─ Agents (towers + minions + heroes all live here)
  └─ Registered abilities (fireball, heal, ult…)

scene.attachAIWorld(world)     ← drives world.tick(dt) from the engine loop
node.attachAgent(world, agent, { think, thinkHz, capabilities })
                                 ← one binding per unit; think picks the next action
```

## Why each piece earns its place

- **`units.js`** — every unit is a JS object, not a C++ subclass. Brogameagent
  `Unit` fields (hp, damage, attacksPerSec, armor, mana, ability slots) are
  all mutable, so we configure via setters at spawn time.
- **`abilities.js`** — hero spells use `world.registerAbility(id, {cooldown,
  manaCost, range, fn})` where `fn` is a JS callback. `fn` spawns projectiles
  via `world.spawnProjectile` or calls `world.dealDamage` for direct effects.
- **`ai/*.js`** — three `think` functions for three archetypes. A minion's
  think gets `capabilities: ["lane_walk","basic_attack","hold"]`; a tower's
  gets `["basic_attack","hold"]` (no movement); a hero's gets the full set
  plus JS-authored `["kite","focus_lowhp"]` via `registerCapability`.
- **`waves.js`** — `setInterval(30_000)` spawns K minions per lane per team,
  sets `laneWaypoints` on each binding, attaches a capsule mesh. Minion
  death removes the agent from the world and destroys the mesh node.

## Verification

```bash
# Headless smoke — 60 s of sim, screenshot to confirm render.
./build/src/headless/Debug/bro-headless.exe apps/moba \
  -e "advanceTime(60000)" \
  -e "screenshot('moba-60s.png')"

# Windowed — manual verification:
./build/src/Debug/bro.exe apps/moba
```

Manual checks in the windowed demo:
- Isometric view shows all three lanes with visible towers.
- Waves spawn every 30 s, march, and fight at the lane meeting point.
- Towers fire only at in-range enemies; minions die to towers eventually.
- Click ground → hero path-walks; right-click enemy → hero attacks;
  `Q` cast triggers a fireball that damages on impact.
- Killing the enemy nexus shows a "VICTORY" HUD; the game stops.
