// units.js — stat presets + spawn helpers.

const Units = (function () {
    "use strict";

    function makeMinion(opts) {
        const a = bro.ai.game.createAgent({
            navGrid: opts.nav,
            x: opts.x, z: opts.z,
            speed: 3.5, radius: 0.4,
            id: opts.id,
            teamId: opts.team,
            hp: 60, damage: 8, attackRange: 2.5,
        });
        const u = a.unit;
        u.maxHp = 60;
        u.attacksPerSec = 1.0;
        u.armor = 2;
        return a;
    }

    function makeTower(opts) {
        const a = bro.ai.game.createAgent({
            navGrid: opts.nav,
            x: opts.x, z: opts.z,
            speed: 0, radius: 0.8,
            id: opts.id,
            teamId: opts.team,
            hp: 500, damage: 40, attackRange: 7,
        });
        const u = a.unit;
        u.maxHp = 500;
        u.attacksPerSec = 0.7;
        u.armor = 10;
        return a;
    }

    function makeNexus(opts) {
        const a = bro.ai.game.createAgent({
            navGrid: opts.nav,
            x: opts.x, z: opts.z,
            speed: 0, radius: 1.5,
            id: opts.id,
            teamId: opts.team,
            hp: 1000, damage: 0, attackRange: 0,
        });
        a.unit.maxHp = 1000;
        return a;
    }

    function makeHero(opts) {
        const a = bro.ai.game.createAgent({
            navGrid: opts.nav,
            x: opts.x, z: opts.z,
            speed: 5.5, radius: 0.5,
            id: opts.id,
            teamId: opts.team,
            hp: 300, damage: 35, attackRange: 5,
        });
        const u = a.unit;
        u.maxHp = 300;
        u.mana = 100; u.maxMana = 100; u.manaRegenPerSec = 4;
        u.attacksPerSec = 1.2;
        u.armor = 12;
        return a;
    }

    return { makeMinion, makeTower, makeNexus, makeHero };
})();
