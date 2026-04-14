// main.js — bootstrap the MOBA demo.

(function () {
    "use strict";

    const canvas = document.getElementById("world");
    const scene  = canvas.getContext("scene");

    const B = Map.BOUNDS;
    const VIEW_HALF = 22;

    function updateCamera() {
        const w = canvas.clientWidth  || canvas.width  || 1100;
        const h = canvas.clientHeight || canvas.height || 780;
        // Perspective camera angled for an isometric-ish feel. Elevated +y
        // with a slight +x/+z offset so we're looking down-southwest.
        scene.setCamera({
            fov: 42,
            aspect: w / h,
            near: 0.5, far: 200,
            position: [26, 38, 26],
            target:   [0, 0, 0],
            up:       [0, 1, 0],
        });
    }
    updateCamera();

    // Ground plane + lane markers + obstacles (scene-only, no collision role).
    scene.createMesh({
        mesh: "plane", halfW: VIEW_HALF, halfD: VIEW_HALF,
        color: "#2b3142", y: 0,
    });
    for (const ob of Map.OBSTACLES) {
        scene.createMesh({
            mesh: "box", halfW: ob.hw, halfH: 0.6, halfD: ob.hd,
            color: "#3f2a1f", x: ob.x, y: 0.6, z: ob.z,
        });
    }
    // Draw a subtle line along each lane mid using thin boxes.
    function drawLane(color, points) {
        for (let i = 0; i + 1 < points.length; i++) {
            const a = points[i], b = points[i+1];
            const cx = (a.x + b.x) / 2, cz = (a.z + b.z) / 2;
            const dx = b.x - a.x, dz = b.z - a.z;
            const len = Math.sqrt(dx*dx + dz*dz) / 2;
            const ang = Math.atan2(dx, dz);
            const node = scene.createMesh({
                mesh: "box", halfW: 0.25, halfH: 0.01, halfD: len,
                color, x: cx, y: 0.02, z: cz,
            });
            node.rotationY = ang;
        }
    }
    drawLane("#3a3f55", Map.LANES.red.top);
    drawLane("#3a3f55", Map.LANES.red.mid);
    drawLane("#3a3f55", Map.LANES.red.bot);

    // --- AI world setup ---
    const nav   = bro.ai.game.createNavGrid({
        minX: B.minX, minZ: B.minZ, maxX: B.maxX, maxZ: B.maxZ,
        cellSize: B.cellSize, obstacles: Map.OBSTACLES, padding: 0.5,
    });
    const world = bro.ai.game.createWorld();
    for (const ob of Map.OBSTACLES) world.addObstacle(ob);

    const ctx = {
        canvas, scene, nav, world,
        allAgents: [],
        unitNodes: {},
        VIEW_HALF,
    };

    // --- Structures: nexuses + towers ---
    function spawnStatic(team, cfg, radius, halfH, color, id, factory) {
        const agent = factory({ nav, team, id, x: cfg.x, z: cfg.z });
        world.addAgent(agent);
        ctx.allAgents.push(agent);
        const mesh = scene.createMesh({
            mesh: "cylinder", radius, halfHeight: halfH,
            color, x: agent.x, y: halfH, z: agent.z,
        });
        ctx.unitNodes[agent.unit.id] = mesh;
        mesh.attachAgent(world, agent, {
            capabilities: ["basic_attack", "hold"],
            thinkHz: 4,
            yOffset: halfH,
            faceMovement: false,
            think: AI.towerThink,
        });
        return { agent, mesh };
    }

    const nexusRed  = spawnStatic(0, Map.NEXUS.red,  2.0, 1.5,
        "#c03020", 100, Units.makeNexus);
    const nexusBlue = spawnStatic(1, Map.NEXUS.blue, 2.0, 1.5,
        "#2060c0", 101, Units.makeNexus);
    // Nexus has attackRange=0; attackThink is a no-op because pickTarget
    // returns null. We keep it so attachAgent is uniform.

    let towerId = 200;
    for (const t of Map.TOWERS) {
        spawnStatic(t.team, { x: t.x, z: t.z }, 0.8, 1.2,
            t.team === 0 ? "#e85949" : "#4a8de8",
            towerId++, Units.makeTower);
    }

    // --- AI world auto-tick ---
    scene.attachAIWorld(world, { stepHz: 60, maxStepsPerFrame: 4 });

    // --- Waves + player hero ---
    Waves.start(ctx);
    const player = Hero.makePlayerHero(ctx);
    // An AI-driven enemy hero on the blue team.
    const enemyHero = Units.makeHero({
        nav, team: 1, id: 3,
        x: Map.NEXUS.blue.x - 1, z: Map.NEXUS.blue.z + 1,
    });
    world.addAgent(enemyHero);
    ctx.allAgents.push(enemyHero);
    const enemyMesh = scene.createMesh({
        mesh: "capsule", radius: 0.5, halfHeight: 0.9,
        color: "#82e8ff",
        x: enemyHero.x, y: 0.9, z: enemyHero.z,
    });
    ctx.unitNodes[enemyHero.unit.id] = enemyMesh;
    enemyMesh.attachAgent(world, enemyHero, {
        capabilities: ["move_to", "basic_attack", "flee", "hold"],
        thinkHz: 15,
        yOffset: 0.9,
        think: AI.heroBotThink,
    });

    // --- Per-frame housekeeping: update HUD, prune dead units, check win ---
    const redNexusEl  = document.getElementById("red-nexus");
    const blueNexusEl = document.getElementById("blue-nexus");
    const bannerEl    = document.getElementById("banner");
    let gameOver = false;

    function tickUI() {
        if (gameOver) return;
        updateCamera();
        redNexusEl.textContent  = Math.max(0, Math.round(nexusRed.agent.unit.hp));
        blueNexusEl.textContent = Math.max(0, Math.round(nexusBlue.agent.unit.hp));

        // Prune dead units.
        for (let i = ctx.allAgents.length - 1; i >= 0; i--) {
            const a = ctx.allAgents[i];
            if (a.unit.alive) continue;
            const id = a.unit.id;
            const node = ctx.unitNodes[id];
            if (node) { node.detachAgent(); node.destroy(); delete ctx.unitNodes[id]; }
            world.removeAgent(a);
            ctx.allAgents.splice(i, 1);
        }

        // Win condition.
        if (!nexusRed.agent.unit.alive) {
            bannerEl.textContent = "BLUE WINS";
            bannerEl.classList.remove("hidden");
            gameOver = true;
        } else if (!nexusBlue.agent.unit.alive) {
            bannerEl.textContent = "RED WINS";
            bannerEl.classList.remove("hidden");
            gameOver = true;
        }

        requestAnimationFrame(tickUI);
    }
    requestAnimationFrame(tickUI);

    // Expose for headless debugging.
    window.getState = function () { return { world, ctx, player }; };
    console.log("MOBA demo started — 3 lanes, 12 towers, 2 nexuses, 2 heroes");
})();
