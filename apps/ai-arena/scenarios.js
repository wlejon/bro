// scenarios.js — match presets (map + roster + ability tuning).
// Each scenario is a pure data object; Arena.build(scenario) consumes it.
// Add a new scenario → push it onto Scenarios.ALL. No engine changes needed.
var Scenarios = {};
(function () {
    "use strict";

    // Ability IDs (stable across scenarios). AB_BASIC is the regular shot
    // driven through the capability layer — registered like any other ability
    // so its cooldown/projectile flows through world.registerAbility's fn.
    Scenarios.AB_HEAL     = 0;
    Scenarios.AB_FIREBALL = 1;
    Scenarios.AB_BEAM     = 2;
    Scenarios.AB_GRENADE  = 3;
    Scenarios.AB_BASIC    = 4;

    function rosterLine(names, teamId, x, zStart, zStep, idOffset) {
        var list = [];
        for (var i = 0; i < names.length; i++) {
            list.push({
                id: idOffset + i + 1,
                name: names[i],
                teamId: teamId,
                x: x,
                z: zStart + i * zStep,
            });
        }
        return list;
    }

    var RED8  = ["Alpha", "Bravo", "Charlie", "Delta",
                 "Echo",  "Foxtrot", "Golf",   "Hotel"];
    var BLUE8 = ["India", "Juliet", "Kilo",   "Lima",
                 "Mike",  "November", "Oscar", "Papa"];

    // Default ability loadout — referenced by multiple scenarios.
    var DEFAULT_ABILITIES = [
        {
            id: Scenarios.AB_HEAL, slot: Scenarios.AB_HEAL,
            cooldown: 6, manaCost: 25, range: 4,
            kind: "heal-ally",
            amount: 35,
        },
        {
            id: Scenarios.AB_FIREBALL, slot: Scenarios.AB_FIREBALL,
            cooldown: 1.5, manaCost: 20, range: 14,
            kind: "projectile",
            projectile: {
                speed: 14, radius: 0.35, damage: 22, life: 1.6,
                kind: "magical", mode: "single",
            },
        },
        {
            id: Scenarios.AB_BEAM, slot: Scenarios.AB_BEAM,
            cooldown: 3.5, manaCost: 30, range: 16,
            kind: "projectile",
            projectile: {
                speed: 22, radius: 0.25, damage: 16, life: 1.0,
                kind: "magical", mode: "pierce", maxHits: 3,
            },
        },
        {
            id: Scenarios.AB_GRENADE, slot: Scenarios.AB_GRENADE,
            cooldown: 5, manaCost: 35, range: 12,
            kind: "grenade",
            projectile: {
                speed: 10, radius: 0.5, damage: 28, splashRadius: 2.5,
                kind: "magical", mode: "aoe",
            },
        },
        {
            // Basic shot fired along the agent's current aim forward. Cooldown
            // matches unit.attacksPerSec (set per-unit below). Range equals
            // unit.attackRange; think() gates firing on BotAim readiness.
            id: Scenarios.AB_BASIC, slot: Scenarios.AB_BASIC,
            cooldown: 1 / 1.4, manaCost: 0, range: 9,
            kind: "basic-shot",
            projectile: {
                speed: 18, radius: 0.22, damage: 9, life: 1.2,
                kind: "physical", mode: "single",
            },
        },
    ];

    var DEFAULT_UNIT_STATS = {
        speed: 5.2,
        radius: 0.4,
        hp: 100,
        damage: 12,
        attackRange: 9,
        maxMana: 100,
        mana: 60,
        manaRegenPerSec: 8,
        attacksPerSec: 1.4,
        armor: 4,
    };

    // Rejection-sample up to `target` non-overlapping boxes in the left half
    // of the arena, then mirror each through the origin so the map has 180°
    // rotational symmetry — both teams see an identical obstacle field.
    function generateSymmetricObstacles(target) {
        var list = [];
        var minHalf = 0.4, maxHalf = 1.8;
        // Gap between boxes, and between any box and the mid-line (x=0) so
        // mirrored partners never touch. Also the min distance from the spawn
        // columns (x=±17) so spawns stay walkable.
        var gap = 0.8;
        var midGap = 0.6;
        var spawnX = 17, spawnGap = 2.5;
        var maxTries = 400;

        function collides(o) {
            if (o.x + o.hw + midGap > 0) return true;  // crosses/nears mid-line
            if (o.x - o.hw < -spawnX + spawnGap) return true;  // too close to spawn column
            if (o.z + o.hd > 19.5 || o.z - o.hd < -19.5) return true;  // arena edge
            for (var i = 0; i < list.length; i++) {
                var p = list[i];
                if (Math.abs(o.x - p.x) < o.hw + p.hw + gap &&
                    Math.abs(o.z - p.z) < o.hd + p.hd + gap) return true;
            }
            return false;
        }

        for (var tries = 0; tries < maxTries && list.length < target; tries++) {
            var hw = minHalf + Math.random() * (maxHalf - minHalf);
            var hd = minHalf + Math.random() * (maxHalf - minHalf);
            var x = -14 + Math.random() * 12;     // ~[-14, -2]
            var z = -16 + Math.random() * 32;     // ~[-16, 16]
            var o = { x: x, z: z, hw: hw, hd: hd };
            if (!collides(o)) list.push(o);
        }

        var full = list.slice();
        for (var i = 0; i < list.length; i++) {
            var p = list[i];
            full.push({ x: -p.x, z: -p.z, hw: p.hw, hd: p.hd });
        }
        return full;
    }

    Scenarios.DEFAULT_8V8 = {
        id: "default_8v8",
        name: "8v8 open field",
        bounds: { minX: -20, minZ: -20, maxX: 20, maxZ: 20 },
        navCell: 0.5,
        // Physics has no obstacle collision; the nav grid is the sole
        // authority on where agents can be. No inflation — blocked cells
        // are exactly the real obstacle footprint, so path waypoints hug
        // the true edge and the unstick code only fires on real clipping.
        navPadding: 0,
        colors: { 0: "#e74c3c", 1: "#3498db" },
        obstacles: generateSymmetricObstacles(11),
        roster: rosterLine(RED8,  0, -17, -14, 4, 0)
          .concat(rosterLine(BLUE8, 1,  17, -14, 4, RED8.length)),
        unitDefaults: DEFAULT_UNIT_STATS,
        abilities: DEFAULT_ABILITIES,
    };

    Scenarios.ALL = [Scenarios.DEFAULT_8V8];

    Scenarios.byId = function (id) {
        for (var i = 0; i < Scenarios.ALL.length; i++) {
            if (Scenarios.ALL[i].id === id) return Scenarios.ALL[i];
        }
        return null;
    };
})();
