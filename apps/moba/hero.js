// hero.js — player-controlled hero. Input events write into a pendingIntent
// slot; the think() fn consumes it each tick.

const Hero = (function () {
    "use strict";

    function makePlayerHero(ctx) {
        const hero = Units.makeHero({
            nav: ctx.nav,
            id: 2,
            team: 0,
            x: Map.NEXUS.red.x + 1,
            z: Map.NEXUS.red.z - 1,
        });
        ctx.world.addAgent(hero);
        ctx.allAgents.push(hero);

        const mesh = ctx.scene.createMesh({
            mesh: "capsule", radius: 0.5, halfHeight: 0.9,
            color: "#ffd24a",
            x: hero.x, y: 0.9, z: hero.z,
        });
        ctx.unitNodes[hero.unit.id] = mesh;

        // Intent slot written by input handlers below.
        const intent = { kind: "hold" };

        mesh.attachAgent(ctx.world, hero, {
            capabilities: ["move_to", "basic_attack", "cast_ability", "hold"],
            thinkHz: 20,
            yOffset: 0.9,
            think: function (self, world) {
                if (intent.kind === "move") {
                    return self.moveTo(intent.x, intent.z);
                }
                if (intent.kind === "attack") {
                    const e = world.findById(intent.targetId);
                    if (!e || !e.unit.alive) { intent.kind = "hold"; return self.hold(); }
                    if (self.inRange(e)) return self.attack(intent.targetId);
                    return self.moveTo(e.x, e.z);
                }
                return self.hold();
            },
        });

        // Click-to-move: project screen coords to world (rough ortho unproject).
        // Canvas covers the visible world [-20..20] in x/z after ortho setup.
        function screenToWorld(evt) {
            const r = ctx.canvas.getBoundingClientRect();
            const nx = (evt.clientX - r.left) / r.width;  // 0..1
            const ny = (evt.clientY - r.top) / r.height;
            // Camera looks down at origin from (+x, +y, +z) with size ~30.
            // Inverse of projection handled by scene.pickGround if available;
            // fallback: assume the ortho box maps linearly.
            const half = ctx.VIEW_HALF;
            const wx = (nx * 2 - 1) * half;
            const wz = (ny * 2 - 1) * half;
            // Account for 30-degree tilt: z scale ~ 1/cos(30deg) ≈ 1.155.
            // For the demo we use a simple linear mapping; precision is fine
            // within a couple of units.
            return { x: wx, z: wz };
        }

        ctx.canvas.addEventListener("contextmenu", function (e) {
            e.preventDefault();
        });

        ctx.canvas.addEventListener("mousedown", function (e) {
            const p = screenToWorld(e);
            if (e.button === 0) {
                // Left click: move
                intent.kind = "move";
                intent.x = p.x; intent.z = p.z;
            } else if (e.button === 2) {
                // Right click: attack-move on nearest enemy near the click
                let best = null, bestD = Infinity;
                const enemies = ctx.world.agents;
                for (const ag of enemies) {
                    if (!ag.unit || !ag.unit.alive || ag.unit.teamId === 0) continue;
                    const dx = ag.x - p.x, dz = ag.z - p.z;
                    const d = dx*dx + dz*dz;
                    if (d < bestD && d < 9) { best = ag; bestD = d; }
                }
                if (best) {
                    intent.kind = "attack";
                    intent.targetId = best.unit.id;
                } else {
                    intent.kind = "move";
                    intent.x = p.x; intent.z = p.z;
                }
            }
        });

        return { hero, mesh, intent };
    }

    return { makePlayerHero };
})();
