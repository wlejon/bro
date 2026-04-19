// agents/options_shared.js — Shared helpers for options-based agents.
//
// Two surfaces here:
//
//   1. View builders (viewAgent, viewWorld). Used when a planner wants to
//      hand plain-object snapshots to JS callbacks from the C++ MCTS/Option
//      sim path — mirrors the shape of the bindings-built views so options
//      authored in tactical_options.js see the same data whether they run
//      inside the sim or inside a live think tick.
//
//   2. robotCommandFor(optionName, agent) — translates a chosen option name
//      into a Bot command. The live planners (options_mcts, options_commander)
//      drive execution through the shared reflex robot rather than queuing
//      raw CombatActions. Firing is then automatic (the robot's side-effect
//      projectile spawn runs every tick regardless of option), fixing the
//      15× basic-fire deficit that options had vs scripted.

var OptionsShared = (function () {
    "use strict";

    function viewAgent(a) {
        if (!a) return null;
        var u = a.unit;
        var abs = [];
        for (var i = 0; i < 8; i++) {
            abs.push({
                abilityId: (typeof u.getAbilitySlot === "function") ? u.getAbilitySlot(i) : -1,
                cooldown:  (typeof u.getAbilityCooldown === "function") ? u.getAbilityCooldown(i) : 0,
            });
        }
        return {
            id: u.id, teamId: u.teamId,
            x: a.x, z: a.z, yaw: a.yaw,
            hp: u.hp, maxHp: u.maxHp, alive: u.alive,
            attackRange: u.attackRange,
            attackCooldown: u.attackCooldown,
            mana: u.mana, maxMana: u.maxMana,
            abilities: abs,
        };
    }

    function viewWorld() {
        var arr = [];
        var teams = AI.shared.teams || [[], []];
        for (var t = 0; t < teams.length; t++) {
            var team = teams[t];
            for (var i = 0; i < team.length; i++) {
                var v = viewAgent(team[i]);
                if (v) arr.push(v);
            }
        }
        return { agents: arr };
    }

    // ── Option → Robot command ──────────────────────────────────────────
    //
    // Ability slot assumptions (match Scenarios.AB_*):
    //   0 = heal (self-target)
    //   1 = fireball (projectile)
    //   2 = beam (pierce)
    //   3 = grenade (aoe)
    //   4 = basic shot (projectile, fires along BotAim each tick)
    //
    // Robot fires basic shots as a side-effect whenever aim+LOS+cd align,
    // so option commands never set `fireBasic: false` — even "heal" keeps
    // putting out damage when the opportunity arises. Ability slots are
    // autocast by the robot when `allowAbilities[slot]` is present + gates
    // pass.
    //
    // Movement `kiteBand` tuned to scripted's ratios: retreatBack uses the
    // engage/support mul pair so heroes don't loiter in range; advance
    // closes to the scripted "engage" band (0.85× range); strafe stays
    // within (0.45, 0.85).

    var COMMAND_BUILDERS = {
        holdAndFire: function () {
            return {
                target: { policy: "focus", requireLOS: true },
                fireBasic: true,
                move: { mode: "hold" },
            };
        },

        advanceToRange: function () {
            return {
                target: { policy: "focus", requireLOS: false },
                fireBasic: true,                 // keep firing while advancing
                move: { mode: "advance", kiteBand: [0.45, 0.85], space: true },
            };
        },

        retreatBack: function () {
            return {
                target: { policy: "nearest", requireLOS: false },
                fireBasic: true,                 // over-the-shoulder shots
                allowAbilities: {
                    0: { target: "self", minMana: 25, maxHpFrac: 0.55 },
                },
                move: { mode: "coverFrom", space: true },
            };
        },

        focusWeakest: function () {
            return {
                target: { policy: "weakest", requireLOS: true },
                fireBasic: true,
                move: { mode: "strafe", kiteBand: [0.4, 0.85], space: true },
            };
        },

        strafeFire: function () {
            return {
                target: { policy: "nearest", requireLOS: true },
                fireBasic: true,
                move: { mode: "strafe", kiteBand: [0.45, 0.85], space: true },
            };
        },

        selfHeal: function () {
            return {
                target: { policy: "nearest", requireLOS: false },
                fireBasic: true,
                allowAbilities: {
                    0: { target: "self", minMana: 25 },
                },
                move: { mode: "coverFrom", space: true },
            };
        },

        pokeFireball: function () {
            return {
                target: { policy: "focus", requireLOS: true },
                fireBasic: true,
                allowAbilities: {
                    1: { target: "focus", minMana: 20 },
                },
                move: { mode: "hold" },
            };
        },

        grenadeCluster: function () {
            return {
                target: { policy: "focus", requireLOS: true },
                fireBasic: true,
                allowAbilities: {
                    3: { target: "cluster", minMana: 35,
                         clusterRadius: 2.5, minCluster: 2 },
                },
                move: { mode: "hold" },
            };
        },
    };

    function robotCommandFor(optionName) {
        var builder = COMMAND_BUILDERS[optionName];
        if (!builder) return defaultCommand();
        return builder();
    }

    // Fallback when no option matches (e.g. search returns null because no
    // option can_initiate). Matches scripted's default engage behavior:
    // pick nearest with LOS, fire, strafe in range, advance if out.
    function defaultCommand() {
        return {
            target: { policy: "nearest", requireLOS: true },
            fireBasic: true,
            move: { mode: "advance", kiteBand: [0.45, 0.85], space: true },
        };
    }

    return {
        viewAgent: viewAgent,
        viewWorld: viewWorld,
        robotCommandFor: robotCommandFor,
        defaultCommand: defaultCommand,
    };
})();
