// 3D Scene Graph Demo — showcases all new 3D transform features
// Scenes: solar system (hierarchy), 3D axes (per-axis rotation), spiral tower (z + scale), physics sync

var canvas = document.querySelector('#canvas');
var scene = canvas.getContext('scene');
var info = document.querySelector('#info');

var W = canvas.clientWidth, H = canvas.clientHeight;
var CX = W / 2, CY = H / 2;

var paused = false;
var currentDemo = 0;
var time = 0;
var nodes = []; // track created nodes for cleanup

// --- Helpers ---

function clear() {
    for (var i = 0; i < nodes.length; i++) {
        nodes[i].destroy();
    }
    nodes = [];
    scene.cameraX = 0;
    scene.cameraY = 0;
    scene.cameraZoom = 1;
}

function hsl(h, s, l) {
    // Simple HSL to hex (s, l in 0-100)
    s /= 100; l /= 100;
    var c = (1 - Math.abs(2 * l - 1)) * s;
    var x = c * (1 - Math.abs((h / 60) % 2 - 1));
    var m = l - c / 2;
    var r, g, b;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    var toHex = function(v) {
        var h = Math.round((v + m) * 255).toString(16);
        return h.length < 2 ? '0' + h : h;
    };
    return '#' + toHex(r) + toHex(g) + toHex(b);
}

// ============================================================================
// Demo 1: Solar System — hierarchical transforms with Z-axis rotation
// ============================================================================

function demoSolarSystem() {
    clear();
    info.textContent = 'Solar System: parent-child hierarchy, Z rotation, scale inheritance';

    // Sun at center
    var sun = scene.createShape({
        shape: 'circle', radius: 40,
        fill: '#ffa502', x: CX, y: CY, name: 'sun'
    });
    nodes.push(sun);

    // Glow rings
    var glowAlphas = ['4d', '33', '1a']; // 0.3, 0.2, 0.1 as hex
    for (var r = 0; r < 3; r++) {
        var glow = scene.createShape({
            shape: 'circle', radius: 44 + r * 5,
            fill: '#00000000',
            stroke: '#ffa502' + glowAlphas[r],
            strokeWidth: 2
        });
        sun.add(glow);
        nodes.push(glow);
    }

    var planets = [
        { dist: 90,  radius: 10, color: '#a5b1c2', speed: 2.0,  name: 'Mercury' },
        { dist: 130, radius: 14, color: '#f7b731', speed: 1.4,  name: 'Venus' },
        { dist: 180, radius: 16, color: '#2d98da', speed: 1.0,  name: 'Earth',
          moons: [{ dist: 28, radius: 5, color: '#d1d8e0', speed: 4.0 }] },
        { dist: 240, radius: 12, color: '#eb3b5a', speed: 0.7,  name: 'Mars',
          moons: [
              { dist: 22, radius: 3, color: '#fc5c65', speed: 5.0 },
              { dist: 30, radius: 2, color: '#fd9644', speed: 3.5 }
          ] },
        { dist: 320, radius: 28, color: '#e67e22', speed: 0.35, name: 'Jupiter',
          moons: [
              { dist: 40, radius: 6, color: '#f5cd79', speed: 3.0 },
              { dist: 52, radius: 5, color: '#aaa69d', speed: 2.2 },
              { dist: 64, radius: 7, color: '#e77f67', speed: 1.5 },
              { dist: 76, radius: 4, color: '#cf6a87', speed: 1.0 }
          ] }
    ];

    for (var i = 0; i < planets.length; i++) {
        var p = planets[i];

        // Orbit ring (visual only, child of sun so it stays centered)
        var orbit = scene.createShape({
            shape: 'circle', radius: p.dist,
            fill: '#00000000',
            stroke: '#ffffff14', strokeWidth: 1
        });
        sun.add(orbit);
        nodes.push(orbit);

        // Orbit pivot — rotates around sun
        var pivot = scene.createNode('pivot_' + p.name);
        sun.add(pivot);
        nodes.push(pivot);
        pivot._speed = p.speed;
        pivot._kind = 'orbit';

        // Planet body — offset from pivot
        var body = scene.createShape({
            shape: 'circle', radius: p.radius,
            fill: p.color, x: p.dist, name: p.name
        });
        pivot.add(body);
        nodes.push(body);

        // Self-rotation marker (line from center)
        var marker = scene.createShape({
            shape: 'line'
        });
        marker.strokeColor = 'white';
        marker.strokeWidth = 1;
        body.add(marker);
        nodes.push(marker);
        body._speed = p.speed * 3;
        body._kind = 'spin';

        // Moons
        if (p.moons) {
            for (var m = 0; m < p.moons.length; m++) {
                var moon = p.moons[m];
                var moonPivot = scene.createNode('moonpivot_' + i + '_' + m);
                body.add(moonPivot);
                nodes.push(moonPivot);
                moonPivot._speed = moon.speed;
                moonPivot._kind = 'orbit';

                var moonBody = scene.createShape({
                    shape: 'circle', radius: moon.radius,
                    fill: moon.color, x: moon.dist
                });
                moonPivot.add(moonBody);
                nodes.push(moonBody);
            }
        }
    }
}

// ============================================================================
// Demo 2: 3D Axes — demonstrates rotationX, rotationY, rotationZ independently
// ============================================================================

function demoAxes() {
    clear();
    info.textContent = 'Per-axis rotation: rotationX (red), rotationY (green), rotationZ (blue)';

    var labels = [
        { axis: 'X', color: '#ff4757', cx: CX - 300, cy: CY },
        { axis: 'Y', color: '#2ed573', cx: CX,       cy: CY },
        { axis: 'Z', color: '#1e90ff', cx: CX + 300, cy: CY }
    ];

    for (var a = 0; a < labels.length; a++) {
        var L = labels[a];

        // Label
        var label = scene.createShape({
            shape: 'rect', width: 60, height: 24,
            fill: L.color, x: L.cx, y: L.cy - 160, name: 'label_' + L.axis
        });
        nodes.push(label);

        // Rotating group
        var group = scene.createNode('group_' + L.axis);
        group.x = L.cx;
        group.y = L.cy;
        nodes.push(group);
        group._axis = L.axis;
        group._kind = 'axis_rotate';

        // Cross-hair of shapes to show the rotation effect
        var arm1 = scene.createShape({
            shape: 'rect', width: 120, height: 16,
            fill: L.color
        });
        group.add(arm1);
        nodes.push(arm1);

        var arm2 = scene.createShape({
            shape: 'rect', width: 16, height: 120,
            fill: L.color
        });
        group.add(arm2);
        nodes.push(arm2);

        // Corner markers to show orientation
        var corners = [
            { x: 60, y: 60 }, { x: -60, y: 60 },
            { x: 60, y: -60 }, { x: -60, y: -60 }
        ];
        for (var c = 0; c < corners.length; c++) {
            var dot = scene.createShape({
                shape: 'circle', radius: 8,
                fill: 'white', x: corners[c].x, y: corners[c].y
            });
            group.add(dot);
            nodes.push(dot);
        }

        // Center pip
        var pip = scene.createShape({
            shape: 'circle', radius: 12,
            fill: L.color, stroke: 'white', strokeWidth: 2
        });
        group.add(pip);
        nodes.push(pip);
    }
}

// ============================================================================
// Demo 3: Spiral Tower — z-position for depth ordering, 3D scale
// ============================================================================

function demoSpiral() {
    clear();
    info.textContent = 'Spiral: z-position (depth), scaleZ, 3D scale inheritance, localToWorld(x,y,z)';

    var N = 40;
    var baseRadius = 200;

    for (var i = 0; i < N; i++) {
        var t = i / N;
        var angle = t * Math.PI * 4; // 2 full turns
        var x = CX + Math.cos(angle) * baseRadius * (1 - t * 0.5);
        var y = CY + Math.sin(angle) * baseRadius * (1 - t * 0.5);
        var z = i; // z increases → depth ordering

        var size = 30 + (1 - t) * 30;
        var hue = (i / N) * 360;

        var block = scene.createShape({
            shape: 'roundrect', width: size, height: size, cornerRadius: 4,
            fill: hsl(hue, 70, 55),
            stroke: hsl(hue, 80, 35), strokeWidth: 2,
            x: x, y: y, name: 'block_' + i
        });
        block.z = z;
        nodes.push(block);

        // Each block gently oscillates in scale
        block._baseScale = 0.5 + t * 0.5;
        block._phase = t * Math.PI * 2;
        block._kind = 'spiral_block';
    }

    // Central pillar — demonstrates scaleZ (visible in the data, affects children)
    var pillar = scene.createNode('pillar');
    pillar.x = CX;
    pillar.y = CY;
    pillar.scaleZ = 2.0; // 3D scale on Z
    nodes.push(pillar);
    pillar._kind = 'pillar';

    // Pillar visual
    var pillarVis = scene.createShape({
        shape: 'circle', radius: 20,
        fill: '#ffd32a', stroke: '#fff', strokeWidth: 3
    });
    pillar.add(pillarVis);
    nodes.push(pillarVis);

    // Orbiting indicator — its localToWorld will show z=0 * parentScaleZ=2 = 0 in world
    var indicator = scene.createShape({
        shape: 'rect', width: 12, height: 12,
        fill: '#ff6b6b'
    });
    indicator.x = 40;
    indicator.z = 5;
    pillar.add(indicator);
    nodes.push(indicator);
    indicator._kind = 'indicator';
}

// ============================================================================
// Demo 4: Physics Sync — Jolt bodies with full quaternion rotation
// ============================================================================

function demoPhysics() {
    clear();
    info.textContent = 'Physics: Jolt bodies synced via quaternion rotation. Click to spawn.';

    var SCALE = 40;

    // Ground
    var groundId = Physics.createBody({
        shape: 'box',
        halfExtents: { x: 10, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 },
        static: true,
        friction: 0.8,
        restitution: 0.3
    });
    var groundNode = scene.createPhysicsNode({
        body: groundId, pixelsPerUnit: SCALE, name: 'ground'
    });
    var groundVis = scene.createShape({
        shape: 'rect', width: 20 * SCALE, height: 1 * SCALE,
        fill: '#444', stroke: '#666', strokeWidth: 1
    });
    groundNode.add(groundVis);
    nodes.push(groundNode);
    nodes.push(groundVis);

    // Center camera on the ground area
    scene.cameraX = -CX;
    scene.cameraY = -CY + 100;

    // Spawn ramps
    var rampAngles = [0.2, -0.15];
    var rampPositions = [{ x: -4, y: 3 }, { x: 4, y: 5.5 }];
    for (var r = 0; r < 2; r++) {
        var rampId = Physics.createBody({
            shape: 'box',
            halfExtents: { x: 3, y: 0.2, z: 0.5 },
            position: { x: rampPositions[r].x, y: rampPositions[r].y, z: 0 },
            rotation: { x: 0, y: 0, z: Math.sin(rampAngles[r] / 2), w: Math.cos(rampAngles[r] / 2) },
            static: true,
            friction: 0.5
        });
        var rampNode = scene.createPhysicsNode({
            body: rampId, pixelsPerUnit: SCALE, name: 'ramp_' + r
        });
        var rampVis = scene.createShape({
            shape: 'rect', width: 6 * SCALE, height: 0.4 * SCALE,
            fill: '#555', stroke: '#777', strokeWidth: 1
        });
        rampNode.add(rampVis);
        nodes.push(rampNode);
        nodes.push(rampVis);
    }

    // Track dynamic bodies for stats
    var bodyCount = 0;

    // Click to spawn dynamic bodies
    canvas.onclick = function(e) {
        if (currentDemo !== 3) return;
        var bx = (e.offsetX - CX) / SCALE - scene.cameraX / SCALE;
        var by = -(e.offsetY - CY + 100) / SCALE;

        var isBox = (bodyCount % 3 === 0);
        var bodyId;
        var vis;
        var colors = ['#ff6b6b', '#ffa502', '#2ed573', '#1e90ff', '#a55eea', '#ff6348'];
        var col = colors[bodyCount % colors.length];

        if (isBox) {
            bodyId = Physics.createBody({
                shape: 'box',
                halfExtents: { x: 0.4, y: 0.4, z: 0.4 },
                position: { x: bx, y: by, z: 0 },
                mass: 1.0, friction: 0.6, restitution: 0.3
            });
            vis = scene.createShape({
                shape: 'rect', width: 0.8 * SCALE, height: 0.8 * SCALE,
                fill: col, stroke: 'white', strokeWidth: 1
            });
        } else {
            bodyId = Physics.createBody({
                shape: 'sphere',
                radius: 0.35,
                position: { x: bx, y: by, z: 0 },
                mass: 1.0, friction: 0.5, restitution: 0.5
            });
            vis = scene.createShape({
                shape: 'circle', radius: 0.35 * SCALE,
                fill: col, stroke: 'white', strokeWidth: 1
            });
        }

        var pn = scene.createPhysicsNode({
            body: bodyId, pixelsPerUnit: SCALE
        });
        pn.add(vis);
        nodes.push(pn);
        nodes.push(vis);
        bodyCount++;
    };
}

// ============================================================================
// Main loop
// ============================================================================

var demos = [demoSolarSystem, demoAxes, demoSpiral, demoPhysics];
var demoNames = ['Solar System', '3D Axes', 'Spiral Tower', 'Physics Sync'];

function switchDemo(idx) {
    if (idx < 0 || idx >= demos.length) return;
    canvas.onclick = null;
    currentDemo = idx;
    demos[idx]();
}

document.addEventListener('keydown', function(e) {
    if (e.key >= '1' && e.key <= '4') {
        switchDemo(parseInt(e.key) - 1);
    }
    if (e.key === ' ') {
        paused = !paused;
        e.preventDefault();
    }
});

// Start with demo 1
switchDemo(0);

// Animation loop
setInterval(function() {
    if (paused) return;
    time += 0.016;

    // Animate nodes based on their _kind tag
    for (var i = 0; i < nodes.length; i++) {
        var n = nodes[i];
        if (!n || !n._kind) continue;

        if (n._kind === 'orbit') {
            n.rotation = n.rotation + n._speed * 0.016;
        }
        else if (n._kind === 'spin') {
            n.rotation = n.rotation + n._speed * 0.016;
        }
        else if (n._kind === 'axis_rotate') {
            var angle = Math.sin(time * 1.5) * 0.8;
            if (n._axis === 'X') n.rotationX = angle;
            else if (n._axis === 'Y') n.rotationY = angle;
            else if (n._axis === 'Z') n.rotationZ = angle;
        }
        else if (n._kind === 'spiral_block') {
            var s = n._baseScale + Math.sin(time * 2 + n._phase) * 0.15;
            n.scaleX = s;
            n.scaleY = s;
        }
        else if (n._kind === 'pillar') {
            n.rotation = time * 0.5;
            // Pulse scaleZ
            n.scaleZ = 1.5 + Math.sin(time * 2) * 0.5;
        }
        else if (n._kind === 'indicator') {
            // Show localToWorld with z coordinate
            var wp = n.localToWorld(0, 0, 0);
            if (wp) {
                info.textContent = 'Spiral | indicator localToWorld: (' +
                    wp.x.toFixed(1) + ', ' + wp.y.toFixed(1) + ', ' + wp.z.toFixed(1) + ')';
            }
        }
    }

    // Physics sync (demo 4)
    if (currentDemo === 3) {
        scene.syncPhysics();
    }

}, 16);
