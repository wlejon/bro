// Tests for the extended Physics binding.
//
// Run: bro-headless apps/lib-tests apps/lib-tests/test_physics.js

'use strict';

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log('  ok   ' + name); }
    catch (e) {
        failed++;
        console.log('  FAIL ' + name + ': ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}
function eq(a, b, msg) { if (a !== b) throw new Error((msg||'eq') + ': ' + a + ' !== ' + b); }
function near(a, b, eps, msg) {
    eps = eps || 0.01;
    if (Math.abs(a - b) > eps) throw new Error((msg||'near') + ': ' + a + ' !~ ' + b);
}
function truthy(v, msg) { if (!v) throw new Error(msg||'truthy'); }

console.log('=== Physics binding tests ===');

Physics.createWorld({ maxBodies: 1024 });
Physics.setGravity(0, -9.81, 0);

t('user data round-trip via getTransform', function() {
    var b = Physics.createBody({
        shape: 'sphere', radius: 0.5,
        position: { x: 0, y: 5, z: 0 },
        userData: 0xdeadbeef
    });
    truthy(b > 0, 'tag valid');
    var x = Physics.getTransform(b);
    eq(Number(x.userData), 0xdeadbeef, 'userData');
    Physics.setUserData(b, 12345);
    eq(Number(Physics.getUserData(b)), 12345);
    Physics.destroyBody(b);
});

t('2D DOF lock (Plane2D) keeps body on z=0', function() {
    var floor = Physics.createBody({
        shape: 'box', static: true,
        position: { x: 0, y: -1, z: 0 },
        halfExtents: { x: 50, y: 1, z: 50 }
    });
    var b = Physics.createBody({
        shape: 'sphere', radius: 0.4,
        position: { x: 0, y: 5, z: 0 },
        dofs: '2d'
    });
    Physics.setLinearVelocity(b, 1.0, 0, 5.0);  // try to push z
    for (var i = 0; i < 60; i++) advanceTime(16);
    var x = Physics.getTransform(b);
    near(x.position.z, 0, 0.05, 'z stays clamped');
    Physics.destroyBody(b);
    Physics.destroyBody(floor);
});

t('sensor body fires sensor:true contact', function() {
    var trigger = Physics.createBody({
        shape: 'box', static: true, sensor: true,
        position: { x: 0, y: 0, z: 0 },
        halfExtents: { x: 1, y: 1, z: 1 }
    });
    var ball = Physics.createBody({
        shape: 'sphere', radius: 0.3,
        position: { x: 0, y: 5, z: 0 }
    });
    Physics.getContacts();  // drain
    var sawSensor = false;
    for (var i = 0; i < 60 && !sawSensor; i++) {
        advanceTime(16);
        var evs = Physics.getContacts();
        for (var j = 0; j < evs.length; j++) {
            if (evs[j].sensor) sawSensor = true;
        }
    }
    truthy(sawSensor, 'observed sensor contact');
    Physics.destroyBody(ball);
    Physics.destroyBody(trigger);
});

t('distance constraint binds two bodies', function() {
    var a = Physics.createBody({
        shape: 'sphere', radius: 0.3,
        position: { x: 0, y: 5, z: 0 }
    });
    var b = Physics.createBody({
        shape: 'sphere', radius: 0.3,
        position: { x: 1.0, y: 5, z: 0 }
    });
    var j = Physics.createConstraint({
        type: 'distance',
        body1: a, body2: b,
        point1: { x: 0, y: 5, z: 0 },
        point2: { x: 1.0, y: 5, z: 0 },
        minDistance: 0.5, maxDistance: 1.5
    });
    truthy(j > 0, 'constraint handle');
    for (var i = 0; i < 60; i++) advanceTime(16);
    var ta = Physics.getTransform(a).position;
    var tb = Physics.getTransform(b).position;
    var dx = ta.x - tb.x, dy = ta.y - tb.y, dz = ta.z - tb.z;
    var d = Math.sqrt(dx*dx + dy*dy + dz*dz);
    truthy(d <= 1.6, 'distance bounded: ' + d);
    Physics.destroyConstraint(j);
    Physics.destroyBody(a);
    Physics.destroyBody(b);
});

t('convex hull body falls under gravity', function() {
    // Tetrahedron points
    var pts = new Float32Array([
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        0, 0, 1
    ]);
    var floor = Physics.createBody({
        shape: 'box', static: true,
        position: { x: 0, y: -1, z: 0 },
        halfExtents: { x: 50, y: 1, z: 50 }
    });
    var hull = Physics.createBody({
        shape: 'convexHull',
        position: { x: 0, y: 5, z: 0 },
        points: pts
    });
    truthy(hull > 0, 'hull created');
    var startY = Physics.getTransform(hull).position.y;
    for (var i = 0; i < 30; i++) advanceTime(16);
    var nowY = Physics.getTransform(hull).position.y;
    truthy(nowY < startY, 'fell under gravity');
    Physics.destroyBody(hull);
    Physics.destroyBody(floor);
});

t('static box collision sanity', function() {
    var floor = Physics.createBody({
        shape: 'box', static: true,
        position: { x: 0, y: -0.5, z: 0 },
        halfExtents: { x: 5, y: 0.5, z: 5 }
    });
    var ball = Physics.createBody({
        shape: 'sphere', radius: 0.3,
        position: { x: 0, y: 5, z: 0 }
    });
    for (var i = 0; i < 90; i++) advanceTime(16);
    var y = Physics.getTransform(ball).position.y;
    truthy(y > -0.5 && y < 1.5, 'ball lands on box: y=' + y);
    Physics.destroyBody(ball);
    Physics.destroyBody(floor);
});

t('static mesh from polyline-style triangles', function() {
    // First reset layers in case a prior test mucked them
    Physics.setLayers({
        names: ['static', 'moving'],
        matrix: [false, true, true, true]
    });
    // Two-triangle ground plane via mesh
    var positions = new Float32Array([
        -5, 0, -5,
         5, 0, -5,
         5, 0,  5,
        -5, 0,  5,
    ]);
    // Wind triangles CCW so normal points +Y (Jolt MeshShape is one-sided).
    var indices = new Uint32Array([0, 2, 1, 0, 3, 2]);
    var ground = Physics.createBody({
        shape: 'mesh',
        static: true,
        positions: positions,
        indices: indices,
        position: { x: 0, y: 0, z: 0 }
    });
    truthy(ground > 0, 'mesh created');
    var ball = Physics.createBody({
        shape: 'sphere', radius: 0.5,
        position: { x: 0, y: 3, z: 0 },
        ccd: true
    });
    for (var i = 0; i < 90; i++) advanceTime(16);
    var y = Physics.getTransform(ball).position.y;
    truthy(y > -0.5 && y < 2.0, 'ball rests near mesh: y=' + y);
    Physics.destroyBody(ball);
    Physics.destroyBody(ground);
});

t('compound shape (two boxes)', function() {
    var c = Physics.createBody({
        shape: 'compound',
        position: { x: 0, y: 5, z: 0 },
        parts: [
            { shape: 'box', halfExtents: {x:0.5,y:0.5,z:0.5}, localPosition: {x:-0.5,y:0,z:0} },
            { shape: 'box', halfExtents: {x:0.5,y:0.5,z:0.5}, localPosition: {x:0.5,y:0,z:0} }
        ]
    });
    truthy(c > 0, 'compound created');
    Physics.destroyBody(c);
});

t('layer pair filtering — static layer 0 vs ghost layer 2', function() {
    var ok = Physics.setLayers({
        names: ['static', 'moving', 'ghost'],
        // 3x3, row-major. ghost (2) collides with nothing.
        matrix: [
            false, true,  false,   // static
            true,  true,  false,   // moving
            false, false, false,   // ghost
        ]
    });
    truthy(ok, 'configureLayers ok');
    var floor = Physics.createBody({
        shape: 'box', static: true, layer: 'static',
        position: {x:0, y:-1, z:0},
        halfExtents: {x:50, y:1, z:50}
    });
    var ghost = Physics.createBody({
        shape: 'sphere', radius: 0.3, layer: 'ghost',
        position: {x:0, y:5, z:0}
    });
    var startY = Physics.getTransform(ghost).position.y;
    for (var i = 0; i < 60; i++) advanceTime(16);
    var nowY = Physics.getTransform(ghost).position.y;
    truthy(nowY < startY - 1.0, 'ghost falls through floor: ' + startY + ' -> ' + nowY);
    Physics.destroyBody(ghost);
    Physics.destroyBody(floor);
    // Reset layers to defaults
    Physics.setLayers({
        names: ['static', 'moving'],
        matrix: [false, true, true, true]
    });
});

t('CCD flag accepted', function() {
    var b = Physics.createBody({
        shape: 'sphere', radius: 0.1,
        position: { x: 0, y: 5, z: 0 },
        ccd: true
    });
    truthy(b > 0);
    Physics.destroyBody(b);
});

// ---------------------------------------------------------------------------
// Pass-2 capability tests
// ---------------------------------------------------------------------------

t('Physics.destroyAll clears bodies and constraints', function() {
    var f = Physics.createBody({
        shape: 'box', static: true,
        position: { x: 0, y: -1, z: 0 },
        halfExtents: { x: 5, y: 1, z: 5 }
    });
    var a = Physics.createBody({ shape: 'sphere', radius: 0.3, position: {x:0,y:5,z:0} });
    var b = Physics.createBody({ shape: 'sphere', radius: 0.3, position: {x:1,y:5,z:0} });
    var c = Physics.createConstraint({
        type: 'distance', body1: a, body2: b,
        point1: {x:0,y:5,z:0}, point2: {x:1,y:5,z:0},
        minDistance: 0.5, maxDistance: 1.5
    });
    truthy(c > 0, 'constraint made');
    Physics.destroyAll();
    // World is reusable: create+step succeeds and old tags are gone.
    eq(Physics.getTransform(a), undefined, 'old tag invalid after destroyAll');
    var fresh = Physics.createBody({ shape: 'sphere', radius: 0.3, position: {x:0,y:5,z:0} });
    truthy(fresh > 0, 'world reusable after destroyAll');
    for (var i = 0; i < 30; i++) advanceTime(16);
    var ty = Physics.getTransform(fresh).position.y;
    truthy(ty < 5, 'gravity still works');
    Physics.destroyAll();
});

t('two-world isolation: handle world independent of default', function() {
    var w = Physics.createWorldHandle({ maxBodies: 64, gravity: {x:0,y:-9.81,z:0} });
    truthy(w, 'handle created');
    var floor = Physics.createBody({ shape: 'box', static: true,
        position: {x:0,y:-1,z:0}, halfExtents:{x:10,y:1,z:10} });
    var defBall = Physics.createBody({ shape: 'sphere', radius: 0.3,
        position: {x:0,y:5,z:0} });
    var sbBall = w.createBody({ shape: 'sphere', radius: 0.3, position: {x:0,y:5,z:0} });
    // Step default world via engine timer (advanceTime); sandbox via .step.
    for (var i = 0; i < 90; i++) {
        advanceTime(16);
        w.step(1/60);
    }
    var defY = Physics.getTransform(defBall).position.y;
    var sbY  = w.getTransform(sbBall).position.y;
    // Default ball lands on floor near 0; sandbox has no floor → falls below.
    truthy(defY > -1.0 && defY < 1.5, 'default ball landed: ' + defY);
    truthy(sbY < -2, 'sandbox ball fell free: ' + sbY);
    // destroyAll on sandbox does not touch default
    w.destroyAll();
    eq(w.getTransform(sbBall), undefined, 'sandbox tags gone after destroyAll');
    truthy(Physics.getTransform(defBall) !== undefined, 'default body intact');
    w.destroy();
    Physics.destroyBody(defBall);
    Physics.destroyBody(floor);
});

t('sandbox handle.destroyAll keeps world reusable', function() {
    var w = Physics.createWorldHandle({ maxBodies: 64 });
    var floor = w.createBody({ shape:'box', static:true, position:{x:0,y:-1,z:0},
        halfExtents:{x:10,y:1,z:10} });
    var ball = w.createBody({ shape:'sphere', radius:0.3, position:{x:0,y:5,z:0} });
    for (var i = 0; i < 60; i++) w.step(1/60);
    w.destroyAll();
    // Reuse: drop again
    var ball2 = w.createBody({ shape:'sphere', radius:0.3, position:{x:0,y:5,z:0} });
    truthy(ball2 > 0, 'created after destroyAll');
    var y0 = w.getTransform(ball2).position.y;
    for (var i = 0; i < 30; i++) w.step(1/60);
    var y1 = w.getTransform(ball2).position.y;
    truthy(y1 < y0, 'gravity active after destroyAll');
    w.destroy();
});

t('polyline (capsule segments): ball rests on it', function() {
    Physics.destroyAll();
    var lib = (typeof Physics2D !== 'undefined') ? Physics2D : null;
    // Build a flat horizontal polyline directly via Physics2D to exercise
    // the capsule-per-segment path. (Physics2D coords are canvas-style.)
    Physics2D.init({ width: 800, height: 600, gravity: 980 });
    var line = Physics2D.createPolyline([
        { x: 100, y: 400 }, { x: 700, y: 400 }
    ], { thickness: 6 });
    truthy(Array.isArray(line), 'returns array of segment tags');
    truthy(line.length >= 1, 'at least one segment');
    var ball = Physics2D.createCircle(400, 200, 12, { restitution: 0.2 });
    var prevY = -1, stableFrames = 0;
    for (var i = 0; i < 240; i++) {
        advanceTime(16);
        var p = Physics2D.getPosition(ball);
        if (p.y > 100 && Math.abs(p.y - prevY) < 0.5 && p.y < 410) stableFrames++;
        else stableFrames = 0;
        prevY = p.y;
    }
    truthy(stableFrames > 20, 'ball came to rest on polyline (stableFrames=' + stableFrames + ', y=' + prevY + ')');
    truthy(prevY < 410, 'ball above polyline: ' + prevY);
    Physics2D.destroyBody(ball);
    Physics2D.destroyBody(line);
    Physics.destroyAll();
});

t('contact events: one Added + one Removed per encounter (no Persisted)', function() {
    Physics.destroyAll();
    var floor = Physics.createBody({ shape:'box', static:true, position:{x:0,y:-1,z:0},
        halfExtents:{x:10,y:1,z:10} });
    var ball = Physics.createBody({ shape:'sphere', radius:0.3, position:{x:0,y:3,z:0},
        restitution: 0.0, linearDamping: 0.5 });
    Physics.getContacts();  // drain stale
    var added = 0, removed = 0;
    // Drop, settle.
    for (var i = 0; i < 60; i++) {
        advanceTime(16);
        var evs = Physics.getContacts();
        for (var j = 0; j < evs.length; j++) {
            if (evs[j].type === 'added') added++;
            else if (evs[j].type === 'removed') removed++;
        }
    }
    // Lift the ball away → expect one Removed.
    Physics.setPosition(ball, 100, 100, 0);
    Physics.setLinearVelocity(ball, 0, 0, 0);
    for (var i = 0; i < 30; i++) {
        advanceTime(16);
        var evs = Physics.getContacts();
        for (var j = 0; j < evs.length; j++) {
            if (evs[j].type === 'added') added++;
            else if (evs[j].type === 'removed') removed++;
        }
    }
    // Allow tiny bounce-induced re-contacts (<= 3) — what we DON'T want is
    // 60+ persistedevents per second. The old singleton + Persisted hookup
    // would have recorded ~60 events/sec while resting.
    truthy(added >= 1 && added <= 3, 'added events bounded (got ' + added + ')');
    truthy(removed >= 1 && removed <= 3, 'removed events bounded (got ' + removed + ')');
    Physics.destroyAll();
});

t('kinematic body pushes dynamic on contact', function() {
    Physics.destroyAll();
    var floor = Physics.createBody({ shape:'box', static:true, position:{x:0,y:-1,z:0},
        halfExtents:{x:20,y:1,z:20} });
    // Kinematic bar at x=-3, sliding in +X
    var bar = Physics.createBody({ shape:'box', position:{x:-3,y:1,z:0},
        halfExtents:{x:0.5,y:0.5,z:0.5} });
    Physics.setKinematic(bar);
    var ball = Physics.createBody({ shape:'sphere', radius:0.4,
        position:{x:0,y:1,z:0}, restitution: 0.0, friction: 0.5 });
    var startX = Physics.getTransform(ball).position.x;
    // Slide bar across by repeated moveKinematic; engine ticks at 1/60s.
    var dt = 1/60;
    for (var i = 0; i < 60; i++) {
        var bx = -3 + (i / 60) * 4;  // bar travels from -3 to ~+1
        Physics.moveKinematic(bar, bx, 1, 0, dt);
        advanceTime(16);
    }
    var endX = Physics.getTransform(ball).position.x;
    truthy(endX > startX + 0.5, 'kinematic bar pushed ball: ' + startX + ' -> ' + endX);
    Physics.destroyAll();
});

t('Physics.setLayer changes runtime collision layer', function() {
    Physics.destroyAll();
    Physics.setLayers({
        names: ['static', 'moving', 'ghost'],
        matrix: [
            false, true,  false,
            true,  true,  false,
            false, false, false,
        ]
    });
    var floor = Physics.createBody({ shape:'box', static:true, layer:'static',
        position:{x:0,y:-1,z:0}, halfExtents:{x:10,y:1,z:10} });
    var ball = Physics.createBody({ shape:'sphere', radius:0.3, layer:'moving',
        position:{x:0,y:5,z:0}, restitution: 0.0 });
    // Land on floor first
    for (var i = 0; i < 60; i++) advanceTime(16);
    var landedY = Physics.getTransform(ball).position.y;
    truthy(landedY > -1 && landedY < 1.5, 'landed on floor: ' + landedY);
    // Now switch ball to ghost layer → no longer collides; it should fall.
    var ok = Physics.setLayer(ball, 'ghost');
    truthy(ok, 'setLayer returned true');
    // Reactivate so it re-evaluates broadphase + gravity.
    Physics.setPosition(ball, 0, 5, 0);
    Physics.activate(ball);
    for (var i = 0; i < 60; i++) advanceTime(16);
    var endY = Physics.getTransform(ball).position.y;
    truthy(endY < -1, 'ghost ball passes through floor: ' + endY);
    Physics.destroyAll();
    Physics.setLayers({ names: ['static', 'moving'], matrix: [false, true, true, true] });
});

t('maxLinearVelocity binding lifts Jolt 500 m/s default', function() {
    // Regression: Jolt's BodyCreationSettings::mMaxLinearVelocity defaults to
    // 500. For pixel-unit games with high gravity, that clamp made the ball
    // appear to decelerate during a free fall. Confirm the binding lets us
    // raise the cap.
    Physics.destroyAll();
    Physics.setGravity(0, -1400, 0);  // px/s^2 style
    var ball = Physics.createBody({
        shape: 'sphere', radius: 9,
        position: { x: 0, y: 1000, z: 0 },
        linearDamping: 0,
        maxLinearVelocity: 2000,
    });
    Physics.setLinearVelocity(ball, 0, 0, 0);
    // Step 0.6s; expected |vy| ~ 840, well above the old 500 cap.
    for (var i = 0; i < 72; i++) advanceTime(16);  // ~1.15s
    var v = Physics.getVelocity(ball);
    truthy(Math.abs(v.linear.y) > 700,
           'free fall passes 500 cap (vy=' + v.linear.y.toFixed(1) + ')');
    Physics.destroyBody(ball);
    Physics.setGravity(0, -9.81, 0);
});

t('default maxLinearVelocity remains Jolt 500', function() {
    Physics.destroyAll();
    Physics.setGravity(0, -1400, 0);
    var ball = Physics.createBody({
        shape: 'sphere', radius: 9,
        position: { x: 0, y: 1000, z: 0 },
        linearDamping: 0,
    });
    Physics.setLinearVelocity(ball, 0, 0, 0);
    for (var i = 0; i < 72; i++) advanceTime(16);
    var v = Physics.getVelocity(ball);
    // Should clamp at ~500.
    truthy(Math.abs(v.linear.y) <= 501,
           'default cap holds (vy=' + v.linear.y.toFixed(1) + ')');
    Physics.destroyBody(ball);
    Physics.setGravity(0, -9.81, 0);
});

console.log('=== Done: ' + tests + ' tests, ' + failed + ' failures ===');
if (failed > 0) throw new Error(failed + ' physics tests failed');
