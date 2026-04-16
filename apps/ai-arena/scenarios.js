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

    Scenarios.DEFAULT_8V8 = {
        id: "default_8v8",
        name: "8v8 open field",
        bounds: { minX: -20, minZ: -20, maxX: 20, maxZ: 20 },
        navCell: 0.5,
        navPadding: 0.55,
        colors: { 0: "#e74c3c", 1: "#3498db" },
        obstacles: [
            { x: -8, z: -8, hw: 1.5, hd: 1.5 },
            { x:  8, z:  8, hw: 1.5, hd: 1.5 },
            { x: -8, z:  8, hw: 1.0, hd: 3.0 },
            { x:  8, z: -8, hw: 3.0, hd: 1.0 },
            { x:  0, z:  0, hw: 1.0, hd: 1.0 },
            { x:-14, z:  0, hw: 0.5, hd: 2.5 },
            { x: 14, z:  0, hw: 0.5, hd: 2.5 },
        ],
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
