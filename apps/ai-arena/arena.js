// arena.js — World construction: nav grid, obstacles, agents, abilities.
var Arena = {};
(function () {
    "use strict";

    Arena.BOUNDS = { minX: -20, minZ: -20, maxX: 20, maxZ: 20 };
    Arena.OBSTACLES = [
        { x: -8, z: -8, hw: 1.5, hd: 1.5 },
        { x:  8, z:  8, hw: 1.5, hd: 1.5 },
        { x: -8, z:  8, hw: 1.0, hd: 3.0 },
        { x:  8, z: -8, hw: 3.0, hd: 1.0 },
        { x:  0, z:  0, hw: 1.0, hd: 1.0 },
        { x:-14, z:  0, hw: 0.5, hd: 2.5 },
        { x: 14, z:  0, hw: 0.5, hd: 2.5 },
    ];

    // Ability slot IDs
    Arena.AB_HEAL     = 0;
    Arena.AB_FIREBALL = 1;
    Arena.AB_BEAM     = 2;
    Arena.AB_GRENADE  = 3;

    // 8v8 — Red on the left column, Blue on the right, spread along z.
    var REDNAMES  = ["Alpha", "Bravo", "Charlie", "Delta",
                     "Echo",  "Foxtrot", "Golf",   "Hotel"];
    var BLUENAMES = ["India", "Juliet", "Kilo",   "Lima",
                     "Mike",  "November", "Oscar", "Papa"];

    Arena.ROSTER = (function () {
        var list = [];
        var N = 8;
        var zStart = -14, zStep = 4;   // 8 rows over -14..+14
        for (var i = 0; i < N; i++) {
            list.push({
                id: i + 1,
                name: REDNAMES[i],
                teamId: 0,
                x: -17,
                z: zStart + i * zStep,
            });
        }
        for (var j = 0; j < N; j++) {
            list.push({
                id: N + j + 1,
                name: BLUENAMES[j],
                teamId: 1,
                x: 17,
                z: zStart + j * zStep,
            });
        }
        return list;
    })();

    Arena.COLORS = { 0: "#e74c3c", 1: "#3498db" };

    // Build nav grid, world, and populate with agents + obstacles + abilities.
    // Returns { nav, world, agents (JS array by id), rosterById }.
    Arena.build = function () {
        var B = Arena.BOUNDS;
        var nav = bro.ai.game.createNavGrid({
            minX: B.minX, minZ: B.minZ, maxX: B.maxX, maxZ: B.maxZ,
            cellSize: 0.5,
            obstacles: Arena.OBSTACLES,
            padding: 0.55,
        });

        var world = bro.ai.game.createWorld();
        for (var i = 0; i < Arena.OBSTACLES.length; i++) {
            world.addObstacle(Arena.OBSTACLES[i]);
        }

        var agentsById = {};
        var agentList = [];
        for (var j = 0; j < Arena.ROSTER.length; j++) {
            var r = Arena.ROSTER[j];
            var a = bro.ai.game.createAgent({
                navGrid: nav,
                x: r.x, z: r.z,
                speed: 5.2,
                radius: 0.4,
                id: r.id,
                teamId: r.teamId,
                hp: 100,
                damage: 12,
                attackRange: 9,   // shoot range — LOS-gated ranged weapon
            });
            var u = a.unit;
            u.maxMana = 100;
            u.mana = 60;
            u.manaRegenPerSec = 8;
            u.attacksPerSec = 1.4;
            u.armor = 4;
            world.addAgent(a);
            agentList.push(a);
            agentsById[r.id] = a;
        }

        // ── Abilities ───────────────────────────────────────────────────
        world.registerAbility(Arena.AB_HEAL, {
            cooldown: 6, manaCost: 25, range: 0,
            fn: function (caster /*, w, targetId */) {
                var u = caster.unit;
                u.hp = Math.min(u.maxHp, u.hp + 35);
            },
        });

        world.registerAbility(Arena.AB_FIREBALL, {
            cooldown: 1.5, manaCost: 20, range: 14,
            fn: function (caster, w, targetId) {
                var tgt = w.findById(targetId);
                if (!tgt) return;
                var dx = tgt.x - caster.x, dz = tgt.z - caster.z;
                var d = Math.hypot(dx, dz) || 1;
                w.spawnProjectile({
                    ownerId: caster.unit.id,
                    teamId:  caster.unit.teamId,
                    targetId: -1,
                    x: caster.x, z: caster.z,
                    vx: (dx / d) * 14, vz: (dz / d) * 14,
                    speed: 14,
                    radius: 0.35,
                    damage: 22,
                    remainingLife: 1.6,
                    kind: "magical",
                    mode: "single",
                });
            },
        });

        world.registerAbility(Arena.AB_BEAM, {
            cooldown: 3.5, manaCost: 30, range: 16,
            fn: function (caster, w, targetId) {
                var tgt = w.findById(targetId);
                if (!tgt) return;
                var dx = tgt.x - caster.x, dz = tgt.z - caster.z;
                var d = Math.hypot(dx, dz) || 1;
                w.spawnProjectile({
                    ownerId: caster.unit.id,
                    teamId:  caster.unit.teamId,
                    targetId: -1,
                    x: caster.x, z: caster.z,
                    vx: (dx / d) * 22, vz: (dz / d) * 22,
                    speed: 22,
                    radius: 0.25,
                    damage: 16,
                    remainingLife: 1.0,
                    maxHits: 3,
                    kind: "magical",
                    mode: "pierce",
                });
            },
        });

        world.registerAbility(Arena.AB_GRENADE, {
            cooldown: 5, manaCost: 35, range: 12,
            fn: function (caster, w, targetId) {
                var tgt = w.findById(targetId);
                if (!tgt) return;
                var dx = tgt.x - caster.x, dz = tgt.z - caster.z;
                var d = Math.hypot(dx, dz) || 1;
                w.spawnProjectile({
                    ownerId: caster.unit.id,
                    teamId:  caster.unit.teamId,
                    targetId: -1,
                    x: caster.x, z: caster.z,
                    vx: (dx / d) * 10, vz: (dz / d) * 10,
                    speed: 10,
                    radius: 0.5,
                    damage: 28,
                    remainingLife: Math.min(2.0, d / 10 + 0.05),
                    splashRadius: 2.5,
                    kind: "magical",
                    mode: "aoe",
                });
            },
        });

        return { nav: nav, world: world, agents: agentList, byId: agentsById };
    };
})();
