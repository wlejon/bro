// =============================================================================
// bro.ai.game: Game AI Search & Planning
// MCTS family, options, commander, belief / IS-MCTS, simulation, replay
// =============================================================================
//
// Companion files (one area each, same library):
//   docs/ai-game-api.js       NavGrid / HexNav / NavMesh / routing
//   docs/ai-game-planning.js  Agent, World, Unit, steering, perception, bindings
//   docs/ai-nn-api.js         bro.ai.game.nn: circuits, nets, ops, WeightsHandle
//   docs/ai-learn-api.js      bro.ai.game.learn: buffers, trainers, inference
//   docs/ai-game-tools.js     bro.ai.game.grid: obs windows, tapes, GridTrainer
//
// Available in all modes, windowed, headless and bro-server.

// -----------------------------------------------------------------------------
// MCTS planners
// -----------------------------------------------------------------------------
//
// Seven flavors, all sharing the same MctsConfig:
//
//   createMcts(cfg), single agent vs scripted opponents
//   createDecoupledMcts(cfg), simultaneous-move 1v1 (both sides searched)
//   createTeamMcts(cfg), cooperative N-hero joint planner
//   createTacticMcts(cfg), coarse team-tactic planner (Hold / FocusLowestHp / ...)
//   createLayeredPlanner({ tactic, fine }).
// TacticMcts over TeamMcts with a tactic-match prior
//   createOptionMcts(cfg), search over caller-authored single-hero Options
//                               (temporally-extended macro-actions)
//   createTeamOptionMcts(cfg), team-scoped option search
//
// MctsConfig fields (all optional):
//   iterations, budgetMs, rolloutHorizon, simDt, actionRepeat, uctC, seed,
//   pwAlpha, progressive widening α (0 disables)
//   priorC, PUCT weight (0 ⇒ plain UCT with uctC)
//   tacticWindowDecisions, LayeredPlanner / TacticMcts only
//   optionMaxWindows, OptionMcts / TeamOptionMcts only; cap
//                                    on in-tree option execution length
//   useLeafValue, skip rollout entirely and return the
//                                    evaluator's value at the expand site.
//                                    Pair with a strong heuristic/learned
//                                    evaluator to decouple depth from cost.
//   rolloutPolicy : "random" | "aggressive" | "scripted"
//                 | function(selfView, worldView) => CombatAction
//                 | a rollout OBJECT (see "MCTS primitives" below)
//   opponentPolicy: "idle"   | "aggressive" | "scripted"
//   prior         : "uniform" | "attackBias" | "tacticMatch"
//                 | function(selfView, worldView, actions[]) => weights[]
//                 | a prior OBJECT (createUniformPrior / createAttackBiasPrior
//                   / createTacticPrior / learn.createNeuralPrior / ...)
//                   (tacticMatch reads `tactic`, `tacticMatchWeight`,
//                    `tacticOtherWeight`)
//   evaluator     : "hpDelta"      (hero-scoped)
//                 | "teamHpDelta" | "teamAdvantage" | "teamPosition"  (team)
//                 | function(worldView, heroId|teamId) => number in [-1, 1]
//                 | an evaluator OBJECT (createHpDeltaEvaluator /
//                   learn.createNeuralEvaluator / ...)
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
mcts.resetTree();                  // drop the tree entirely
mcts.setConfig({ iterations: 1600 });
console.log(mcts.lastStats); // { iterations, bestVisits, elapsedMs, reusedRoot, ... }
// lastStats is a PROPERTY on the combat searchers; on GenericMcts (bottom of
// this file) it is a method, lastStats().

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

// Coarse tactic-only planner: one Tactic for the whole team per window.
const tac = bro.ai.game.createTacticMcts({
    iterations: 300, tacticWindowDecisions: 4, evaluator: "teamHpDelta",
});
const tactic = tac.search(world, heroes);     // a Tactic
tac.advanceRoot(tactic);
tac.resetTree();
tac.lastStats;

// Hierarchical: tactic every N windows + fine per-hero every call
const planner = bro.ai.game.createLayeredPlanner({
    tactic: { iterations: 300, budgetMs: 4, tacticWindowDecisions: 4, actionRepeat: 4 },
    fine:   { iterations: 600, budgetMs: 6, actionRepeat: 4, priorC: 2.0 },
    rolloutPolicy: "aggressive",
    opponentPolicy: "aggressive",
    evaluator: "teamHpDelta",
});
const groupActions = planner.decide(world, heroes); // [CombatAction, ...]
planner.reset();                                    // forget the committed tactic
console.log(planner.committedTactic.kind);          // "FocusLowestHp", etc.
console.log(planner.windowsUntilReplan);
console.log(planner.lastStats.fineStats.iterations);

// Root-parallel: N native threads, each with its own Mcts over its own
// World, joined and merged into one action. Caller must hand in an array of
// pre-built Worlds already seeded to the SAME game state. This does not
// clone `world` for you (a World doesn't own its Agents, so cloning would be
// unsafe to do implicitly). Blocking call: all worker threads are fully
// joined before it returns, so host callers never observe concurrency and there
// is no caller-side locking to worry about. Because of that same threading,
// `evaluator`/`rolloutPolicy` MUST be a string preset or a native/neural
// wrapper (e.g. bro.ai.game.learn.createNeuralEvaluator(...)), a plain host
// function is rejected with a TypeError, since N native threads calling
// back concurrently would be unsafe.
const parallelWorlds = [world1, world2, world3, world4]; // same state, N clones
const { action: pAction, stats: pStats } = bro.ai.game.rootParallelSearch({
    worlds: parallelWorlds, heroId: hero.id,
    iterations: 200, rolloutHorizon: 12, actionRepeat: 4, seed: 0xDEAD,
    evaluator: "hpDelta", rolloutPolicy: "aggressive", opponentPolicy: "aggressive",
});
console.log(pStats); // { numThreads, totalIterations, elapsedMs, mergedBestVisits }

// Decoupled root-parallel: same threading contract, no opponentPolicy (both
// sides searched simultaneously, like createDecoupledMcts).
const { hero: pHero, opp: pOpp } = bro.ai.game.rootParallelSearchDecoupled({
    worlds: parallelWorlds, heroId: hero.id, oppId: opp.id,
    iterations: 200, evaluator: "hpDelta", rolloutPolicy: "aggressive",
});

// Helpers for custom planners / UI debug:
const legalA = bro.ai.game.legalActions(hero, world);     // [CombatAction, ...]
const legalT = bro.ai.game.legalTactics(world, heroes);   // [Tactic, ...]
const concrete = bro.ai.game.tacticToAction(              // Tactic → CombatAction
    { kind: bro.ai.game.TACTIC.FocusLowestHp }, hero, world);

/** Apply a CombatAction to the LIVE world (the same path the search applies
 *  internally during a rollout). Use it to execute what search() chose.
 *  @param {AIAgent} agent @param {AIWorld} world @param {Object} action */
bro.ai.game.applyCombatAction(hero, world, action);


// ─── Options (temporally-extended macro-actions) ──────────────────────────
//
// An Option is a policy with initiation + termination predicates. OptionMcts
// plans at the granularity of options rather than per-tick CombatActions,
// branching collapses from ~18 to the size of the option set, and each tree
// edge covers many windows, so the effective horizon multiplies by option
// length. The right tool for multi-tick maneuvers (peek/shoot, retreat to
// cover, flank) that plain search can't plan cheaply at realtime budgets.

// Author a single-hero option. All three callbacks run synchronously inside
// MCTS search, keep them allocation-light.
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
peekAndShoot.name;                       // read-only, the authored name

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
opt.resetTree(); opt.setConfig({ iterations: 160 }); opt.lastStats;

// Team variant, callbacks receive heroesView[] and step returns a
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
if (chosenTeam) teamOpt.executeOption(world, heroes, chosenTeam);
teamOpt.advanceRoot(chosenTeam); teamOpt.resetTree(); teamOpt.lastStats;


// ─── Commander (role-based hierarchical team planner) ────────────────────
//
// Assigns heroes to roles (lead / flank / support / etc.); each role owns
// an option set and optional per-role evaluator. Per-hero OptionMcts runs
// within the role's option space, no joint-action combinatorial search.
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
cmdr.reset();                                     // drop assignments + trees
console.log(cmdr.currentAssignments);             // [0, 1, 0, ...] role indices
console.log(cmdr.committedOption(0));             // "peekAndShoot" | null
console.log(cmdr.windowsUntilReplan);
console.log(cmdr.roles);                          // [{name, optionCount}, ...]


// ─── MCTS primitives as first-class objects ──────────────────────────────
//
// The string presets have object equivalents. Pass them as `evaluator`,
// `prior` or `rolloutPolicy` in any Mcts / TacticMcts / OptionMcts /
// InfoSetMcts / LayeredPlanner / Commander config. Objects are what
// rootParallelSearch requires, since a JS function cannot be called from N
// native threads.

const hpEval    = bro.ai.game.createHpDeltaEvaluator();
const tHp       = bro.ai.game.createTeamHpDeltaEvaluator();
const tAdv      = bro.ai.game.createTeamAdvantageEvaluator();
const tPos      = bro.ai.game.createTeamPositionEvaluator();
const randRoll  = bro.ai.game.createRandomRollout();
const aggRoll   = bro.ai.game.createAggressiveRollout();
const scrRoll   = bro.ai.game.createScriptedRollout();
const uniformPr = bro.ai.game.createUniformPrior();
const atkBiasPr = bro.ai.game.createAttackBiasPrior();

/** The tactic-match prior: boosts actions that agree with the committed
 *  tactic and damps the rest. Only this one carries knobs. */
const tacticPr = bro.ai.game.createTacticPrior();
tacticPr.setMatchWeight(8.0);
tacticPr.setOtherWeight(1.0);
tacticPr.setTactic({ kind: bro.ai.game.TACTIC.FocusLowestHp });

// String constants mirror the enum values.
bro.ai.game.PROJECTILE_MODE.Single;      // "single" | "pierce" | "aoe"
bro.ai.game.DAMAGE_KIND.Magical;         // "physical" | "magical" | "true"
bro.ai.game.TACTIC.Retreat;              // "Hold"|"FocusLowestHp"|"Scatter"|"Retreat"
bro.ai.game.MOVE_DIR.NE;                 // integer move directions


// -----------------------------------------------------------------------------
// NN training: observation, action mask, reward tracker
// -----------------------------------------------------------------------------
//
// The pieces a learned policy needs at every tick: an ego-centric float
// observation, a validity mask over the discrete action heads, and a per-
// agent reward delta. All three are zero-allocation on the C++ side and
// hand back typed arrays / plain objects to JS.

/**
 * Build the ego-centric observation vector for `agent`. Layout (constants
 * exposed as `bro.ai.game.OBS_TOTAL`):
 *   self block    (14 floats: hp, mana, attack/ability cooldowns, speed,
 *                  sin/cos(aim - yaw))
 *   enemy block   (5 nearest enemies × 6 floats: valid, relX, relZ, dist,
 *                  hp%, inAttackRange)
 *   ally block    (4 nearest allies × 5 floats: valid, relX, relZ, dist, hp%)
 * Positions are in agent's local frame, normalized by OBS_RANGE (50 units).
 *
 * @param {AIAgent} agent
 * @param {AIWorld} world
 * @returns {Float32Array} length = bro.ai.game.OBS_TOTAL
 * @throws {TypeError} when either argument is not an agent / world
 */
const obs = bro.ai.game.buildObservation(focusAgent, world);
UI.drawObservation(obs);

/**
 * Build the legal-action mask for `agent`. Use to renormalize a policy
 * softmax over only currently-valid choices.
 *
 * Layout (`bro.ai.game.MASK_TOTAL` floats, 1.0 = legal, 0.0 = illegal):
 *   [0 .. N_ENEMY_SLOTS)   "attack enemy in slot k", slot k matches the
 *                          k-th enemy in the observation (nearest-first).
 *                          `enemyIds[k]` is the underlying Unit::id (or -1).
 *   [N_ENEMY_SLOTS ..)     "cast ability slot s", bound + cooldown ready
 *                          + mana sufficient (range not checked).
 *
 * @param {AIAgent} agent
 * @param {AIWorld} world
 * @returns {{ mask: Float32Array, enemyIds: Int32Array }}
 */
const am = bro.ai.game.buildActionMask(focusAgent, world);
const legalAttacks = am.enemyIds.filter((id, k) => id >= 0 && am.mask[k] > 0);

bro.ai.game.OBS_TOTAL;        // observation length
bro.ai.game.MASK_TOTAL;       // mask length
bro.ai.game.N_ENEMY_SLOTS;    // enemy slots in the observation/mask (5)

/**
 * Per-agent reward-delta accumulator. Captures the agent's baseline at
 * construction; each `consume()` call returns the delta since the previous
 * call and re-latches. Reads `world.events`. Do not call
 * `world.clearEvents()` in between consume() calls.
 *
 * @param {AIAgent} agent
 * @param {AIWorld} world
 * @returns {AIRewardTracker}
 */
const tracker = bro.ai.game.createRewardTracker(agent, world);

/** @returns {{damageDealt, damageTaken, kills, deaths, distanceTravelled}} */
const d = tracker.consume(agent, world);
const r = d.damageDealt - d.damageTaken + d.kills * 20 - d.deaths * 20;

/** Re-latch the baseline (e.g. start of a new episode). */
tracker.reset(agent, world);


// -----------------------------------------------------------------------------
// Headless training harness: bro.ai.game.createSimulation(world)
// -----------------------------------------------------------------------------
//
// Fixed-dt rollout driver for offline NN training and offscreen sims. Wraps
// brogameagent::Simulation. Per step(dt): registered policies fire, results
// are applied via World::applyAction, then World::tick advances scripted
// agents and projectiles. Agents WITHOUT a registered policy keep their
// scripted behaviour (lane walk, basic attack, etc.).
//
// The simulation does not own the world; the caller controls lifetime.

/**
 * @param {AIWorld} world
 * @returns {AISimulation}
 */
const sim = bro.ai.game.createSimulation(world);

/**
 * Register a policy for one agent. Called every step with the agent and a
 * world view; must return an AgentAction
 *   { moveX, moveZ, aimYaw, aimPitch, attackTargetId, abilitySlot, abilityTargetId }.
 *
 * @param {number} agentId  - Unit::id of the controlled agent
 * @param {function(agent, world): AgentAction} policy
 */
sim.addPolicy(heroAgent.unit.id, function (self, w) {
    const obs  = bro.ai.game.buildObservation(self, w);
    const mask = bro.ai.game.buildActionMask(self, w);
    return myPolicy.forward(obs, mask);   // your NN inference
});

/** Stop driving this agent. It falls back to scripted World::tick. */
sim.removePolicy(heroAgent.unit.id);

/** One fixed-dt step. */
sim.step(1 / 60);

/** Convenience: call step(dt) `n` times. Headless training inner loop. */
sim.runSteps(1 / 60, 600);          // 10 sim-seconds at 60Hz

sim.steps;                          // total steps taken (read-only)
sim.elapsed;                        // total sim seconds (read-only)
sim.resetCounters();                // does NOT reset world state


// -----------------------------------------------------------------------------
// Replay I/O: createRecorder() / createReplayReader()
// -----------------------------------------------------------------------------
//
// Streaming binary format (.bgar). Recorder writes per-frame agent state +
// damage events + a frame index appended on close() so any frame can be
// random-accessed. ReplayReader opens a finished file and exposes per-frame
// snapshots, per-agent trajectories, and a damage summary.
//
// For a schema-driven, non-combat replay (your own row/event layout) use
// bro.ai.game.grid.createGenericRecorder instead — docs/ai-game-tools.js.

/** @returns {AIRecorder} */
const rec = bro.ai.game.createRecorder();

/**
 * Open a file for writing. `dt` is recorded in the header for playback.
 * @param {string} path
 * @param {number} episodeId
 * @param {number} seed
 * @param {number} dt
 * @returns {boolean} false on I/O error
 */
rec.open(path, 1, Date.now(), 1 / 60);
rec.isOpen;                                       // true

/** Write the static roster (one entry per agent). Call once before frames.
 *  @param {AIWorld} world */
rec.writeRoster(world);

/** Capture one frame: agents, projectiles, and the slice of world.events
 *  that arrived since the last recordFrame. Do NOT call world.clearEvents()
 *  between recordFrame calls or that window's events are lost.
 *  @param {number} stepIdx @param {number} elapsed @param {AIWorld} world */
rec.recordFrame(state.steps, state.elapsed, world);

rec.frameCount;                                    // frames written so far
rec.close();                                       // appends index + footer

/** @returns {AIReplayReader} */
const rr = bro.ai.game.createReplayReader();
if (!rr.open(path)) UI.log("open failed: " + rr.errorMessage);
rr.frameCount;                                     // total frames

/** Random-access one frame.
 *  @returns {{ stepIdx, elapsed,
 *              agents: [{id, x, z, hp, mana, yaw, alive}, ...],
 *              events: [{attackerId, targetId, amount, killed}, ...] }} */
const f = rr.frame(0);

/** Full XZ trajectory for one agent across the replay.
 *  @returns {[{stepIdx, elapsed, x, z, hp, alive}, ...]} */
const traj = rr.trajectory(heroAgent.unit.id);

/** Aggregate damage / hits / kills per (attacker, target) pair.
 *  @returns {[{attackerId, targetId, totalDamage, hits, kills}, ...]} */
const dmg = rr.damageSummary();


// =============================================================================
// Belief / observability / Information-Set MCTS
// =============================================================================
//
// Planning when the enemy is not fully observed: a per-team particle cloud
// over hidden enemy state, plus the two searchers that determinize against
// it every iteration.

/** VisibilityConfig is a plain object:
 *   { fovRadians?: number, maxRange?: number, checkLos?: boolean } */

/**
 * Build a fresh TeamObservation against ground truth. Allies are fully
 * known; enemies are visible iff any living ally has LOS + FOV + range.
 * @param {AIWorld} world @param {number} teamId
 * @param {Object} visCfg @param {number} now - sim time
 * @returns {{teamId, timestamp, allies, enemies}}
 */
const teamObs = bro.ai.game.observe(world, teamId, visCfg, simTime);

/** Merge a fresh observation into a prior one, carrying stale enemies
 *  forward with lastSeenElapsed updated. */
const merged = bro.ai.game.mergeObservations(prior, fresh, now);

/** TeamBelief, per-team particle cloud over hidden enemy state. */
const tb = bro.ai.game.createTeamBelief({
    teamId: 0, numParticles: 32, navGrid: nav,
    motion: { maxSpeed: 6, accelStd: 4, spreadOnLoss: 3 },
    seed: 0xBE11Fn,
});
tb.registerEnemy(enemyId, maxHp, /*initialPos*/ { x: 0, z: 0 });
tb.propagate(world, visCfg, dt);      // diffuse particles forward one step
tb.update(teamObs);                   // reweight + resample against a sighting
const particles = tb.sample();        // { [enemyId]: {x,z,vx,vz,hp,heading,weight} }
const means = tb.mean();              // per-enemy weighted mean state
tb.ess;                               // effective sample size (read-only)
tb.enemies();                         // per-enemy {enemyId, visible, everSeen, ...}
tb.teamId; tb.numParticles;           // read-only
tb.clear();

/** InfoSetMcts, IS-MCTS for a single hero under partial observability: each
 *  iteration samples one determinization from the belief and searches it. */
const isMcts = bro.ai.game.createInfoSetMcts();
isMcts.setBelief(tb);
isMcts.setEvaluator("hpDelta");       // string preset, function, or object
isMcts.setPrior("attackBias");
isMcts.setConfig({ iterations: 500, rolloutHorizon: 32, simDt: 0.016 });
const act = isMcts.search(world, hero);
isMcts.advanceRoot(act);
isMcts.resetTree();
isMcts.lastStats;                     // {iterations, meanEss, ...}

/** InfoSetTeamMcts, the team analogue. It takes the belief and a config but
 *  no per-call evaluator/prior setters. */
const isTeam = bro.ai.game.createInfoSetTeamMcts();
isTeam.setBelief(tb);
isTeam.setConfig({ iterations: 400 });
const joint = isTeam.search(world, [hero1, hero2]);
isTeam.resetTree();
isTeam.lastStats;


// =============================================================================
// Snapshots, projectiles, VecSimulation
// =============================================================================

/** Snapshot / restore as opaque handles. Cheaper and lossless compared with
 *  world.snapshot()/restore(), which marshal through plain JS objects (see
 *  docs/ai-game-planning.js). Routes set by node.navigateTo are NOT captured:
 *  re-issue them after applying a snapshot. */
const asnap = bro.ai.game.captureAgentSnapshot(agent);
bro.ai.game.applyAgentSnapshot(agent, asnap);
asnap.id; asnap.x; asnap.z; asnap.yaw; asnap.hp; asnap.alive;   // read-only

const wsnap = bro.ai.game.captureWorldSnapshot(world);
bro.ai.game.applyWorldSnapshot(world, wsnap);
wsnap.agentCount; wsnap.projectileCount; wsnap.eventCount; wsnap.nextProjectileId;
const projs = wsnap.projectiles();     // [{id, x, z, mode, ...}, ...]

/** Patch a captured WorldSnapshot with a sampled particle map, the IS-MCTS
 *  determinization helper. `particles` is what tb.sample() returned. */
bro.ai.game.patchSnapshotWithParticles(wsnap, particles);

/**
 * Free projectile helpers. `kind` is "physical" | "magical" | "true";
 * `mode` is "single" | "pierce" | "aoe". Defaults: speed 20, radius 0.3,
 * remainingLife 2, damage 0, targetId -1, mode "single".
 * @returns {number} the new projectile id, or -1
 */
const pid = bro.ai.game.spawnProjectile(world, {
    ownerId: attackerId, teamId: 0, targetId: -1,
    x: 0, z: 0, vx: 20, vz: 0, speed: 20, radius: 0.3,
    damage: 25, kind: "physical",
    remainingLife: 2.0, mode: "single",
    splashRadius: 0, maxHits: 0,
});
const live = bro.ai.game.worldProjectiles(world);   // alive projectiles only

/** VecSimulation, N batched 1v1 envs for self-play training. Both spellings
 *  create the same thing. */
const vec = bro.ai.game.createVecSimulation({
    numEnvs: 64, arenaHalfSize: 12, dt: 0.016, maxStepsPerEpisode: 600,
    minSpawnDist: 4, maxSpawnDist: 10,
    hp: 100, maxMana: 100, manaRegenPerSec: 0,
    damage: 5, attackRange: 2.5, attacksPerSec: 1,
    moveSpeed: 6, maxAccel: 30, maxTurnRate: 8, radius: 0.4,
    rewardDamageDealt: 1.0, rewardDamageTakenMul: 1.0,
    rewardKill: 100, rewardDeath: -100, rewardStep: 0, rewardTimeout: 0,
});
vec.numEnvs;                              // read-only
vec.seedAndReset(0x1234n);
const heroObs = vec.observe(1);           // Float32Array, N * OBS_TOTAL
const heroMask = vec.actionMask(1);       // { mask, enemyIds }
vec.applyActions(1, heroActions);         // side 1 = hero, 2 = opponent
vec.applyActions(2, oppActions);          // arrays of N AgentAction objects
vec.step();
const done = vec.dones();                 // { done: Int32Array, winner: Int32Array }
const rew = vec.rewards();                // { hero: Float32Array, opponent: Float32Array }
vec.stepCounts(); vec.episodeCounts();
vec.resetDone();                          // reset only the finished envs
vec.resetEnv(0);


// =============================================================================
// GenericMcts, env-agnostic PUCT search
// =============================================================================
//
// The MCTS classes above are welded to the bundled World/Agent/CombatAction
// model, they only plan over brogameagent's combat sim. createGenericMcts is
// the escape hatch for anything else: a custom JS-side env, a 2D platformer,
// a board game, a planner test harness. You describe the env with five
// callbacks and search() runs the same AlphaZero-style PUCT algorithm over it.
//
// Algorithm:
//
//     score(a) = Q(s, a) + cPuct · P(s, a) · √N(s) / (1 + N(s, a))
//
// Q is the action's mean discounted return; P is the prior (uniform when no
// priorFn is set); N is visit count. Leaf evaluation uses valueFn when
// provided, otherwise a random rollout of rolloutDepth steps. Optional
// Dirichlet root noise matches the AlphaZero exploration scheme.

/**
 * @param {Object} opts
 * @param {Object} opts.env  - Environment description; required.
 * @param {number} opts.env.numActions     - Total action space size (max action index + 1).
 * @param {function():any}    opts.env.snapshot      - Capture current env state. Held opaquely
 *                                                     for the duration of one search() call.
 * @param {function(any)}     opts.env.restore       - Restore env from a snapshot.
 * @param {function(int):{reward:number,done:boolean}} opts.env.step - Apply action, mutate env.
 * @param {function():(Int32Array|number[])}          opts.env.legalActions - Legal action indices
 *                                                     in [0, numActions). Re-queried at every
 *                                                     expansion, so it must reflect current state.
 * @param {function():Float32Array}                   opts.env.observe - Observation vector forwarded
 *                                                     to priorFn / valueFn. Shape and semantics
 *                                                     are entirely up to you.
 * @param {number} [opts.iterations=100]   - Iterations per search() call.
 * @param {number} [opts.cPuct=1.5]        - PUCT exploration constant.
 * @param {number} [opts.gamma=0.99]       - Discount per decision step.
 * @param {number} [opts.rolloutDepth=8]   - Random-rollout depth (used only when valueFn unset).
 * @param {number} [opts.dirichletAlpha=0] - Dirichlet root noise concentration. 0 = noise off.
 * @param {number} [opts.dirichletEpsilon=0] - Mixing weight for Dirichlet noise on root prior.
 * @param {bigint|number} [opts.seed]      - Rollout / noise RNG seed.
 * @param {function(Float32Array obs, Int32Array legal):Float32Array} [opts.priorFn]
 *        - Optional learned-policy prior. Returns a probability per action over numActions.
 *          Only entries at `legal` indices are read.
 * @param {function(Float32Array obs):number} [opts.valueFn]
 *        - Optional learned-value function in [-1, 1]. When set, replaces random rollout
 *          for leaf evaluation.
 * @param {Object} [opts.backend]
 *        - A learn.createDirectBackend/createServerBackend wrapper: masked-softmax
 *          prior + value evaluated entirely in C++, no per-node JS round trip.
 *          An explicit priorFn/valueFn wins over it. See docs/ai-learn-api.js.
 * @returns {AIGenericMcts}
 * @throws {TypeError} when any of the five callbacks is missing, when
 *     numActions is not a positive integer, or when `backend` is not a
 *     DirectBackend/ServerBackend
 *
 * Two conveniences: the env may be passed INLINE (the options object itself
 * carries snapshot/restore/step/legalActions/observe/numActions, no `env`
 * key), and `legal` is accepted as an alias for `legalActions`.
 */
const m = bro.ai.game.createGenericMcts({
    env: {
        numActions: 6,
        snapshot()    { return mySim.snapshot(); },
        restore(s)    { mySim.restore(s); },
        step(action)  { return mySim.step(action); },     // {reward, done}
        legalActions(){ return mySim.legalActions(); },
        observe()     { return buildObs(mySim); },
    },
    iterations: 200,
    cPuct: 1.5,
    gamma: 0.99,
    rolloutDepth: 8,
    dirichletAlpha: 0.5,
    dirichletEpsilon: 0.25,
    seed: 0xC0DE1234n,
    priorFn(obs, legal) { return netForwardProbs(obs, legal); },
    valueFn(obs)        { return netForwardValue(obs); },
});

m.numActions;   // read-only, echoed from env.numActions

/**
 * Run a search starting from the env's current state. Returns the most-visited
 * root action, or -1 if the action space is empty. Snapshot/restore is used
 * internally; the env is left in its pre-search state on exit.
 * @returns {number}
 */
const genericAction = m.search();

/**
 * Normalized root visit distribution over [0, numActions). Sums to 1 over
 * visited actions. Useful as the policy target for ExIt-style training
 * (learn.createGenericReplayBuffer, docs/ai-learn-api.js).
 * @returns {Float32Array}
 */
const visits = m.rootVisits();

/**
 * Promote the subtree under `action` to the new root, reusing its statistics.
 * If `action` was never expanded, the tree is dropped and the next search()
 * rebuilds from scratch.
 */
m.advanceRoot(genericAction);

/** Drop the search tree. */
m.reset();

/**
 * Replace the search-time configuration. Same fields as the constructor opts
 * (iterations, cPuct, gamma, rolloutDepth, dirichletAlpha, dirichletEpsilon, seed).
 */
m.setConfig({ iterations: 400, dirichletAlpha: 0 });

/** Replace or clear the prior / value functions at any time. */
m.setPriorFn((obs, legal) => netForwardProbs(obs, legal));
m.setValueFn((obs) => netForwardValue(obs));
m.setPriorFn(null);   // pass null/undefined to fall back to uniform prior

/**
 * Stats from the most recent search() call. A METHOD here, unlike the
 * combat searchers' `lastStats` property.
 *   { iterations, treeSize, bestVisits, bestAction }
 * @returns {Object}
 */
const stats = m.lastStats();
