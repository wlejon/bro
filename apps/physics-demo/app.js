// Physics Demo — top-down view of a 3D Jolt Physics scene rendered on Canvas 2D
// Click = drop sphere, right-click = drop box, buttons for gravity/explode/reset

var canvas = document.querySelector('#canvas');
var ctx = canvas.getContext('2d');
var stats = document.querySelector('#stats');

// Canvas dimensions (bro uses canvasWidth/canvasHeight, browsers use canvas.width)
function getW() { return ctx.canvasWidth || canvas.width || 1024; }
function getH() { return ctx.canvasHeight || canvas.height || 768; }

// World scale: 1 physics unit = 40 pixels
var SCALE = 40;

// Track all bodies with their metadata
var bodies = []; // {id, shape, size, color, isStatic}

// Color palette for dynamic bodies
var COLORS = [
    '#ff6b6b', '#ffa502', '#ffdd59', '#2ed573',
    '#1e90ff', '#a55eea', '#ff6348', '#7bed9f',
    '#70a1ff', '#eccc68', '#ff4757', '#5352ed'
];
var colorIndex = 0;

function nextColor() {
    return COLORS[colorIndex++ % COLORS.length];
}

// Convert physics coords (Y-up) to canvas coords (Y-down)
function toScreen(px, py) {
    var w = getW();
    var h = getH();
    var originX = w / 2;
    var originY = h - 60;
    return [originX + px * SCALE, originY - py * SCALE];
}

// --- Create ground ---
var ground = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 12, y: 0.5, z: 100 },
    position: { x: 0, y: 0, z: 0 },
    static: true,
    friction: 0.8,
    restitution: 0.2
});
bodies.push({ id: ground, shape: 'box', w: 12, h: 0.5, color: '#444', isStatic: true });

// Side walls
var wallL = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 0.5, y: 6, z: 100 },
    position: { x: -12.5, y: 6, z: 0 },
    static: true,
    friction: 0.5
});
bodies.push({ id: wallL, shape: 'box', w: 0.5, h: 6, color: '#444', isStatic: true });

var wallR = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 0.5, y: 6, z: 100 },
    position: { x: 12.5, y: 6, z: 0 },
    static: true,
    friction: 0.5
});
bodies.push({ id: wallR, shape: 'box', w: 0.5, h: 6, color: '#444', isStatic: true });

// A couple of ramps
var ramp1 = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 3, y: 0.2, z: 100 },
    position: { x: -4, y: 3, z: 0 },
    rotation: { x: 0, y: 0, z: 0.19866, w: 0.98007 },
    static: true,
    friction: 0.5
});
bodies.push({ id: ramp1, shape: 'box', w: 3, h: 0.2, color: '#555', isStatic: true });

var ramp2 = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 3, y: 0.2, z: 100 },
    position: { x: 4, y: 6, z: 0 },
    rotation: { x: 0, y: 0, z: -0.14944, w: 0.98877 },
    static: true,
    friction: 0.5
});
bodies.push({ id: ramp2, shape: 'box', w: 3, h: 0.2, color: '#555', isStatic: true });

// --- Spawn initial demo objects ---
for (var si = 0; si < 10; si++) {
    var sx = -4 + Math.random() * 8;
    var sy = 8 + Math.random() * 6;
    if (si % 2 === 0) {
        var r = 0.2 + Math.random() * 0.3;
        var bid = Physics.createBody({
            shape: 'sphere',
            radius: r,
            position: { x: sx, y: sy, z: 0 },
            friction: 0.4,
            restitution: 0.6
        });
        bodies.push({ id: bid, shape: 'sphere', r: r, color: nextColor(), isStatic: false });
    } else {
        var bsize = 0.3 + Math.random() * 0.3;
        var bid = Physics.createBody({
            shape: 'box',
            halfExtents: { x: bsize, y: bsize, z: bsize },
            position: { x: sx, y: sy, z: 0 },
            friction: 0.6,
            restitution: 0.3
        });
        bodies.push({ id: bid, shape: 'box', w: bsize, h: bsize, color: nextColor(), isStatic: false });
    }
}

// --- Input (use document.body for event handling) ---
document.body.addEventListener('mousedown', function(e) {
    var w = getW();
    var h = getH();
    var originX = w / 2;
    var originY = h - 60;

    // Use clientX/clientY directly (canvas is fullscreen)
    var mx = e.clientX;
    var my = e.clientY;

    // Convert screen to physics coords
    var px = (mx - originX) / SCALE;
    var py = (originY - my) / SCALE;

    // Don't spawn if clicking in the HUD area
    if (mx < 230 && my < 160) return;

    if (e.button === 3 || e.button === 2) {
        // Right click: box (SDL button 3 or DOM button 2)
        var size = 0.3 + Math.random() * 0.4;
        var id = Physics.createBody({
            shape: 'box',
            halfExtents: { x: size, y: size, z: size },
            position: { x: px, y: py, z: 0 },
            friction: 0.6,
            restitution: 0.3
        });
        bodies.push({ id: id, shape: 'box', w: size, h: size, color: nextColor(), isStatic: false });
    } else {
        // Left click (default): sphere
        var radius = 0.2 + Math.random() * 0.3;
        var id = Physics.createBody({
            shape: 'sphere',
            radius: radius,
            position: { x: px, y: py, z: 0 },
            friction: 0.4,
            restitution: 0.6
        });
        bodies.push({ id: id, shape: 'sphere', r: radius, color: nextColor(), isStatic: false });
    }
});

// --- Buttons ---
document.querySelector('#btn-reset').addEventListener('click', function() {
    for (var i = bodies.length - 1; i >= 0; i--) {
        if (!bodies[i].isStatic) {
            Physics.destroyBody(bodies[i].id);
            bodies.splice(i, 1);
        }
    }
});

var zeroGravity = false;
document.querySelector('#btn-gravity').addEventListener('click', function() {
    zeroGravity = !zeroGravity;
    if (zeroGravity) {
        Physics.setGravity(0, 0, 0);
        document.querySelector('#btn-gravity').textContent = 'Normal Gravity';
    } else {
        Physics.setGravity(0, -9.81, 0);
        document.querySelector('#btn-gravity').textContent = 'Zero Gravity';
    }
});

document.querySelector('#btn-explode').addEventListener('click', function() {
    for (var i = 0; i < bodies.length; i++) {
        var b = bodies[i];
        if (b.isStatic) continue;
        var t = Physics.getTransform(b.id);
        if (!t) continue;
        var dx = t.position.x;
        var dy = t.position.y - 5;
        var len = Math.sqrt(dx * dx + dy * dy) || 1;
        var force = 15;
        Physics.addImpulse(b.id, (dx / len) * force, (dy / len) * force, 0);
    }
});

// --- Render loop ---
function draw() {
    var w = getW();
    var h = getH();
    var originX = w / 2;
    var originY = h - 60;

    ctx.clearRect(0, 0, w, h);

    // Background
    ctx.fillStyle = '#1a1a2e';
    ctx.fillRect(0, 0, w, h);

    // Draw grid (single batched path)
    ctx.strokeStyle = 'rgba(255,255,255,0.05)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (var x = -15; x <= 15; x++) {
        var gx = originX + x * SCALE;
        ctx.moveTo(gx, 0);
        ctx.lineTo(gx, h);
    }
    for (var y = -2; y <= 20; y++) {
        var gy = originY - y * SCALE;
        ctx.moveTo(0, gy);
        ctx.lineTo(w, gy);
    }
    ctx.stroke();

    // Get all transforms as packed Float32Array: [id,x,y,z,rx,ry,rz,rw, ...]
    var transforms = Physics.getAllTransforms();
    var transformMap = {};
    for (var ti = 0; ti < transforms.length; ti += 8) {
        var tid = transforms[ti];
        transformMap[tid] = ti;
    }

    // Draw bodies
    var dynamicCount = 0;
    for (var i = 0; i < bodies.length; i++) {
        var b = bodies[i];
        var tIdx = transformMap[b.id];
        if (tIdx === undefined) continue;

        var tx = transforms[tIdx + 1];
        var ty = transforms[tIdx + 2];
        var trz = transforms[tIdx + 6];
        var trw = transforms[tIdx + 7];

        var screenPos = toScreen(tx, ty);
        var sx = screenPos[0];
        var sy = screenPos[1];

        if (b.shape === 'sphere') {
            // Circles don't need save/restore/rotate
            var r = b.r * SCALE;
            ctx.fillStyle = b.color;
            ctx.beginPath();
            ctx.arc(sx, sy, r, 0, 6.2832);
            ctx.fill();
        } else if (b.shape === 'box') {
            var angle = -2 * Math.atan2(trz, trw);
            ctx.save();
            ctx.translate(sx, sy);
            ctx.rotate(angle);
            var hw = b.w * SCALE;
            var hh = b.h * SCALE;
            ctx.fillStyle = b.color;
            ctx.fillRect(-hw, -hh, hw * 2, hh * 2);
            ctx.restore();
        }

        if (!b.isStatic) dynamicCount++;
    }

    stats.textContent = 'Bodies: ' + dynamicCount;

    requestAnimationFrame(draw);
}

requestAnimationFrame(draw);
