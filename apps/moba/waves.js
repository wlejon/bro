// waves.js — periodic minion wave spawner.
//
// Every WAVE_INTERVAL seconds, spawn N minions per lane per team. Each minion
// gets an AgentBinding with the minion think + that lane's waypoints.

const Waves = (function () {
    "use strict";

    const WAVE_INTERVAL_MS = 30000;
    const MINIONS_PER_LANE = 3;

    let nextId = 1000; // unit ids — keep disjoint from towers/heroes

    function spawnOne(ctx, team, lane, idx) {
        const spawn = Map.MINION_SPAWN[team === 0 ? "red" : "blue"][lane];
        const lateral = (idx - (MINIONS_PER_LANE - 1) / 2) * 0.8;
        const minion = Units.makeMinion({
            nav: ctx.nav, team,
            id: nextId++,
            x: spawn.x, z: spawn.z + lateral,
        });
        ctx.world.addAgent(minion);
        ctx.allAgents.push(minion);

        const mesh = ctx.scene.createMesh({
            mesh: "capsule", radius: 0.4, halfHeight: 0.6,
            color: team === 0 ? "#e74c3c" : "#3498db",
            x: minion.x, y: 0.6, z: minion.z,
        });
        ctx.unitNodes[minion.unit.id] = mesh;

        const wps = Map.LANES[team === 0 ? "red" : "blue"][lane];
        mesh.attachAgent(ctx.world, minion, {
            capabilities: ["lane_walk", "basic_attack", "hold"],
            thinkHz: 8,
            yOffset: 0.6,
            laneWaypoints: wps,
            think: AI.minionThink,
        });
    }

    function spawnWave(ctx) {
        for (const team of [0, 1]) {
            for (const lane of ["top", "mid", "bot"]) {
                for (let i = 0; i < MINIONS_PER_LANE; i++) {
                    spawnOne(ctx, team, lane, i);
                }
            }
        }
    }

    // Starts the wave timer. Returns a handle that can stop it.
    function start(ctx) {
        spawnWave(ctx); // initial wave at t=0
        const handle = setInterval(function () { spawnWave(ctx); }, WAVE_INTERVAL_MS);
        return { stop: function () { clearInterval(handle); } };
    }

    return { start, spawnWave };
})();
