# Plan: `apps/ai-arena` — Full brogameagent Demo

## Context

`src/js/ai_bindings.cpp` now exposes the full brogameagent C++ API to JS under `bro.ai.game.*` (NavGrid, Agent, Unit, World, Projectile, perception, steering, observation, action mask, reward, simulation, recorder, replay reader, MCTS). The `apps/fps` demo exercises pathfinding + perception + steering but nothing else. We need a single app that visibly demonstrates every subsystem so the integration is self-documenting and discoverable.

Target: top-down 2D canvas arena, 2v2 team combat, ~20×20 units with static obstacles. Every brogameagent feature should map to something visible on screen or a UI toggle. Keep it server-free (single-window app) so the demo runs from one file.

## App layout

```
apps/ai-arena/
├── index.html     # DOM scaffolding: canvas, HUD panels, controls
├── style.css      # panel styling (dark theme, monospace)
├── main.js        # entry: engine setup, main loop, input handling
├── arena.js       # world construction, obstacle layout, spawn points
├── render.js      # canvas 2D drawing: agents, projectiles, paths, FOV, HP bars
├── ai.js          # bot policies: scripted + MCTS + reward-aware
└── ui.js          # HUD panels: roster, obs vector bars, damage log, MCTS stats
```

Canvas 2D (not WebGL / scene3d) keeps drawing code small. Use `requestAnimationFrame` via existing timer APIs.

## Feature coverage matrix

| brogameagent feature | Visible in demo |
|---|---|
| `createNavGrid` + obstacles | Grey arena + black obstacle boxes |
| `Agent.setTarget` + `update` | Circle agents move along nav paths |
| `agent.path` | Faint polyline behind each bot showing its A* path |
| `canSee` (FOV + LOS) | Translucent cone wedge in front of each bot |
| `hasLineOfSight` | Targeting line turns green when LOS, red when blocked |
| `computeAim` / `computeLeadAim` | Projectiles lead moving targets; option toggles between aim-at-now vs aim-ahead |
| `steer.flee` / `steer.evade` | Low-HP bots kite; drawn motion vector arrow |
| `World.spawnProjectile` (Single/Pierce/AoE) | 3 ability types: basic bolt (Single), piercing beam (Pierce), grenade (AoE — explosion circle) |
| `World.resolveAttack` | Melee tick-attack when in range |
| `World.registerAbility` / `resolveAbility` | Heal ability (HoT), Fireball (damage + DoT), Dash (movement boost) |
| `Unit` stats (hp/mana/armor/buffs) | HP + mana bars above each agent; buff icons |
| `World.events` (damage log) | Floating damage numbers + scrolling event log panel |
| `buildObservation` | Right panel: bar graph of 64 observation floats for focused agent |
| `buildActionMask` | Below obs panel: 13 mask cells colored green/red |
| `RewardTracker` | Score graph: cumulative reward per agent over time |
| `Simulation` + `addPolicy` | Sim drives AI-controlled agents; toggle Sim on/off |
| `Mcts.search` | One team can be switched to MCTS; "Thinking... N iters" overlay |
| `World.snapshot` / `restore` | Pause/rewind button — jumps back 2 seconds |
| `Recorder` + `ReplayReader` | Record button saves `.bgar`; Play button loads it and scrubs frames |

## UI wireframe

```
┌─────────────────────────────────┬──────────────────────┐
│                                 │ ROSTER               │
│                                 │ Red Team  Blue Team  │
│        ARENA CANVAS             │ ■ Alpha   □ Echo     │
│        (700×700 px)             │ ■ Bravo   □ Foxtrot  │
│                                 │                      │
│   • agents as colored circles   │ MCTS (blue)          │
│   • HP/mana bars                │  iters: 842          │
│   • FOV cones                   │  bestMean: +0.31     │
│   • path polylines              │                      │
│   • projectiles                 │ OBSERVATION (focused)│
│   • AoE explosion rings         │ ▌▌▍▍▌▍▃▁▂▁...        │
│   • damage numbers              │                      │
│                                 │ ACTION MASK          │
│                                 │ ■■□□□ ■■■□□□□        │
│                                 │                      │
├─────────────────────────────────┤ REWARD SCORE         │
│ DAMAGE LOG                      │ Red: +12  Blue: +18  │
│ 03:42 Alpha → Echo    -10 ✓     │                      │
│ 03:44 Echo → Alpha    -8        │ ┌──────chart──────┐  │
│ 03:47 Bravo fireball  -25 ✓✝    │ │    /─\          │  │
├─────────────────────────────────┤ └──────────────────┘ │
│ [Pause] [Rewind 2s] [Record]    │                      │
│ [Play file] Sim:[on] AI:[mcts]  │                      │
└─────────────────────────────────┴──────────────────────┘
```

## Build order

1. **Scaffolding** — manifest, empty canvas, dark theme, 60fps loop. Draw a blank arena with obstacle rectangles.

2. **World + agents** — create 4 agents (2 per team), HP/damage/attackRange, NavGrid with padded obstacles. Scripted policy: seek-nearest-enemy. Verify movement + melee attacks + HP depletion + events log.

3. **Rendering layer** — agent circles with HP bars, path polylines, velocity arrows. Damage numbers that float up and fade over 1 second.

4. **Perception visuals** — FOV cones via `canSee`; draw lines between agents when LOS is clear (debug overlay toggleable).

5. **Projectiles + abilities** — register 3 abilities on World (Fireball=Single, Beam=Pierce, Grenade=AoE). Render projectiles each frame from `world.projectiles`. AoE detonation draws an expanding ring.

6. **Simulation integration** — wrap the per-frame tick through `Simulation.addPolicy` instead of manually calling per-agent update. Policies receive (agent, world) and return `AgentAction`.

7. **MCTS team** — toggle makes Blue team use `Mcts.search(world, hero)` each decision window; map `CombatAction.moveDir` to `AgentAction.moveX/moveZ`. Display `lastStats`.

8. **Observation + reward panels** — `buildObservation` bar graph, `buildActionMask` grid, `RewardTracker` score chart.

9. **Snapshot / replay** — Rewind button stashes `world.snapshot()` every second (ring buffer of 5), restores on click. Recorder writes `.bgar` to `apps/ai-arena/replays/`; Play loads latest and replays frames via a read-only render loop.

10. **Polish** — player-controlled hero via WASD + mouse, keybound abilities (Q/W/E), Auto button flips control to scripted policy. Tune HP/damage/speeds.

## Implementation notes

- **Reuse FPS patterns** (`apps/fps/server.js`): cover-point generation, `hasLineOfSight` checks, obstacle array layout are all transferable. But drop the networking layer — arena is single-process.
- **Decision cadence**: run Simulation at 60 Hz but MCTS search at 4 Hz (every 15 frames) since `Mcts.search` is iteration-bounded. Cache the last MCTS action between searches.
- **AgentAction ↔ MCTS CombatAction**: MCTS returns `{moveDir: 0..8, attackSlot, abilitySlot}`. Map MoveDir to 2D unit vector (N=-Z, E=+X, etc.), multiply by agent speed, write into `agent.applyAction({moveX, moveZ, ...})`.
- **World cross-references**: bindings currently use `__agents` JS array on World to resolve C++ → JS; queries like `nearestEnemy` work. Don't bypass with raw pointers.
- **Player control**: when human drives an agent, skip registering a Simulation policy for it — instead call `agent.applyAction()` manually each frame with WASD input mapped to moveX/moveZ in local frame.
- **Observation panel refresh**: don't rebuild every frame — 10 Hz is enough; creating Float32Array(64) per frame is wasteful.
- **Replay rendering**: the ReplayReader Frame gives agent {id, x, z, hp, yaw, alive} — just redraw agents from that, ignore the live World. A "Replay mode" flag suppresses the main loop.

## Verification checklist

- [ ] `cmake --build build --config Debug` clean
- [ ] `bro.exe apps/ai-arena` opens window, 4 agents spawn, fight, someone dies
- [ ] FOV cones visible and update with facing
- [ ] Projectiles travel, hit, and deal damage; AoE splash works
- [ ] Damage log scrolls with kill events
- [ ] Toggle MCTS → Blue bots noticeably change behavior; stats panel updates
- [ ] Rewind 2s → agents jump back to prior positions + HP
- [ ] Record → `.bgar` file appears in `replays/`; Play → arena replays from file
- [ ] Observation panel shows 64 bars, ~half non-zero when enemies nearby
- [ ] Action mask cells light up/down as cooldowns cycle

## Out of scope (future)

- 3D rendering (scene3d). Canvas 2D is enough to prove the APIs; 3D is a polish pass.
- Network multiplayer. The FPS demo already covers that.
- Human-in-the-loop ML training UI. The observation/reward panels are diagnostic only — no actual NN.
- VecSimulation (batched envs). Not useful for a single interactive demo; belongs to a Python training script.
- TeamMcts / TacticMcts / LayeredPlanner. Single-player Mcts is enough to demonstrate tree search; add the coop variants later if a 3v3 PvE scenario lands.
