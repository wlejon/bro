// options/tactical_options.js — Initial Option design for ai-arena.
//
// Eight tactical primitives that compress the key decisions the scripted
// ai.js makes: engage / disengage / poke / AOE / heal / focus / kite /
// regroup. Each option encodes WHEN it applies (canInitiate), WHAT to do
// per decision window (step), and WHEN to stop (shouldTerminate) so that
// OptionMcts can plan at option granularity instead of raw CombatActions.
//
// Why this set, specifically:
//
// The scripted agent beats raw-MCTS because its decisions compose
// primitives that each take several ticks to pay off — a peek-and-shoot
// is ~15 ticks, a retreat-to-cover is ~30. CombatAction-level MCTS at
// a 16ms budget can't search deep enough to rediscover those primitives
// per-tick; it just looks at local movement deltas that don't matter.
// By lifting those primitives into named options, a 3-deep OptionMcts
// tree plans ~3×6 = 18 windows of game time while still fitting the
// frame budget. That's the depth the scripted policy is playing at.
//
// Usage:
//
//   const opts = TacticalOptions.build({
//       basicSlot: 4, healSlot: 0, fireballSlot: 1,
//       beamSlot: 2, grenadeSlot: 3,
//       lowHpFrac: 0.35, healMinMana: 25,
//   });
//   const mcts = bro.ai.game.createOptionMcts({
//       iterations: 120, optionMaxWindows: 6, useLeafValue: true,
//       options: opts, opponentPolicy: "scripted", evaluator: "hpDelta",
//   });
//   // In .think(): search returns an option NAME string.
//   const picked = mcts.search(world, hero);
//
// Or pass to a Commander role:
//
//   bro.ai.game.createCommander({
//       roles: [{ name: "lead", options: opts }, ...]
//   });

var TacticalOptions = (function () {
    "use strict";

    // MoveDir enum values (mcts.h). Using literals so this file is
    // self-contained and doesn't depend on constants exposed on bro.ai.game
    // at load time.
    var MOVE = {
        HOLD: 0, N: 1, NE: 2, E: 3, SE: 4, S: 5, SW: 6, W: 7, NW: 8,
        PATH_TO_TARGET: 9, PATH_AWAY: 10,
    };

    // --- View helpers ---------------------------------------------------
    //
    // Every option callback receives a selfView and worldView. These views
    // are plain-object snapshots built by the C++ bindings — cheap to
    // traverse but limited to the fields exposed by buildAgentFields
    // (id, teamId, x, z, yaw, hp, maxHp, alive, attackRange,
    // attackCooldown, mana, maxMana, abilities[]).

    function dist2(a, b) {
        var dx = a.x - b.x, dz = a.z - b.z;
        return dx * dx + dz * dz;
    }

    function livingEnemies(selfView, worldView) {
        var out = [];
        var agents = worldView.agents;
        for (var i = 0; i < agents.length; i++) {
            var a = agents[i];
            if (a && a.alive && a.teamId !== selfView.teamId) out.push(a);
        }
        return out;
    }

    function livingAllies(selfView, worldView) {
        var out = [];
        var agents = worldView.agents;
        for (var i = 0; i < agents.length; i++) {
            var a = agents[i];
            if (a && a.alive && a.teamId === selfView.teamId && a.id !== selfView.id) {
                out.push(a);
            }
        }
        return out;
    }

    function nearest(self, list) {
        var best = null, bestD = Infinity;
        for (var i = 0; i < list.length; i++) {
            var d = dist2(self, list[i]);
            if (d < bestD) { bestD = d; best = list[i]; }
        }
        return best;
    }

    function weakest(list) {
        var best = null, bestHp = Infinity;
        for (var i = 0; i < list.length; i++) {
            if (list[i].hp < bestHp) { bestHp = list[i].hp; best = list[i]; }
        }
        return best;
    }

    // Return 8-way compass MoveDir toward target. Local frame: N is -Z.
    function compassToward(self, target) {
        var dx = target.x - self.x, dz = target.z - self.z;
        var ang = Math.atan2(dx, -dz);                   // 0 = N, +π/2 = E
        var octant = Math.round(ang / (Math.PI / 4));    // -4..4
        if (octant < 0) octant += 8;
        // octant 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
        return MOVE.N + octant;
    }

    function abilityReady(selfView, slot) {
        var ab = selfView.abilities && selfView.abilities[slot];
        if (!ab || ab.abilityId < 0) return false;
        return ab.cooldown <= 0;
    }

    function hpFrac(a)   { return a.maxHp > 0 ? a.hp / a.maxHp : 0; }
    function manaOk(s, cost) { return (s.mana || 0) >= cost; }

    // Count enemies within radius r of a point.
    function enemiesNear(enemies, point, r) {
        var r2 = r * r, n = 0;
        for (var i = 0; i < enemies.length; i++) {
            if (dist2(enemies[i], point) <= r2) n++;
        }
        return n;
    }

    // Allocate target-slot index for the enemy with id `targetId`. The
    // C++ action_mask builds enemy slots sorted by proximity; we mirror
    // that ordering here so attackSlot references resolve correctly at
    // apply time. Returns -1 if the target isn't among the N nearest.
    var N_ENEMY_SLOTS = 5;
    function slotForEnemy(selfView, enemies, targetId) {
        var sorted = enemies.slice().sort(function (a, b) {
            return dist2(selfView, a) - dist2(selfView, b);
        });
        for (var i = 0; i < Math.min(sorted.length, N_ENEMY_SLOTS); i++) {
            if (sorted[i].id === targetId) return i;
        }
        return -1;
    }

    // --- Option builder -------------------------------------------------

    function build(cfg) {
        cfg = cfg || {};
        var C = {
            basicSlot:    cfg.basicSlot    !== undefined ? cfg.basicSlot    : 4,
            healSlot:     cfg.healSlot     !== undefined ? cfg.healSlot     : 0,
            fireballSlot: cfg.fireballSlot !== undefined ? cfg.fireballSlot : 1,
            beamSlot:     cfg.beamSlot     !== undefined ? cfg.beamSlot     : 2,
            grenadeSlot:  cfg.grenadeSlot  !== undefined ? cfg.grenadeSlot  : 3,
            lowHpFrac:    cfg.lowHpFrac    !== undefined ? cfg.lowHpFrac    : 0.35,
            healMinMana:  cfg.healMinMana  !== undefined ? cfg.healMinMana  : 25,
            fireballMana: cfg.fireballMana !== undefined ? cfg.fireballMana : 20,
            grenadeMana:  cfg.grenadeMana  !== undefined ? cfg.grenadeMana  : 35,
            fireballMinRange: cfg.fireballMinRange !== undefined ? cfg.fireballMinRange : 4,
            fireballMaxRange: cfg.fireballMaxRange !== undefined ? cfg.fireballMaxRange : 13,
            clusterRadius:    cfg.clusterRadius    !== undefined ? cfg.clusterRadius    : 2.5,
            clusterMinEnemies:cfg.clusterMinEnemies!== undefined ? cfg.clusterMinEnemies: 2,
        };

        // specs[name] keeps JS-callable copies of each option's predicates
        // so the live agent can call step() directly (to translate to
        // self.moveTo/self.cast) without going back through the C++
        // OptionMcts simulation path.
        var specs = {};
        function def(name, spec) {
            specs[name] = spec;
            return bro.ai.game.createOption({
                name: name,
                canInitiate: spec.canInitiate,
                step: spec.step,
                shouldTerminate: spec.shouldTerminate,
            });
        }

        // 1) holdAndFire ─────────────────────────────────────────────────
        // Stand still and attack the nearest visible enemy. The anchor
        // option: every tactical plan collapses into "hold + shoot" at
        // some point. Short duration (~3 windows) so it re-evaluates
        // often and exits if the enemy moves out of range.
        var holdAndFire = def("holdAndFire", {
            canInitiate: function (self, world) {
                var enemies = livingEnemies(self, world);
                var e = nearest(self, enemies);
                if (!e) return false;
                return dist2(self, e) <= self.attackRange * self.attackRange;
            },
            step: function (self, world) {
                var enemies = livingEnemies(self, world);
                var e = nearest(self, enemies);
                var slot = e ? slotForEnemy(self, enemies, e.id) : -1;
                return { moveDir: MOVE.HOLD,
                         attackSlot: -1,
                         abilitySlot: abilityReady(self, C.basicSlot) ? C.basicSlot : -1 };
            },
            shouldTerminate: function (self, world, ticks) {
                if (ticks >= 3) return true;
                var e = nearest(self, livingEnemies(self, world));
                if (!e) return true;
                return dist2(self, e) > self.attackRange * self.attackRange * 1.2;
            },
        });

        // 2) advanceToRange ──────────────────────────────────────────────
        // Path toward the nearest enemy until attack range is reached.
        // Uses PathToTarget so obstacles are avoided via the nav grid.
        // Terminates on range reached OR a clearer engagement (low HP,
        // lost all enemies) so the next option can take over.
        var advanceToRange = def("advanceToRange", {
            canInitiate: function (self, world) {
                var e = nearest(self, livingEnemies(self, world));
                if (!e) return false;
                return dist2(self, e) > self.attackRange * self.attackRange;
            },
            step: function (self, world) {
                var enemies = livingEnemies(self, world);
                var e = nearest(self, enemies);
                var slot = e ? slotForEnemy(self, enemies, e.id) : 0;
                return { moveDir: MOVE.PATH_TO_TARGET, attackSlot: -1, abilitySlot: -1 };
            },
            shouldTerminate: function (self, world, ticks) {
                if (ticks >= 5) return true;
                if (hpFrac(self) < C.lowHpFrac) return true;
                var e = nearest(self, livingEnemies(self, world));
                if (!e) return true;
                return dist2(self, e) <= self.attackRange * self.attackRange;
            },
        });

        // 3) retreatBack ─────────────────────────────────────────────────
        // Path away from the nearest enemy. Runs until we're well out of
        // their attack range OR we've regained HP. The heal decision is
        // separate (selfHeal) — retreat just buys distance.
        var retreatBack = def("retreatBack", {
            canInitiate: function (self, world) {
                if (!livingEnemies(self, world).length) return false;
                if (hpFrac(self) < C.lowHpFrac) return true;
                var enemies = livingEnemies(self, world);
                var e = nearest(self, enemies);
                // Outnumbered-local check: retreat if 2+ enemies within 1.5× range.
                var r = self.attackRange * 1.5;
                return e && enemiesNear(enemies, self, r) >= 2;
            },
            step: function () {
                return { moveDir: MOVE.PATH_AWAY, attackSlot: -1, abilitySlot: -1 };
            },
            shouldTerminate: function (self, world, ticks) {
                if (ticks >= 5) return true;
                var e = nearest(self, livingEnemies(self, world));
                if (!e) return true;
                var safe = self.attackRange * 1.8;
                return dist2(self, e) >= safe * safe;
            },
        });

        // 4) focusWeakest ────────────────────────────────────────────────
        // Commit to finishing the lowest-HP enemy: move toward them
        // (compass, so movement doesn't re-target the nav grid's "path
        // to slot 0"), fire when in range. Highest-value option when an
        // enemy is near death — scripted AI encodes this as FIRING_BAND
        // with target-latch via threatWeighted pick.
        var focusWeakest = def("focusWeakest", {
            canInitiate: function (self, world) {
                var enemies = livingEnemies(self, world);
                var w = weakest(enemies);
                // Only commit to focus when the target is genuinely weaker
                // than average — prevents thrashing between similarly-hurt
                // enemies.
                if (!w || enemies.length < 1) return false;
                var avg = 0;
                for (var i = 0; i < enemies.length; i++) avg += enemies[i].hp;
                avg /= enemies.length;
                return w.hp < avg * 0.7;
            },
            step: function (self, world) {
                var enemies = livingEnemies(self, world);
                var w = weakest(enemies);
                if (!w) return { moveDir: MOVE.HOLD, attackSlot: -1, abilitySlot: -1 };
                var slot = slotForEnemy(self, enemies, w.id);
                var inRange = dist2(self, w) <= self.attackRange * self.attackRange;
                return {
                    moveDir: inRange ? MOVE.HOLD : compassToward(self, w),
                    attackSlot: -1,
                    abilitySlot: (inRange && abilityReady(self, C.basicSlot)) ? C.basicSlot : -1,
                };
            },
            shouldTerminate: function (self, world, ticks) {
                if (ticks >= 4) return true;
                var w = weakest(livingEnemies(self, world));
                return !w;
            },
        });

        // 5) strafeFire ──────────────────────────────────────────────────
        // Perpendicular kiting while in attack range: alternate E/W every
        // 2 ticks to make us a harder target. Matches scripted FIRE state
        // strafe behaviour (flip_every = 0.8s ≈ 2 windows). The target is
        // the nearest enemy; attackSlot tracks proximity order.
        var strafeFire = def("strafeFire", {
            canInitiate: function (self, world) {
                var e = nearest(self, livingEnemies(self, world));
                if (!e) return false;
                var d2 = dist2(self, e);
                // Only engage when in kite band: not point-blank, not out of range.
                var r = self.attackRange;
                return d2 <= r * r && d2 >= r * r * 0.3;
            },
            step: function (self, world, ticks) {
                var enemies = livingEnemies(self, world);
                var e = nearest(self, enemies);
                var slot = e ? slotForEnemy(self, enemies, e.id) : 0;
                // Strafe E for ticks 0,1, then W for 2,3, etc.
                var dir = (Math.floor(ticks / 2) % 2 === 0) ? MOVE.E : MOVE.W;
                return {
                    moveDir: dir, attackSlot: -1,
                    abilitySlot: abilityReady(self, C.basicSlot) ? C.basicSlot : -1,
                };
            },
            shouldTerminate: function (self, world, ticks) {
                if (ticks >= 4) return true;
                if (hpFrac(self) < C.lowHpFrac) return true;
                var e = nearest(self, livingEnemies(self, world));
                if (!e) return true;
                var d2 = dist2(self, e);
                var r = self.attackRange;
                return d2 > r * r || d2 < r * r * 0.2;
            },
        });

        // 6) selfHeal ────────────────────────────────────────────────────
        // Cast heal on self while stepping away from threats. Gated by
        // HP fraction + mana + cooldown. Terminates as soon as the heal
        // fires OR we've topped up — never holds the window to full
        // charge, that's what retreatBack is for.
        var selfHeal = def("selfHeal", {
            canInitiate: function (self) {
                return hpFrac(self) < C.lowHpFrac
                    && manaOk(self, C.healMinMana)
                    && abilityReady(self, C.healSlot);
            },
            step: function (self, world, ticks) {
                var enemies = livingEnemies(self, world);
                var nearestEnemy = nearest(self, enemies);
                var threatClose = nearestEnemy
                    && dist2(self, nearestEnemy) < self.attackRange * self.attackRange * 1.2;
                return {
                    // First tick: fire the heal (cast at self → attackSlot=-1).
                    moveDir: threatClose ? MOVE.PATH_AWAY : MOVE.HOLD,
                    attackSlot: -1,
                    abilitySlot: ticks === 0 && abilityReady(self, C.healSlot)
                        ? C.healSlot : -1,
                };
            },
            shouldTerminate: function (self, world, ticks) {
                // After cast or HP recovered, done. Safety cap at 2 windows.
                if (ticks >= 2) return true;
                return hpFrac(self) > 0.7;
            },
        });

        // 7) pokeFireball ────────────────────────────────────────────────
        // Stand off at fireball range and chip. Requires HP high (we're
        // not under pressure) and a target in the poke band. Single-cast
        // option — fires once and terminates.
        var pokeFireball = def("pokeFireball", {
            canInitiate: function (self, world) {
                if (hpFrac(self) < 0.6) return false;
                if (!manaOk(self, C.fireballMana)) return false;
                if (!abilityReady(self, C.fireballSlot)) return false;
                var e = nearest(self, livingEnemies(self, world));
                if (!e) return false;
                var d = Math.sqrt(dist2(self, e));
                return d >= C.fireballMinRange && d <= C.fireballMaxRange;
            },
            step: function (self, world, ticks) {
                var enemies = livingEnemies(self, world);
                var e = nearest(self, enemies);
                var slot = e ? slotForEnemy(self, enemies, e.id) : 0;
                return {
                    moveDir: MOVE.HOLD, attackSlot: -1,
                    abilitySlot: ticks === 0 && abilityReady(self, C.fireballSlot)
                        ? C.fireballSlot : -1,
                };
            },
            shouldTerminate: function (_self, _world, ticks) { return ticks >= 1; },
        });

        // 8) grenadeCluster ──────────────────────────────────────────────
        // When ≥2 enemies are close to each other AND a target enemy is in
        // range, throw the grenade. Mirrors scripted GRENADE intent: only
        // worth the mana when it can hit multiple units.
        var grenadeCluster = def("grenadeCluster", {
            canInitiate: function (self, world) {
                if (!manaOk(self, C.grenadeMana)) return false;
                if (!abilityReady(self, C.grenadeSlot)) return false;
                var enemies = livingEnemies(self, world);
                for (var i = 0; i < enemies.length; i++) {
                    var e = enemies[i];
                    if (dist2(self, e) > 144) continue;     // grenade range ≈ 12
                    if (enemiesNear(enemies, e, C.clusterRadius) >= C.clusterMinEnemies) {
                        return true;
                    }
                }
                return false;
            },
            step: function (self, world, ticks) {
                var enemies = livingEnemies(self, world);
                // Pick the densest cluster target.
                var best = null, bestN = 0;
                for (var i = 0; i < enemies.length; i++) {
                    var n = enemiesNear(enemies, enemies[i], C.clusterRadius);
                    if (n > bestN) { bestN = n; best = enemies[i]; }
                }
                if (!best) return { moveDir: MOVE.HOLD, attackSlot: -1, abilitySlot: -1 };
                var slot = slotForEnemy(self, enemies, best.id);
                return {
                    moveDir: MOVE.HOLD, attackSlot: -1,
                    abilitySlot: ticks === 0 && abilityReady(self, C.grenadeSlot)
                        ? C.grenadeSlot : -1,
                };
            },
            shouldTerminate: function (_self, _world, ticks) { return ticks >= 1; },
        });

        var handles = [
            holdAndFire, advanceToRange, retreatBack, focusWeakest,
            strafeFire, selfHeal, pokeFireball, grenadeCluster,
        ];
        // Ordered name list mirrors handles; caller needs both to iterate
        // deterministically and to look up step fns by name.
        var order = [
            "holdAndFire", "advanceToRange", "retreatBack", "focusWeakest",
            "strafeFire", "selfHeal", "pokeFireball", "grenadeCluster",
        ];
        return { handles: handles, specs: specs, order: order, config: C };
    }

    return { build: build, MOVE: MOVE };
})();
