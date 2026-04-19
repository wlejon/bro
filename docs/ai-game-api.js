// =============================================================================
// bro.ai.game — Game AI API Reference
// =============================================================================
//
// The game AI API provides server-side pathfinding, steering, and perception
// for building game bots. Backed by the brogameagent C++ library.
//
// Available in all modes (windowed, headless, server).
//
// Quick start:
//   const nav = bro.ai.game.createNavGrid({
//     minX: -20, minZ: -20, maxX: 20, maxZ: 20,
//     cellSize: 0.5,
//     obstacles: [{ x: 0, z: 0, hw: 2, hd: 2 }],
//     padding: 0.4,
//   });
//
//   const bot = bro.ai.game.createAgent({ navGrid: nav, speed: 6, radius: 0.4 });
//   bot.setTarget(10, 5);
//   bot.update(dt);
//   console.log(bot.x, bot.z);
//
// =============================================================================


// -----------------------------------------------------------------------------
// NavGrid — 2D grid-based navigation mesh
// -----------------------------------------------------------------------------

/**
 * Create a navigation grid for pathfinding.
 *
 * @param {Object} opts
 * @param {number} opts.minX - Left bound of navigable area
 * @param {number} opts.minZ - Top bound of navigable area
 * @param {number} opts.maxX - Right bound of navigable area
 * @param {number} opts.maxZ - Bottom bound of navigable area
 * @param {number} [opts.cellSize=0.5] - Grid cell size (smaller = more precise)
 * @param {Array<{x, z, hw, hd}>} [opts.obstacles] - AABB obstacles to mark as blocked
 * @param {number} [opts.padding=0] - Extra clearance around obstacles (agent radius)
 * @returns {NavGrid}
 */
const nav = bro.ai.game.createNavGrid({
    minX: -20, minZ: -20, maxX: 20, maxZ: 20,
    cellSize: 0.5,
    obstacles: [
        { x: 0, z: 0, hw: 2, hd: 2 },   // 4x4 box at origin
        { x: 10, z: 5, hw: 1, hd: 3 },   // 2x6 box
    ],
    padding: 0.4,  // agent radius clearance
});

/**
 * Check if a position is on a walkable cell.
 * @param {number} x
 * @param {number} z
 * @returns {boolean}
 */
nav.isWalkable(5, 5);  // true (open space)
nav.isWalkable(0, 0);  // false (inside obstacle)

/**
 * Find a path from start to goal using A* with path smoothing.
 * Returns an empty array if no path exists.
 *
 * @param {number} fromX
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toZ
 * @returns {Array<{x: number, z: number}>} Smoothed waypoints
 */
const path = nav.findPath(-10, 0, 10, 0);
// path = [{ x: -10, z: 0 }, { x: -2, z: 3 }, { x: 10, z: 0 }]

/**
 * Add an obstacle after creation (e.g., for dynamic obstacles).
 *
 * @param {{x, z, hw, hd}} obstacle - AABB obstacle
 * @param {number} [padding=0] - Extra clearance
 */
nav.addObstacle({ x: 5, z: 5, hw: 1, hd: 1 }, 0.4);


// -----------------------------------------------------------------------------
// Agent — Pathfinding + steering combined
// -----------------------------------------------------------------------------

/**
 * Create a game agent that navigates using a NavGrid.
 *
 * @param {Object} [opts]
 * @param {NavGrid} [opts.navGrid] - Navigation grid for pathfinding
 * @param {number} [opts.x=0] - Initial X position
 * @param {number} [opts.z=0] - Initial Z position
 * @param {number} [opts.speed=6] - Movement speed (units/second)
 * @param {number} [opts.radius=0.4] - Collision radius
 * @returns {Agent}
 */
const bot = bro.ai.game.createAgent({
    navGrid: nav,
    x: -16, z: -16,
    speed: 6,
    radius: 0.4,
});

/**
 * Set the movement target. Automatically computes a path via the NavGrid.
 * Recomputes if the target moves significantly (>2 units).
 * @param {number} x
 * @param {number} z
 */
bot.setTarget(10, 5);

/** Clear the target. Agent stops moving. */
bot.clearTarget();

/**
 * Advance the agent by dt seconds. Moves along the current path
 * using steering behaviors (seek for intermediate waypoints, arrive for final).
 * @param {number} dt - Delta time in seconds
 */
bot.update(1 / 60);

/** Teleport the agent to a new position. */
bot.setPosition(0, 0);

/**
 * Compute aim yaw/pitch from the agent's position to a 3D world point.
 * Uses -Z forward convention (yaw=0 faces -Z).
 *
 * @param {number} targetX
 * @param {number} targetY
 * @param {number} targetZ
 * @param {number} eyeHeight - Agent's eye height above ground
 * @returns {{ yaw: number, pitch: number }}
 */
const aim = bot.aimAt(enemyX, 1.6, enemyZ, 1.6);

/** Current X position (read-only). */
bot.x;

/** Current Z position (read-only). */
bot.z;

/** Current facing direction in radians (read-only). 0 = -Z, positive = clockwise. */
bot.yaw;

/** Whether the agent has an active target (read-only). */
bot.hasTarget;

/** Whether the agent has reached its target (read-only). */
bot.atTarget;


// -----------------------------------------------------------------------------
// Perception — Line of sight, aim computation
// -----------------------------------------------------------------------------

/**
 * 2D line-of-sight check through AABB obstacles.
 *
 * @param {number} fromX
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toZ
 * @param {Array<{x, z, hw, hd}>} obstacles - AABBs to test against
 * @returns {boolean} true if line is clear
 */
const canSee = bro.ai.game.hasLineOfSight(
    botX, botZ, enemyX, enemyZ,
    obstacles
);

/**
 * Compute aim angles from one 3D point to another.
 * Uses -Z forward convention.
 *
 * @param {number} fromX
 * @param {number} fromY
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toY
 * @param {number} toZ
 * @returns {{ yaw: number, pitch: number }}
 */
const aim2 = bro.ai.game.computeAim(0, 1.6, 0, 10, 1.6, -5);


// -----------------------------------------------------------------------------
// Capability / policy / AgentBinding — scene-driven AI
// -----------------------------------------------------------------------------
//
// A scene node can own an "agent binding" — a capability set (the tools this
// object can use) plus a JS think(self, world) callback (how it decides).
// Minions, towers, and heroes all use the same binding shape; behaviour
// differs only by which capabilities are enabled and which think() fn is
// supplied. Difficulty scales along three orthogonal knobs:
//   1. capability set — add or remove tools
//   2. thinkHz        — how often the decision fires
//   3. think fn       — simple scripted, MCTS wrapper, or NN policy
//
// Built-in capabilities (string ids used in opts.capabilities):
//   "move_to"      — self.moveTo(x, z) sets the pathfinding target
//   "lane_walk"    — self.laneWalk() steps through opts.laneWaypoints
//   "basic_attack" — self.attack(targetId); blocks for 1/attacksPerSec
//   "cast_ability" — self.cast(slot, targetId); blocks for cast time (~0.25s)
//   "flee"         — self.flee([x, z]); retreats away from nearest enemy
//   "hold"         — self.hold([dur]); no-op for dur seconds (always exposed)
//
// A `self` proxy is built fresh each think tick. It only exposes methods
// whose capability is present on the binding — towers won't have .moveTo.


/**
 * Register a JS-authored capability. Returns the assigned capability id.
 * Callbacks run from C++; keep them fast. `start` is invoked when the
 * capability is chosen; `advance` each frame while it's in flight; `gate`
 * when building the available-capability mask (optional, default true).
 *
 * @param {string} name
 * @param {Object} spec
 * @param {function} [spec.gate]     - () => boolean
 * @param {function} [spec.start]    - () => void
 * @param {function} [spec.advance]  - () => boolean (true = done)
 * @param {number}   [spec.id]       - optional explicit id (default: auto-allocated from 100+)
 * @returns {number} capability id
 */
bro.ai.game.registerCapability("kite", {
    gate()    { return true; },
    start()   { /* ... */ },
    advance() { return true; /* done this tick */ },
});


// -----------------------------------------------------------------------------
// SceneGraph.attachAIWorld — auto-tick a World each frame
// -----------------------------------------------------------------------------

/**
 * Drive a brogameagent::World from the engine frame loop at a fixed step.
 * Replaces the JS-side accumulator pattern (see apps/ai-arena's main.js).
 *
 * @param {AIWorld} world
 * @param {Object}  [opts]
 * @param {number}  [opts.stepHz=60]            - fixed-step rate
 * @param {number}  [opts.maxStepsPerFrame=8]   - catch-up clamp for stalls
 */
scene.attachAIWorld(world, { stepHz: 60, maxStepsPerFrame: 8 });
scene.detachAIWorld();


// -----------------------------------------------------------------------------
// SceneNode.attachAgent — bind an AI agent to a scene object
// -----------------------------------------------------------------------------

/**
 * Attach an AI agent + capability set to a scene node. The binding takes
 * care of steering, combat, aim, cast timing, and writing transforms. Call
 * attachAIWorld first (the binding reads the world via the scene graph's
 * attached ticker; the `world` arg here is the JS wrapper passed to think).
 *
 * @param {AIWorld} world
 * @param {AIAgent} agent
 * @param {Object}  [opts]
 * @param {string[]} [opts.capabilities] - ids of enabled caps (default: all built-ins)
 * @param {function(self, world): void} [opts.think] - imperative decision fn
 * @param {number}  [opts.thinkHz=15]
 * @param {number}  [opts.yOffset=0]    - extra Y on the node (ground clearance)
 * @param {boolean} [opts.faceMovement=true]
 * @param {Array<{x,z}>} [opts.laneWaypoints] - waypoints for lane_walk
 * @param {string}  [opts.policy]       - "scripted_minion" as a C++ fallback
 */
minionNode.attachAgent(world, minionAgent, {
    capabilities: ["lane_walk", "basic_attack", "hold"],
    thinkHz: 10,
    laneWaypoints: [{ x: -15, z: 0 }, { x: 0, z: 0 }, { x: 15, z: 0 }],
    think(self, w) {
        const e = w.nearestEnemy(self.agent);
        if (e && self.inRange(e)) return self.attack(e.unit.id);
        return self.laneWalk();
    },
});

/** Remove the binding; the node stops receiving AI updates. */
minionNode.detachAgent();


// -----------------------------------------------------------------------------
// The `self` proxy passed to think()
// -----------------------------------------------------------------------------
//
// Built fresh each think tick. Read-only snapshots of unit state plus
// imperative methods for the enabled capabilities. Exactly one method call
// per think determines the next action (last call wins).
//
// Read-only:
//   self.hp / mana / x / z / id / teamId / attackRange / alive
//   self.agent                          — the underlying AIAgent (for world queries)
//
// Universal helpers (always available):
//   self.distanceTo(target)             — target may be {x,z}, AIAgent, or self
//   self.inRange(target [, range])      — default range is self.attackRange
//   self.hold([dur])                    — no-op fallback
//
// Capability methods (present only when the cap is enabled):
//   self.moveTo(x, z)                   // if move_to
//   self.laneWalk()                     // if lane_walk
//   self.attack(targetId)               // if basic_attack
//   self.cast(slot, targetId)           // if cast_ability
//   self.flee([x, z])                   // if flee
//
// A tower ({capabilities:["basic_attack","hold"]}) has no .moveTo / .laneWalk;
// attempting to call them throws. A think() that falls through without
// picking an action defaults to .hold().


// -----------------------------------------------------------------------------
// MCTS planners
// -----------------------------------------------------------------------------
//
// Seven flavors, all sharing the same MctsConfig:
//
//   createMcts(cfg)           — single agent vs scripted opponents
//   createDecoupledMcts(cfg)  — simultaneous-move 1v1 (both sides searched)
//   createTeamMcts(cfg)       — cooperative N-hero joint planner
//   createTacticMcts(cfg)     — coarse team-tactic planner (Hold / FocusLowestHp / ...)
//   createLayeredPlanner({ tactic, fine })
//                             — TacticMcts over TeamMcts with a tactic-match prior
//   createOptionMcts(cfg)     — search over caller-authored single-hero Options
//                               (temporally-extended macro-actions)
//   createTeamOptionMcts(cfg) — team-scoped option search
//
// MctsConfig fields (all optional):
//   iterations, budgetMs, rolloutHorizon, simDt, actionRepeat, uctC, seed,
//   pwAlpha                        — progressive widening α (0 disables)
//   priorC                         — PUCT weight (0 ⇒ plain UCT with uctC)
//   tacticWindowDecisions          — LayeredPlanner / TacticMcts only
//   optionMaxWindows               — OptionMcts / TeamOptionMcts only; cap
//                                    on in-tree option execution length
//   useLeafValue                   — skip rollout entirely and return the
//                                    evaluator's value at the expand site.
//                                    Pair with a strong heuristic/learned
//                                    evaluator to decouple depth from cost.
//   rolloutPolicy : "random" | "aggressive" | "scripted"
//                 | function(selfView, worldView) => CombatAction
//   opponentPolicy: "idle"   | "aggressive" | "scripted"
//   prior         : "uniform" | "attackBias" | "tacticMatch"
//                 | function(selfView, worldView, actions[]) => weights[]
//                   (tacticMatch reads `tactic`, `tacticMatchWeight`,
//                    `tacticOtherWeight`)
//   evaluator     : "hpDelta"      (hero-scoped)
//                 | "teamHpDelta" | "teamAdvantage" | "teamPosition"  (team)
//                 | function(worldView, heroId|teamId) => number in [-1, 1]
//
// JS callbacks receive plain-object views:
//   selfView/agentView : { id, teamId, x, z, yaw, hp, maxHp, alive, attackRange }
//   worldView          : { agents: [agentView, ...] }
// Rollout/prior run many times per search; prefer C++ presets for hot paths.
//
// A CombatAction is { moveDir, attackSlot, abilitySlot }.
// A Tactic is { kind: "Hold" | "FocusLowestHp" | "Scatter" | "Retreat" }.
// Move direction ints are exposed at bro.ai.game.MOVE_DIR, tactic strings at
// bro.ai.game.TACTIC.

// Single-agent: one hero vs scripted rest
const mcts = bro.ai.game.createMcts({
    iterations: 800, budgetMs: 10, rolloutHorizon: 24,
    rolloutPolicy: "aggressive", opponentPolicy: "aggressive",
    prior: "attackBias", evaluator: "hpDelta",
});
const action = mcts.search(world, hero);
mcts.advanceRoot(action);
console.log(mcts.lastStats); // { iterations, bestVisits, elapsedMs, reusedRoot, ... }

// Decoupled 1v1: both sides searched simultaneously (no opponentPolicy)
const duel = bro.ai.game.createDecoupledMcts({ iterations: 1500, prior: "attackBias" });
const joint1v1 = duel.search(world, hero, opp); // { hero: CombatAction, opp: CombatAction }
duel.advanceRoot(joint1v1.hero, joint1v1.opp);

// Cooperative team: one planner, joint action per hero
const team = bro.ai.game.createTeamMcts({
    iterations: 1200, rolloutHorizon: 20,
    rolloutPolicy: "aggressive", opponentPolicy: "aggressive",
    evaluator: "teamHpDelta",
});
const perHero = team.search(world, heroes); // [CombatAction, ...]  (length = heroes.length)
team.advanceRoot(perHero);

// Hierarchical: tactic every N windows + fine per-hero every call
const planner = bro.ai.game.createLayeredPlanner({
    tactic: { iterations: 300, budgetMs: 4, tacticWindowDecisions: 4, actionRepeat: 4 },
    fine:   { iterations: 600, budgetMs: 6, actionRepeat: 4, priorC: 2.0 },
    rolloutPolicy: "aggressive",
    opponentPolicy: "aggressive",
    evaluator: "teamHpDelta",
});
const groupActions = planner.decide(world, heroes); // [CombatAction, ...]
console.log(planner.committedTactic.kind);          // "FocusLowestHp", etc.
console.log(planner.windowsUntilReplan);
console.log(planner.lastStats.fineStats.iterations);

// Helpers for custom planners / UI debug:
const legalA = bro.ai.game.legalActions(hero, world);     // [CombatAction, ...]
const legalT = bro.ai.game.legalTactics(world, heroes);   // [Tactic, ...]
const concrete = bro.ai.game.tacticToAction(              // Tactic → CombatAction
    { kind: bro.ai.game.TACTIC.FocusLowestHp }, hero, world);


// ─── Options (temporally-extended macro-actions) ──────────────────────────
//
// An Option is a policy with initiation + termination predicates. OptionMcts
// plans at the granularity of options rather than per-tick CombatActions —
// branching collapses from ~18 to the size of the option set, and each tree
// edge covers many windows, so the effective horizon multiplies by option
// length. The right tool for multi-tick maneuvers (peek/shoot, retreat to
// cover, flank) that plain search can't plan cheaply at realtime budgets.

// Author a single-hero option. All three callbacks run synchronously inside
// MCTS search — keep them allocation-light.
const peekAndShoot = bro.ai.game.createOption({
    name: "peekAndShoot",
    canInitiate:     (self, world) =>
        world.agents.some(a => a.alive && a.teamId !== self.teamId
                               && Math.hypot(a.x - self.x, a.z - self.z) < 12),
    step:            (self, world, ticks) => {
        const enemy = world.agents.find(a => a.alive && a.teamId !== self.teamId);
        return { moveDir: ticks < 2 ? 3 /*E*/ : 7 /*W*/,
                 attackSlot: enemy ? 0 : -1, abilitySlot: -1 };
    },
    shouldTerminate: (self, world, ticks) => ticks >= 4 || self.hp < self.maxHp * 0.3,
});
const retreat = bro.ai.game.createOption({
    name: "retreatToCover",
    canInitiate:     (self) => self.hp < self.maxHp * 0.5,
    step:            ()     => ({ moveDir: 10 /*PathAway*/, attackSlot: -1, abilitySlot: -1 }),
    shouldTerminate: (self, _w, ticks) => ticks >= 5 || self.hp > self.maxHp * 0.8,
});
const hold = bro.ai.game.createOption({
    name: "hold",
    canInitiate:     ()     => true,
    step:            ()     => ({ moveDir: 0, attackSlot: 0, abilitySlot: -1 }),
    shouldTerminate: (_s, _w, ticks) => ticks >= 2,
});

const opt = bro.ai.game.createOptionMcts({
    iterations: 80, rolloutHorizon: 3, optionMaxWindows: 6,
    options: [peekAndShoot, retreat, hold],
    opponentPolicy: "scripted",
    evaluator: "hpDelta",
    useLeafValue: true,                 // skip random rollouts; use eval at leaf
});
const chosen = opt.search(world, hero); // "peekAndShoot" | "retreatToCover" | "hold" | null
if (chosen) {
    opt.executeOption(world, hero, chosen);  // advance the live world
    opt.advanceRoot(chosen);                  // reuse tree next call
}

// Team variant — callbacks receive heroesView[] and step returns a
// CombatAction[] (one per hero, in order).
const teamPush = bro.ai.game.createTeamOption({
    name: "push",
    canInitiate: (heroes, world) =>
        world.agents.some(a => a.alive && heroes[0] && a.teamId !== heroes[0].teamId),
    step: (heroes) => heroes.map(() => ({ moveDir: 9 /*PathToTarget*/, attackSlot: 0, abilitySlot: -1 })),
    shouldTerminate: (_h, _w, ticks) => ticks >= 4,
});
const teamOpt = bro.ai.game.createTeamOptionMcts({
    iterations: 60, optionMaxWindows: 4,
    options: [teamPush],
    evaluator: "teamAdvantage",
    opponentPolicy: "scripted",
});
const chosenTeam = teamOpt.search(world, heroes);  // option name or null


// ─── Commander (role-based hierarchical team planner) ────────────────────
//
// Assigns heroes to roles (lead / flank / support / etc.); each role owns
// an option set and optional per-role evaluator. Per-hero OptionMcts runs
// within the role's option space — no joint-action combinatorial search.
// Role re-assignment fires every `replanEveryWindows` decide() calls, or
// whenever the team composition changes.
//
// This is the "group thinking" primitive. Use it when joint-space search
// (TeamMcts / TeamOptionMcts) is too wide at your iteration budget.

const leadOpts    = [peekAndShoot, hold];
const supportOpts = [retreat, hold];
const cmdr = bro.ai.game.createCommander({
    replanEveryWindows: 3,
    roleCfg: { iterations: 80, rolloutHorizon: 3, optionMaxWindows: 4, useLeafValue: true },
    opponentPolicy: "scripted",
    evaluator: "hpDelta",                  // default for roles without their own
    roles: [
        { name: "lead",    options: leadOpts },
        { name: "support", options: supportOpts,
          evaluator: (worldView, heroId) => {
              // Support values ally HP preservation over damage dealt.
              const me = worldView.agents.find(a => a.id === heroId);
              if (!me) return 0;
              const allies = worldView.agents.filter(a => a.teamId === me.teamId && a.alive);
              const avg = allies.reduce((s, a) => s + a.hp / a.maxHp, 0) / Math.max(1, allies.length);
              return avg * 2 - 1;
          } },
    ],
    // Optional custom assigner. Default is round-robin by hero index.
    assign: (heroes, world) => heroes.map((h, i) => h.hp < h.maxHp * 0.4 ? 1 : 0),
});

const groupActions2 = cmdr.decide(world, heroes); // [CombatAction, ...]
console.log(cmdr.currentAssignments);             // [0, 1, 0, ...] role indices
console.log(cmdr.committedOption(0));             // "peekAndShoot" | null
console.log(cmdr.windowsUntilReplan);
console.log(cmdr.roles);                          // [{name, optionCount}, ...]
