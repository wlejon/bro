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

console.log('=== Done: ' + tests + ' tests, ' + failed + ' failures ===');
if (failed > 0) throw new Error(failed + ' physics tests failed');
