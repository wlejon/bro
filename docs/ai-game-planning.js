// =============================================================================
// bro.ai.game: Agents, Worlds, Units, steering and perception
// =============================================================================
//
// The simulation half of the namespace: the agent that walks a path, the
// world that ticks a roster and resolves combat, the unit stat block each
// agent carries, the stateless steering kernels, and the scene bindings that
// drive all of it from the engine frame loop.
//
// Companion files (one area each, same library):
//   docs/ai-game-api.js       NavGrid / HexNav / NavMesh / routing
//   docs/ai-game-learning.js  MCTS family, planners, belief, simulation, replay
//   docs/ai-nn-api.js         bro.ai.game.nn: circuits, nets, ops, WeightsHandle
//   docs/ai-learn-api.js      bro.ai.game.learn: buffers, trainers, inference
//   docs/ai-game-tools.js     bro.ai.game.grid: obs windows, tapes, GridTrainer
//
// Available in all modes, windowed, headless and bro-server.

// -----------------------------------------------------------------------------
// Agent, Pathfinding + steering combined
// -----------------------------------------------------------------------------

/**
 * Create a game agent that navigates using a NavGrid.
 *
 * @param {Object} [opts]
 * @param {NavGrid} [opts.navGrid] - Navigation grid for pathfinding
 * @param {NavMesh} [opts.navMesh] - Navmesh for 3D routing (setGoal / see
 *                                  "Agent routing over a navmesh" in
 *                                  docs/ai-game-api.js)
 * @param {{x,y,z}|number[]} [opts.position] - initial position; when given it
 *                                  wins over x / y / z
 * @param {number} [opts.x=0] - Initial X position
 * @param {number} [opts.y=0] - Initial Y (alias of `elevation`)
 * @param {number} [opts.z=0] - Initial Z position
 * @param {number} [opts.speed=6] - Movement speed (units/second). `maxSpeed`
 *                                  is accepted as a synonym and wins.
 * @param {number} [opts.radius=0.4] - Collision radius
 * @param {number} [opts.maxAcceleration] - accel cap (`maxAccel` synonym);
 *                                  omitted leaves the engine default
 * @param {number} [opts.maxTurnRate] - radians/second turn cap
 * @param {number} [opts.elevation=0] - Vertical position (Y) for the ORCA
 *                                  multi-level elevation filter (see bot.elevation)
 * @param {boolean|Object} [opts.avoidance] - ORCA participation/tuning (see
 *                                  "Local avoidance" below)
 *
 * Unit stats, all forwarded into agent.unit (see "Unit" below):
 * @param {number} [opts.id=0]           - Unit::id, what world.findById matches
 * @param {number} [opts.teamId=0]
 * @param {number} [opts.hp=100]
 * @param {number} [opts.maxHp=hp]
 * @param {number} [opts.mana=0]
 * @param {number} [opts.maxMana=100]
 * @param {number} [opts.damage=10]
 * @param {number} [opts.attackRange=3]
 * @param {number} [opts.attacksPerSec=1]
 * @param {number} [opts.armor=0]
 * @param {number} [opts.magicResist=0]
 * @returns {AIAgent}
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
 * Follow an externally planned route verbatim: the waypoints are walked in
 * order with the usual steering/avoidance/dynamics, and are never re-planned
 * by NavGrid A* — the embedder owns the route (its own pathfinder, terrain
 * costs the grid can't express), the agent owns the movement. The final
 * waypoint becomes the target for hasTarget/atTarget; an empty array is
 * clearTarget(). A later setTarget at the route's own goal keeps the route.
 * Routes are not captured by snapshots — re-issue them after applySnapshot.
 * @param {Array<{x:number,z:number}|[number,number]>} waypoints - world XZ, in order
 */
bot.setPath([{ x: 0, z: 8 }, { x: 8, z: 8 }]);

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

/**
 * Vertical position (Y) for multi-level worlds: read/write. Only feeds the
 * ORCA elevation filter (see avoidance.height): agents on different levels
 * don't steer around each other. Movement itself stays XZ. Also accepted as
 * `elevation:` in createAgent(opts). Scene-attached agents with groundFollow
 * or a navmesh route get it updated automatically from the surface height.
 */
bot.elevation = 4;

/** Current facing direction in radians (read-only). 0 = -Z, positive = clockwise. */
bot.yaw;

/** Whether the agent has an active target (read-only). */
bot.hasTarget;

/** Whether the agent has reached its target (read-only). */
bot.atTarget;

/**
 * 3D position, read AND write. Reading gives {x, y, z} where y is the
 * elevation (or the route height while a navmesh route is active). Writing
 * takes a {x,y,z} object or [x,y,z] array and teleports.
 */
bot.position;
bot.position = { x: 4, y: 0, z: -2 };
bot.getPosition();                 // same value as the getter, as a method

/** Current velocity as {x, y, z}; y is always 0 (movement is XZ).
 *  Read-only as a property; getVelocity() is the method form. */
bot.velocity; bot.getVelocity();

/** The agent's Unit stat block (see "Unit" below). Read-only handle, but its
 *  own fields are writable. Created lazily on first access and cached. */
bot.unit;

/** Speed cap. `maxSpeed` and `speed` are the same accessor, read/write. */
bot.maxSpeed = 8; bot.speed;

/** Collision radius, read/write. */
bot.radius = 0.5;

/** Acceleration cap. `maxAccel` and `maxAcceleration` are the same accessor.
 *  NOTE: the GETTER reads the unit's moveSpeed field, not the steering accel
 *  cap; treat it as write-mostly and keep your own copy if you need the
 *  value back. */
bot.maxAccel = 30;

/** Path following (read-only): the remaining waypoint list and the index of
 *  the one being walked toward. */
bot.path;               // [{x, z}, ...]
bot.currentWaypoint;    // index into path

/** Aim angles, driven by aimAt() (read-only). */
bot.aimYaw; bot.aimPitch;

/**
 * Set a 3D goal. With a navmesh bound (createAgent({navMesh}) or setNavMesh)
 * this plans a route on the mesh and follows it, and update() advances the
 * waypoints; without one it degrades to setTarget(x, z). Accepts
 * (x, y, z), (x, z), or a {x,y,z} / [x,y,z] object.
 */
bot.setGoal(10, 0, 5);

/** Teleport. Accepts (x, y, z), (x, z), or a {x,y,z} object. Also clears any
 *  navmesh route height tracking. */
bot.setPosition(0, 0, 0);

/** Individual setters; each mirrors the accessor of the same name. */
bot.setSpeed(8);
bot.setRadius(0.5);
bot.setMaxAccel(30);
bot.setMaxTurnRate(8);          // radians/second
bot.setVelocity(vx, vz);        // XZ, overrides steering until the next update
bot.setYaw(Math.PI / 2);

/** Rebind navigation at runtime. Passing anything that is not a nav handle
 *  UNBINDS (the agent then walks straight lines). */
bot.setNavGrid(otherGrid);
bot.setNavMesh(otherMesh);
bot.setNavGrid(null);

/** `stop()` is a synonym for `clearTarget()`: it drops the target, the
 *  navmesh route, and the path. */
bot.stop();

/**
 * Permanently disable the agent: drops its target, route, and nav binding.
 * A destroyed agent ignores update() and setGoal(); remove it from its world
 * as well. There is no undo.
 */
bot.destroy();

/**
 * Bind a standalone AgentBinding (the same object node.attachAgent creates
 * internally), for driving navmesh routes without a scene node.
 * @param {Object} [opts]
 * @param {NavMesh} [opts.navMesh] @param {NavGrid} [opts.navGrid]
 * @param {number} [opts.yOffset=0] @param {number} [opts.repathInterval=0]
 * @returns {AIAgentBinding} with .agent, .navigateTo(), .stopNavigation(),
 *     .navigationInfo(), and .step(dt) (alias .update(dt))
 */
const binding = bot.bind({ navMesh, yOffset: 0.5, repathInterval: 1.0 });
binding.navigateTo({ x: 8, y: 0, z: 0 });
binding.step(1 / 60);

/**
 * Continuous-control entry point: apply one AgentAction for dt seconds. This
 * is what a learned policy drives, as opposed to the path-following
 * update(dt). The action is
 *   { moveX, moveZ, aimYaw, aimPitch, attackTargetId, abilitySlot, abilityTargetId }
 * @param {Object} action @param {number} dt
 * @throws {TypeError} when either argument is missing
 */
bot.applyAction({ moveX: 1, moveZ: 0, aimYaw: 0, attackTargetId: -1 }, 1 / 60);


// -----------------------------------------------------------------------------
// Agent-local steering: one-shot velocity overrides
// -----------------------------------------------------------------------------
//
// Unlike bro.ai.game.steer.* (pure functions, below), these WRITE the
// agent's velocity as a side effect: the computed direction times the
// agent's speed. Call one per tick in place of update()'s path following,
// then integrate with update(dt). Each also returns the raw {fx, fz}.

bot.seek(targetX, targetZ);           // or bot.seek({x, z})
bot.flee(threatX, threatZ);
bot.arrive(targetX, targetZ, /*slowingRadius*/ 3.0);   // or ({x,z}, slowR)

/** Random walk: a jittered point on a circle `dist` ahead of the facing.
 *  Defaults radius 2, dist 3, jitter 0.5. Uses a thread-local RNG, so it is
 *  NOT reproducible across runs — seed your own if you need determinism. */
bot.wander(2, 3, 0.5);

/** Steer around AABBs in the direction of travel. Returns {fx: 0, fz: 0}
 *  (and writes a zero velocity) when nothing is on the ray.
 *  @param {Array<{x,z,hw,hd}>} obstacles @param {number} [lookahead=1] seconds */
bot.avoid(obstacles, 1.0);


// -----------------------------------------------------------------------------
// Local avoidance (ORCA), agents stop walking through each other
// -----------------------------------------------------------------------------
//
// Optimal Reciprocal Collision Avoidance, solved natively inside
// world.tick(). Off by default (agents keep the legacy pass-through-each-
// other movement). When enabled, each living agent's path-following
// steering becomes its *preferred* velocity, the ORCA solver filters it
// against nearby agents (the pair splits the avoidance effort by their
// priorities, 50/50 by default, see avoidance.priority) and static walls,
// and the filtered velocity drives the agent's usual dynamics (maxAccel /
// maxTurnRate / nav-grid clamping still apply). avoidance.layers/mask
// scope who avoids whom. Deterministic: the same roster + obstacles +
// ticks replay identically.
//
// Scene-attached agents get this for free, attachAIWorld ticks the same
// world, so think() callbacks issuing self.moveTo(...) produce paths that
// flow around other agents.

/**
 * Enable/disable the avoidance pass on a world.
 *
 * Walls: the world's own addObstacle() AABBs are always respected while
 * avoidance is on. Passing a navGrid additionally bases avoidance-only
 * walls on that grid's obstacle boxes, so agents locally steer around the
 * exact geometry A* paths around, and since createNavGrid({fromPhysics})
 * bakes obstacles from static physics bodies, that composition makes
 * avoidance physics-aware for free. Boxes are copied (no reference kept);
 * call again after mutating the grid.
 *
 * @param {boolean|Object} opts - boolean, or:
 * @param {boolean} [opts.enabled=true]
 * @param {NavGrid} [opts.navGrid] - rebase avoidance walls on this grid's obstacles
 */
world.setAvoidance(true);
world.setAvoidance({ navGrid: nav });   // enable + respect the grid's walls
world.setAvoidance(false);

/** Whether the avoidance pass is enabled (read-only). */
world.avoidanceEnabled;

/**
 * Per-agent participation + tuning. Also accepted as `avoidance:` in
 * createAgent(opts) and node.attachAgent(world, agent, opts).
 * `false` opts the agent out: it keeps legacy (unfiltered) movement but
 * others still steer around it at full effort: good for bosses or
 * player-driven units that shouldn't yield.
 *
 * @param {boolean|Object} opts - boolean, or:
 * @param {boolean} [opts.enabled=true]
 * @param {number}  [opts.radius]            - avoidance disc radius (default: agent radius)
 * @param {number}  [opts.maxSpeed]          - speed cap on the solved velocity (default: agent speed)
 * @param {number}  [opts.neighborDist=10]   - only agents within this range are considered
 * @param {number}  [opts.maxNeighbors=10]   - nearest-N cap on the neighbor set
 * @param {number}  [opts.timeHorizon=2]     - seconds of mutual lookahead vs agents
 * @param {number}  [opts.timeHorizonObst=1] - seconds of lookahead vs walls
 * @param {number}  [opts.height=2]          - vertical extent for the multi-level
 *   elevation filter: agents whose spans [elevation - height/2, elevation +
 *   height/2] don't overlap are on different levels (bridge over tunnel,
 *   stacked floors) and ignore each other. Elevations default to 0, so
 *   single-level worlds never filter.
 * @param {number}  [opts.priority=0.5]      - avoidance responsibility weight,
 *   0..1. When two agents negotiate, each takes the effort share
 *       share = clamp(0.5 + 0.5 * (otherPriority - selfPriority), 0, 1),
 * shares sum to 1 across the pair, so ORCA's collision-free reciprocity
 *   is preserved. Equal priorities keep the classic 50/50; the LOWER-priority
 *   agent takes proportionally more, and at the extremes (1 vs 0) the
 *   low-priority agent does all the avoiding while the high-priority one
 *   holds course (think boss vs minions). Deterministic.
 * @param {number}  [opts.layers=1]          - layer membership bitmask (which
 *   avoidance layers this agent occupies)
 * @param {number}  [opts.mask=1]            - neighbor-selection bitmask: agent
 *   A avoids neighbor B only when (A.mask & B.layers) !== 0. One-sided by
 *   design: B may still avoid A if B's mask matches A's layers, and an agent
 *   avoiding a neighbor that cannot see it back automatically takes the FULL
 *   effort (no reciprocity to count on). Default 1/1 = everyone avoids
 *   everyone.
 */
bot.setAvoidance({ radius: 0.5, timeHorizon: 2.5 });
// A boss that plows through the crowd; minions scatter:
boss.setAvoidance({ priority: 1.0 });
// Ghosts and the living never steer around each other:
ghost.setAvoidance({ layers: 2, mask: 2 });

// Typical setup: shared nav grid + avoidance, then just set targets.
const arena = bro.ai.game.createNavGrid({
    minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5,
    obstacles: [{ x: 0, z: 0, hw: 3, hd: 0.5 }],
});
const world2 = bro.ai.game.createWorld();
world2.setAvoidance({ navGrid: arena });
const a1 = bro.ai.game.createAgent({ navGrid: arena, x: -10, z: 0, avoidance: { radius: 0.5 } });
const a2 = bro.ai.game.createAgent({ navGrid: arena, x: 10, z: 0 });
world2.addAgent(a1); world2.addAgent(a2);
a1.setTarget(10, 0); a2.setTarget(-10, 0);   // they pass, not overlap
// world2.tick(dt) each frame, or scene.attachAIWorld(world2).


// -----------------------------------------------------------------------------
// World, the roster, the combat resolver, the tick
// -----------------------------------------------------------------------------
//
// A World owns nothing: it holds pointers to agents you created and keeps a
// roster so queries can hand the JS wrappers back. Remove an agent before
// destroying it. One tick advances scripted behaviour, the ORCA pass (when
// enabled), projectiles, and cooldowns.

/** @returns {AIWorld} */
const world = bro.ai.game.createWorld();

world.addAgent(bot);
world.removeAgent(bot);
world.agentCount;                 // read-only

/** Static avoidance walls, independent of any NavGrid. Always respected
 *  while avoidance is on. @param {{x,z,hw,hd}} box */
world.addObstacle({ x: 0, z: 0, hw: 2, hd: 2 });

/** Advance the simulation. `step(dt)` is a synonym for `tick(dt)`. */
world.tick(1 / 60);

/** Find an agent's JS wrapper by its Unit::id.
 *  @returns {AIAgent|null} */
const who = world.findById(7);

/** Nearest living enemy of `agent`, or null.
 *  @param {AIAgent} agent @returns {AIAgent|null} */
const foe = world.nearestEnemy(bot);

/** All living enemies / allies within `range` of `agent`.
 *  @returns {AIAgent[]} */
world.enemiesInRange(bot, 12);
world.alliesInRange(bot, 12);

/**
 * Damage events since the last clearEvents(), in order. `events` and
 * `damageEvents` are two names for the same read-only list.
 * Each entry: { sourceId, attackerId, targetId, amount, kind, killed }
 * (`sourceId` and `attackerId` are the same number, kept for both spellings;
 * `kind` is "physical" | "magical" | "true").
 */
for (const e of world.events) if (e.killed) onKill(e.attackerId, e.targetId);
world.clearEvents();
// Anything that reads the event window — createRewardTracker.consume(),
// recorder.recordFrame() — consumes it from the same list, so do not clear
// between their calls or that window is lost.

/** Resolve one basic attack through the full pipeline: range, cooldown,
 *  armor/resist, crit roll, events. @returns {boolean} whether it landed */
world.resolveAttack(bot, targetUnitId);

/** Resolve an ability cast: cooldown + mana + range, then the spec's fn.
 *  @param {AIAgent} caster @param {number} slot @param {number} targetId
 *  @returns {boolean} */
world.resolveAbility(bot, 0, targetUnitId);

/** Apply damage directly, bypassing range and cooldown but not mitigation.
 *  @param {AIAgent} attacker @param {AIAgent} target @param {number} amount
 *  @param {"physical"|"magical"|"true"} [kind="physical"]
 *  @returns {number} the amount actually dealt after mitigation */
const dealt = world.dealDamage(bot, foe, 25, "magical");

/**
 * Register a JS-authored ability under an integer id. Bind it to a unit slot
 * with unit.setAbilitySlot(slot, abilityId), then cast it with
 * resolveAbility.
 *
 * `fn(caster, world, targetId)` runs from the NATIVE cast path — including
 * inside an MCTS rollout on a cloned World, where `caster` is a clone's agent
 * with no JS wrapper and so arrives as undefined. Guard for that.
 *
 * @param {number} abilityId
 * @param {Object} spec
 * @param {number} [spec.cooldown=1]
 * @param {number} [spec.manaCost=0]
 * @param {number} [spec.range=0]
 * @param {function(AIAgent|undefined, AIWorld, number)} [spec.fn]
 */
world.registerAbility(3, {
    cooldown: 6, manaCost: 40, range: 8,
    fn(caster, w, targetId) {
        const target = w && w.findById(targetId);
        if (caster && target) w.dealDamage(caster, target, 60, "magical");
    },
});

/** Reseed the world RNG: crit rolls, attack spread, dodge. Set it for a
 *  reproducible episode. @param {number|bigint} seed */
world.seed(0xC0DE1234);

/** Spawn a projectile on this world. Same option object as the free
 *  bro.ai.game.spawnProjectile(world, opts) — see docs/ai-game-learning.js.
 *  @returns {number} the id, or -1 */
world.spawnProjectile({ ownerId: bot.unit.id, x: 0, z: 0, vx: 20, vz: 0, damage: 25 });

/** Live projectiles, read-only: [{id, ownerId, teamId, x, z, vx, vz, speed,
 *  damage, alive, mode}, ...]. Dead ones are filtered out. */
world.projectiles;

/**
 * Plain-object snapshot / restore of the roster. Captures per agent:
 * id, x, z, vx, vz, yaw, aimYaw, aimPitch, speed, radius, hp, maxHp, mana,
 * teamId, hasTarget, targetX, targetZ — plus nextProjectileId at the top.
 * NOTE it does NOT carry projectiles or events; for a lossless capture use
 * bro.ai.game.captureWorldSnapshot (docs/ai-game-learning.js).
 * @returns {Object}
 */
const snap = world.snapshot();
world.restore(snap);

/** Avoidance: see "Local avoidance" above for setAvoidance and
 *  avoidanceEnabled. */


// -----------------------------------------------------------------------------
// Unit, the stat block behind every agent
// -----------------------------------------------------------------------------
//
// `agent.unit` is a live view, not a copy: writing a field writes the agent.
// Every base field is read/write, so an ability can just assign.

const u = bot.unit;

// Identity and base stats (read/write):
u.id; u.teamId;
u.hp; u.maxHp; u.mana; u.maxMana; u.manaRegenPerSec;
u.damage; u.attackRange; u.attacksPerSec;
u.armor; u.magicResist;
u.moveSpeed; u.radius;
u.attackCooldown;                   // seconds until the next basic attack
u.attackKind = "magical";           // "physical" | "magical" | "true"

// Derived (read-only): base plus the active buff, so read these when
// resolving, not the base field.
u.alive;
u.effectiveArmor;                   // armor + armorBonus
u.effectiveMagicResist;             // magicResist + magicResistBonus
u.effectiveDamage;                  // damage * damageMul
u.effectiveMoveSpeed;               // moveSpeed * moveSpeedMul
u.effectiveAttacksPerSec;           // attacksPerSec * attacksMul

/** Decay cooldowns, buff timers and DoT/HoT by dt. world.tick() does this
 *  for rostered agents; call it yourself for an agent you tick by hand. */
u.tickCooldowns(1 / 60);

/** Ability slots. `abilityId` is what world.registerAbility registered.
 *  @param {number} slot @param {number} abilityId */
u.setAbilitySlot(0, 3);
u.getAbilitySlot(0);                // the bound ability id, or -1
u.getAbilityCooldown(0);            // seconds remaining

/** Apply damage to this unit with mitigation, no attacker and no event.
 *  @param {number} amount @param {"physical"|"magical"|"true"} [kind="physical"]
 *  @returns {number} the amount actually taken */
u.takeDamage(30, "magical");

// --- Timed buffs, DoT and HoT (all read/write) ---
//
// Each effect is a magnitude plus a `...Remaining` duration in seconds.
// Apply one by setting both; tickCooldowns decays the duration and drops the
// magnitude back to its default when it hits zero. Multipliers default to 1,
// additive bonuses to 0.
//
//   u.armorBonus        / u.armorBonusRemaining          (default 0)
//   u.magicResistBonus  / u.magicResistBonusRemaining    (default 0)
//   u.damageMul         / u.damageMulRemaining           (default 1)
//   u.attacksMul        / u.attacksMulRemaining          (default 1)
//   u.moveSpeedMul      / u.moveSpeedMulRemaining        (default 1)
//   u.stealthChance     / u.stealthChanceRemaining       (default 0)
//   u.dotDps            / u.dotRemaining                 (damage per second)
//   u.hotRate           / u.hotRemaining                 (healing per second)
//
//   u.dotSourceId       - who gets credit for a DoT kill (-1 for nobody)
//   u.dotKind           - "physical" | "magical" | "true"

// A 4-second 30% slow plus a 3-second burn credited to the caster:
u.moveSpeedMul = 0.7; u.moveSpeedMulRemaining = 4;
u.dotDps = 12; u.dotRemaining = 3; u.dotKind = "magical"; u.dotSourceId = caster.unit.id;


// -----------------------------------------------------------------------------
// Perception, Line of sight, aim computation
// -----------------------------------------------------------------------------

/**
 * 2D line-of-sight check through AABB obstacles.
 *
 * @param {number} fromX
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toZ
 * @param {Array<{x, z, hw, hd}>|NavGrid} obstacles - AABBs to test against,
 *     OR a createNavGrid() handle, in which case the test walks that grid's
 *     blocked cells instead (nav.hasLineOfSight with the same result)
 * @returns {boolean} true if line is clear
 */
const clear = bro.ai.game.hasLineOfSight(
    botX, botZ, enemyX, enemyZ,
    obstacles
);
const clearOnGrid = bro.ai.game.hasLineOfSight(botX, botZ, enemyX, enemyZ, nav);

/**
 * 2D field-of-view and line-of-sight check through AABB obstacles.
 *
 * @param {number} fromX
 * @param {number} fromZ
 * @param {number} toX
 * @param {number} toZ
 * @param {number} facingYaw - Facing direction in radians (0 = -Z)
 * @param {number} fovRadians - Field of view arc in radians (e.g. Math.PI / 2 for 90°)
 * @param {number} maxRange - Sight range limit in world units
 * @param {Array<{x, z, hw, hd}>} obstacles - AABBs to test against
 * @returns {boolean} true if target is within FOV arc, within maxRange, and unobstructed
 */
const canSee = bro.ai.game.canSee(
    botX, botZ, enemyX, enemyZ,
    facingYaw, Math.PI / 2, 15, obstacles
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
 * @returns {{ valid: boolean, yaw: number, pitch: number }} `valid` is always
 *     true for a successful call; it returns null when given fewer than two
 *     points
 */
const aim2 = bro.ai.game.computeAim(0, 1.6, 0, 10, 1.6, -5);
// Two {x,y,z} objects work as well:
const aim3 = bro.ai.game.computeAim({ x: 0, y: 1.6, z: 0 }, enemyPos);

/**
 * Lead a moving target with a finite-speed projectile. Solves for the future
 * intercept point and returns the aim angles to that point, plus a `valid`
 * flag (false if no real intercept exists, e.g. target outrunning projectile)
 * and the predicted time-of-flight.
 *
 * @param {number} fromX  @param {number} fromY  @param {number} fromZ
 * @param {number} targetX @param {number} targetY @param {number} targetZ
 * @param {number} targetVX @param {number} targetVY @param {number} targetVZ
 * @param {number} projectileSpeed
 * @returns {{ yaw: number, pitch: number, valid: boolean, timeToHit: number }}
 */
const lead = bro.ai.game.computeLeadAim(
    0, 1.6, 0,             // shooter
    10, 1.6, -5,           // target pos
    -2, 0, 1,              // target velocity
    40);                   // projectile speed
if (lead.valid) bot.fire(lead.yaw, lead.pitch);


// -----------------------------------------------------------------------------
// bro.ai.game.steer.*, pure-function steering primitives
// -----------------------------------------------------------------------------
//
// Stateless 2D steering kernels. Each returns a desired-velocity direction
// `{fx, fz}` (NOT normalized), the caller integrates it into actual motion
// (multiply by speed * dt, clamp, blend, etc.). Use these inside custom
// think() callbacks or scripted policies when Agent's built-in path-following
// isn't the right behavior. All positions and velocities are XZ-plane.

/** Move directly toward `target` at full desired speed.
 *  @returns {{fx: number, fz: number}} */
const s1 = bro.ai.game.steer.seek(selfX, selfZ, targetX, targetZ);

/** Seek with deceleration once within `slowingRadius` of `target`. Used
 *  by Agent for the final waypoint so it doesn't overshoot.
 *  @param {number} slowingRadius */
const s2 = bro.ai.game.steer.arrive(selfX, selfZ, targetX, targetZ, 1.5);

/** Move directly away from a threat point. */
const s3 = bro.ai.game.steer.flee(selfX, selfZ, threatX, threatZ);

/** Lead a moving target. Seeks the predicted future position assuming
 *  constant target velocity. `selfSpeed` sets the lookahead horizon.
 *  @param {number} targetVX @param {number} targetVZ @param {number} selfSpeed */
const s4 = bro.ai.game.steer.pursue(
    selfX, selfZ, targetX, targetZ, targetVX, targetVZ, selfSpeed);

/** Inverse of pursue, flee from the threat's predicted future position.
 *  @param {number} threatVX @param {number} threatVZ @param {number} selfSpeed */
const s5 = bro.ai.game.steer.evade(
    selfX, selfZ, threatX, threatZ, threatVX, threatVZ, selfSpeed);


// -----------------------------------------------------------------------------
// Capability / policy / AgentBinding, scene-driven AI
// -----------------------------------------------------------------------------
//
// A scene node can own an "agent binding", a capability set (the tools this
// object can use) plus a JS think(self, world) callback (how it decides).
// Minions, towers, and heroes all use the same binding shape; behaviour
// differs only by which capabilities are enabled and which think() fn is
// supplied. Difficulty scales along three orthogonal knobs:
//   1. capability set, add or remove tools
//   2. thinkHz, how often the decision fires
//   3. think fn, simple scripted, MCTS wrapper, or NN policy
//
// Built-in capabilities (string ids used in opts.capabilities):
//   "move_to", self.moveTo(x, z) sets the pathfinding target
//   "lane_walk", self.laneWalk() steps through opts.laneWaypoints
//   "basic_attack", self.attack(targetId); blocks for 1/attacksPerSec
//   "cast_ability", self.cast(slot, targetId); blocks for cast time (~0.25s)
//   "flee", self.flee([x, z]); retreats away from nearest enemy
//   "hold", self.hold([dur]); no-op for dur seconds (always exposed)
//
// A `self` proxy is built fresh each think tick. It only exposes methods
// whose capability is present on the binding, towers won't have .moveTo.
//
// Custom capabilities (registerCapability, below) have no dedicated
// self.<name>() accessor of their own, invoke them with:
//   self.useCapability(name, arg0?, arg1?)
// exposed whenever the binding's capabilities list contains at least one
// registerCapability'd id. arg0/arg1 land in the capability's Action as
// i0/i1 (same slots self.attack/self.cast use for target/slot ids) but
// aren't visible to gate/start/advance below. Read them via a JS-side
// closure handoff instead, same as self.attack/self.cast's targets aren't
// passed to the *native* capability callbacks either.


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
 *
 * LIMITATION, current build: the registration allocates and returns an id but
 * does NOT store `gate` / `start` / `advance` — the callbacks are never
 * invoked. Treat this as id allocation only, and drive custom behaviour from
 * the binding's own think() for now.
 */
bro.ai.game.registerCapability("kite", {
    gate()    { return true; },
    start()   { /* ... */ },
    advance() { return true; /* done this tick */ },
});


// -----------------------------------------------------------------------------
// SceneGraph.attachAIWorld, auto-tick a World each frame
// -----------------------------------------------------------------------------

/**
 * Drive a brogameagent::World from the engine frame loop at a fixed step.
 * Replaces the JS-side accumulator pattern (see broworkshop's ai/ai-arena/main.js).
 *
 * @param {AIWorld} world
 * @param {Object}  [opts]
 * @param {number}  [opts.stepHz=60]            - fixed-step rate
 * @param {number}  [opts.maxStepsPerFrame=8]   - catch-up clamp for stalls
 */
scene.attachAIWorld(world, { stepHz: 60, maxStepsPerFrame: 8 });
scene.detachAIWorld();


// -----------------------------------------------------------------------------
// SceneNode.attachAgent, bind an AI agent to a scene object
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
 * @param {number}  [opts.yOffset=0]    - node Y (absolute), or clearance above
 *                                        the ground when groundFollow is set
 * @param {boolean} [opts.faceMovement=true]
 * @param {Array<{x,z}>} [opts.laneWaypoints] - waypoints for lane_walk
 * @param {string}  [opts.policy]       - "scripted_minion" as a C++ fallback
 * @param {boolean|Object} [opts.avoidance] - ORCA participation/tuning for the
 *                                        bound agent (see "Local avoidance"
 *                                        above); effective while the world has
 *                                        world.setAvoidance(true)
 * @param {NavMesh} [opts.navMesh]      - bind a bakeNavMesh()/loadNavMesh()
 *                                        handle, enabling node.navigateTo()
 *                                        route-following (see "Agent routing
 *                                        over a navmesh" above)
 *
 * Ground follow: agents plan in 2D (x, z); groundFollow makes the bound
 * node's Y track the ground under the agent instead of sitting at a constant
 * height. The probe runs natively once per frame; when it has no answer
 * (chunk not streamed in / nothing under the ray) the node keeps the last
 * known ground height.
 * @param {Object}  [opts.groundFollow]
 * @param {string}  opts.groundFollow.mode - 'terrain' (sample a voxel terrain's
 *                                  surface) or 'raycast' (physics down-raycast
 *                                  against the default world's static geometry)
 * @param {Terrain} [opts.groundFollow.terrain] - required for mode 'terrain':
 *                                  a scene.createTerrain() object
 * @param {number}  [opts.groundFollow.rayStart=100] - world Y the down-probe starts from
 * @param {number}  [opts.groundFollow.rayLength=200] - probe length below rayStart
 * @param {Array<string|number>} [opts.groundFollow.layers] - raycast mode: only
 *                                  hit bodies on these collision layers
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

// Walk the terrain: yOffset becomes clearance above the sampled surface.
scoutNode.attachAgent(world, scoutAgent, {
    yOffset: 0.5,
    groundFollow: { mode: 'terrain', terrain: myTerrain },
    think(self) { return self.moveTo(30, -12); },
});

// Or follow physics floors/platforms via a downward raycast.
guardNode.attachAgent(world, guardAgent, {
    yOffset: 1.0,
    groundFollow: { mode: 'raycast', layers: ['static'] },
    think(self) { return self.hold(1); },
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
//   self.agent, the underlying AIAgent (for world queries)
//
// Universal helpers (always available):
//   self.distanceTo(target), target may be {x,z}, AIAgent, or self
//   self.inRange(target [, range]), default range is self.attackRange
//   self.hold([dur]), no-op fallback
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


