// ai.js — think(self, world) functions for each archetype.
//
// All three archetypes share the same signature. Differences:
//   - tower  : no movement caps → .moveTo / .laneWalk not exposed
//   - minion : lane walk + basic attack
//   - hero   : full caps; player input overrides the think via pendingIntent

const AI = (function () {
    "use strict";

    // Prefer minion targets in range, then lowest-HP non-tower, then any.
    function pickTarget(self, world) {
        const inRange = [];
        const candidates = world.enemiesInRange(self.agent, self.attackRange);
        for (let i = 0; i < candidates.length; i++) inRange.push(candidates[i]);
        if (inRange.length === 0) return null;
        // Sort: minions (speed>0, hp<200) first, then lowest hp.
        inRange.sort(function (a, b) {
            const am = a.unit.maxHp < 200 ? 0 : 1;
            const bm = b.unit.maxHp < 200 ? 0 : 1;
            if (am !== bm) return am - bm;
            return a.unit.hp - b.unit.hp;
        });
        return inRange[0];
    }

    // Tower: attack nearest in range, else hold. Stationary.
    function towerThink(self, world) {
        const t = pickTarget(self, world);
        if (t) return self.attack(t.unit.id);
        return self.hold();
    }

    // Minion: attack anything in range, else lane walk toward enemy nexus.
    function minionThink(self, world) {
        const t = pickTarget(self, world);
        if (t) return self.attack(t.unit.id);
        return self.laneWalk();
    }

    // Hero bot (enemy side). Kites: flee below 30% HP; else attack-nearest
    // or close the gap.
    function heroBotThink(self, world) {
        if (self.hp < 0.30 * 300) return self.flee();
        const t = pickTarget(self, world);
        if (t) return self.attack(t.unit.id);
        // Approach nearest enemy
        const e = world.nearestEnemy(self.agent);
        if (e) return self.moveTo(e.x, e.z);
        return self.hold();
    }

    return { towerThink, minionThink, heroBotThink, pickTarget };
})();
