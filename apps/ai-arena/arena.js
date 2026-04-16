// arena.js — World construction from a Scenario. Builds the nav grid,
// the world, agents with their ability slots wired, and registers ability
// resolvers against the world. Ability behavior is data-driven: each entry
// in scenario.abilities has a `kind` (projectile | grenade | heal-ally)
// and a parameter block.
var Arena = {};
(function () {
    "use strict";

    // Stable ability IDs — mirrored from Scenarios for code that references
    // them by name (render, ai, loop).
    Arena.AB_HEAL     = 0;
    Arena.AB_FIREBALL = 1;
    Arena.AB_BEAM     = 2;
    Arena.AB_GRENADE  = 3;
    Arena.AB_BASIC    = 4;

    // Populated by Arena.build(scenario). Downstream modules read these
    // rather than the scenario object so swapping scenarios is a single
    // assignment point.
    Arena.BOUNDS = null;
    Arena.OBSTACLES = null;
    Arena.ROSTER = null;
    Arena.COLORS = null;
    Arena.scenario = null;

    function spawnStraight(caster, w, tgt, p) {
        var dx = tgt.x - caster.x, dz = tgt.z - caster.z;
        var d = Math.hypot(dx, dz) || 1;
        var out = {
            ownerId: caster.unit.id,
            teamId:  caster.unit.teamId,
            targetId: -1,
            x: caster.x, z: caster.z,
            vx: (dx / d) * p.speed,
            vz: (dz / d) * p.speed,
            speed: p.speed,
            radius: p.radius,
            damage: p.damage,
            remainingLife: p.life || 1.2,
            kind: p.kind,
            mode: p.mode,
        };
        if (p.maxHits) out.maxHits = p.maxHits;
        return out;
    }

    function registerProjectileAbility(world, ab) {
        var p = ab.projectile;
        world.registerAbility(ab.id, {
            cooldown: ab.cooldown, manaCost: ab.manaCost, range: ab.range,
            fn: function (caster, w, targetId) {
                var tgt = w.findById(targetId);
                if (!tgt) return;
                w.spawnProjectile(spawnStraight(caster, w, tgt, p));
            },
        });
    }

    function registerGrenadeAbility(world, ab) {
        var p = ab.projectile;
        world.registerAbility(ab.id, {
            cooldown: ab.cooldown, manaCost: ab.manaCost, range: ab.range,
            fn: function (caster, w, targetId) {
                var tgt = w.findById(targetId);
                if (!tgt) return;
                var dx = tgt.x - caster.x, dz = tgt.z - caster.z;
                var d = Math.hypot(dx, dz) || 1;
                var spawn = spawnStraight(caster, w, tgt, p);
                // Grenade life is tuned so it detonates on the target —
                // derived from travel distance, not a fixed lifetime.
                spawn.remainingLife = Math.min(2.0, d / p.speed + 0.05);
                spawn.splashRadius = p.splashRadius;
                w.spawnProjectile(spawn);
            },
        });
    }

    function registerHealAbility(world, ab) {
        var range2 = ab.range * ab.range;
        world.registerAbility(ab.id, {
            cooldown: ab.cooldown, manaCost: ab.manaCost, range: ab.range,
            fn: function (caster, w, targetId) {
                var tgt = caster;
                if (targetId !== caster.unit.id) {
                    var found = w.findById(targetId);
                    if (found && found.unit.alive &&
                        found.unit.teamId === caster.unit.teamId) {
                        var dx = found.x - caster.x;
                        var dz = found.z - caster.z;
                        if (dx * dx + dz * dz <= range2) tgt = found;
                    }
                }
                tgt.unit.hp = Math.min(tgt.unit.maxHp, tgt.unit.hp + ab.amount);
            },
        });
    }

    // Basic shot: uses the caster's BotAim forward (set each think tick by
    // ai.js). Cooldown comes from the ability spec; range gating is applied
    // by the caster's think() before the cast is chosen.
    function registerBasicShotAbility(world, ab) {
        var p = ab.projectile;
        world.registerAbility(ab.id, {
            cooldown: ab.cooldown, manaCost: ab.manaCost, range: ab.range,
            fn: function (caster, w /*, targetId*/) {
                var mem = AI.getMem(caster.unit.id);
                var f = BotAim.forward(mem.aim);
                var u = caster.unit;
                w.spawnProjectile({
                    ownerId: u.id,
                    teamId:  u.teamId,
                    x: caster.x + f.x * (u.radius + 0.4),
                    z: caster.z + f.z * (u.radius + 0.4),
                    vx: f.x * p.speed,
                    vz: f.z * p.speed,
                    speed:  p.speed,
                    radius: p.radius,
                    damage: p.damage,
                    remainingLife: p.life,
                    kind: p.kind,
                    mode: p.mode,
                });
            },
        });
    }

    var REGISTRARS = {
        "projectile": registerProjectileAbility,
        "grenade":    registerGrenadeAbility,
        "heal-ally":  registerHealAbility,
        "basic-shot": registerBasicShotAbility,
    };

    Arena.build = function (scenario) {
        Arena.scenario = scenario;
        Arena.BOUNDS = scenario.bounds;
        Arena.OBSTACLES = scenario.obstacles;
        Arena.ROSTER = scenario.roster;
        Arena.COLORS = scenario.colors;

        var B = scenario.bounds;
        var nav = bro.ai.game.createNavGrid({
            minX: B.minX, minZ: B.minZ, maxX: B.maxX, maxZ: B.maxZ,
            cellSize: scenario.navCell,
            obstacles: scenario.obstacles,
            padding: scenario.navPadding,
        });

        var world = bro.ai.game.createWorld();
        for (var i = 0; i < scenario.obstacles.length; i++) {
            world.addObstacle(scenario.obstacles[i]);
        }

        var ud = scenario.unitDefaults;
        var agentsById = {};
        var agentList = [];
        for (var j = 0; j < scenario.roster.length; j++) {
            var r = scenario.roster[j];
            var a = bro.ai.game.createAgent({
                navGrid: nav,
                x: r.x, z: r.z,
                speed: ud.speed,
                radius: ud.radius,
                id: r.id,
                teamId: r.teamId,
                hp: ud.hp,
                damage: ud.damage,
                attackRange: ud.attackRange,
            });
            var u = a.unit;
            u.maxMana = ud.maxMana;
            u.mana = ud.mana;
            u.manaRegenPerSec = ud.manaRegenPerSec;
            u.attacksPerSec = ud.attacksPerSec;
            u.armor = ud.armor;
            // Wire this unit's ability slots — without this every cast
            // silently fails (abilitySlot defaults to -1).
            for (var s = 0; s < scenario.abilities.length; s++) {
                var ab = scenario.abilities[s];
                u.setAbilitySlot(ab.slot, ab.id);
            }
            world.addAgent(a);
            agentList.push(a);
            agentsById[r.id] = a;
        }

        for (var k = 0; k < scenario.abilities.length; k++) {
            var abk = scenario.abilities[k];
            var reg = REGISTRARS[abk.kind];
            if (reg) reg(world, abk);
            else console.log("ai-arena: unknown ability kind: " + abk.kind);
        }

        return { nav: nav, world: world, agents: agentList, byId: agentsById };
    };
})();
