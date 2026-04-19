// agents/options_shared.js — Shared helpers for options-based agents.
//
// Live agents run inside the ai-arena think() loop with access to the
// bound C++ Agent/World objects. Tactical options, by contrast, are
// evaluated by the C++ OptionMcts simulation and receive plain-object
// views built by the bindings. To run the same options live (translate
// the chosen option's step() into self.moveTo / self.cast), we need to
// build equivalent views in JS from the bound objects, and we need a
// translator that maps CombatAction back to self.* calls.
//
// Both files below — options_mcts.js and options_commander.js — use
// these helpers. Keep this file free of Agents.register so load order
// doesn't matter.

var OptionsShared = (function () {
    "use strict";

    // Build a plain-object view of one bound Agent, matching the shape
    // the C++ bindings expose to option callbacks (buildAgentFields in
    // ai_bindings.cpp). Unit accessors come from the UnitData class
    // registered by the bindings (abilitySlot/cooldown via getter methods).
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

    // Build a world view from AI.shared.teams (populated each frame by
    // the scripted AI's think-prelude). Using AI.shared means we don't
    // need a bound-World iterator — both teams' Agents are reachable.
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

    // MoveDir values — must match mcts.h MoveDir enum.
    var MOVE = TacticalOptions.MOVE;

    // Translate a CombatAction emitted by an option's step() into
    // self.moveTo / self.cast / self.hold calls. attackSlot references
    // enemy-slot-by-proximity ordering (same ordering slotForEnemy used).
    function applyCombatAction(self, world, action) {
        var a = self.agent;
        var u = a.unit;

        // Compute ordered living enemies (matches the view's slot
        // allocation). Used both for resolving attackSlot targets and
        // for directional moves that depend on the target's position.
        var myTeam = u.teamId;
        var livingEnemies = [];
        var teams = AI.shared.teams || [[], []];
        var enemyTeam = teams[1 - myTeam] || [];
        for (var i = 0; i < enemyTeam.length; i++) {
            var e = enemyTeam[i];
            if (e && e.unit && e.unit.alive) livingEnemies.push(e);
        }
        livingEnemies.sort(function (e1, e2) {
            var d1 = (e1.x - a.x) * (e1.x - a.x) + (e1.z - a.z) * (e1.z - a.z);
            var d2 = (e2.x - a.x) * (e2.x - a.x) + (e2.z - a.z) * (e2.z - a.z);
            return d1 - d2;
        });
        var target = (action.attackSlot >= 0 && livingEnemies[action.attackSlot]) || null;

        // Ability casts first so in-flight projectiles get queued before
        // movement commits this tick's destination.
        if (action.abilitySlot >= 0 && typeof self.cast === "function") {
            // Convention: slot 0 is heal (self-target); others are enemy-targeted.
            // Mirrors scenarios.js AB_* layout — options default to the same map.
            var healSlot = 0;
            if (action.abilitySlot === healSlot) {
                self.cast(action.abilitySlot, u.id);
            } else if (target) {
                self.cast(action.abilitySlot, target.unit.id);
            }
            // If no valid target for a non-heal ability, skip the cast —
            // don't waste the cooldown.
        }

        // Movement.
        var rStep = u.attackRange || 8;
        switch (action.moveDir) {
            case MOVE.HOLD:
                if (typeof self.hold === "function") self.hold(0.2);
                break;
            case MOVE.N:  self.moveTo(a.x,              a.z - rStep);         break;
            case MOVE.NE: self.moveTo(a.x + rStep*0.7,  a.z - rStep*0.7);     break;
            case MOVE.E:  self.moveTo(a.x + rStep,      a.z);                 break;
            case MOVE.SE: self.moveTo(a.x + rStep*0.7,  a.z + rStep*0.7);     break;
            case MOVE.S:  self.moveTo(a.x,              a.z + rStep);         break;
            case MOVE.SW: self.moveTo(a.x - rStep*0.7,  a.z + rStep*0.7);     break;
            case MOVE.W:  self.moveTo(a.x - rStep,      a.z);                 break;
            case MOVE.NW: self.moveTo(a.x - rStep*0.7,  a.z - rStep*0.7);     break;
            case MOVE.PATH_TO_TARGET:
                if (target) self.moveTo(target.x, target.z);
                else if (typeof self.hold === "function") self.hold(0.1);
                break;
            case MOVE.PATH_AWAY: {
                var threat = livingEnemies[0];
                if (threat) {
                    var dx = a.x - threat.x, dz = a.z - threat.z;
                    var m = Math.sqrt(dx * dx + dz * dz) || 1;
                    self.moveTo(a.x + dx / m * rStep, a.z + dz / m * rStep);
                } else if (typeof self.hold === "function") self.hold(0.1);
                break;
            }
            default:
                if (typeof self.hold === "function") self.hold(0.1);
        }
    }

    return {
        viewAgent: viewAgent,
        viewWorld: viewWorld,
        applyCombatAction: applyCombatAction,
    };
})();
