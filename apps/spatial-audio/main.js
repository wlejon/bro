// Spatial Audio Demo — procedural 3D audio shaped by environment zones
// Three.js scene with broaudio spatial sources, zone buses, and effect chains

// ───────────────────────── Three.js setup ─────────────────────────

var canvas = document.getElementById('c');
var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setSize(canvas.width, canvas.height, false);
renderer.shadowMap.enabled = true;

var scene = new THREE.Scene();
scene.background = new THREE.Color(0x1a1a2e);
scene.fog = new THREE.Fog(0x1a1a2e, 20, 60);

var camera = new THREE.PerspectiveCamera(75, canvas.width / canvas.height, 0.1, 200);
camera.position.set(0, 1.6, 8); // eye height, starting in forest

// Handle window resize
window.addEventListener('resize', function() {
    var w = window.innerWidth, h = window.innerHeight;
    canvas.width = w; canvas.height = h;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
});

var ambientLight = new THREE.AmbientLight(0x404060, 1.5);
scene.add(ambientLight);

var dirLight = new THREE.DirectionalLight(0xffeedd, 2.0);
dirLight.position.set(10, 20, 10);
scene.add(dirLight);

// ───────────────────────── Zone geometry ─────────────────────────

// Layout:  [Cave] ---bridge--- [Forest] ---bridge--- [Metal Hall]
//          x: -30..-12         x: -8..8               x: 12..30

var zoneDefs = {
    cave:   { minX: -30, maxX: -12, minZ: -10, maxZ: 10, color: 0x3a3a3a, label: 'Stone Cave' },
    forest: { minX:  -8, maxX:   8, minZ: -10, maxZ: 10, color: 0x2d5a27, label: 'Forest Clearing' },
    metal:  { minX:  12, maxX:  30, minZ: -10, maxZ: 10, color: 0x6a6a7a, label: 'Metal Hall' }
};

// Ground planes
function makeGround(zone, y) {
    var w = zone.maxX - zone.minX;
    var d = zone.maxZ - zone.minZ;
    var geo = new THREE.PlaneGeometry(w, d);
    var mat = new THREE.MeshStandardMaterial({ color: zone.color, roughness: 0.9 });
    var mesh = new THREE.Mesh(geo, mat);
    mesh.rotation.x = -Math.PI / 2;
    mesh.position.set((zone.minX + zone.maxX) / 2, y || 0, 0);
    mesh.receiveShadow = true;
    scene.add(mesh);
    return mesh;
}

makeGround(zoneDefs.cave, 0);
makeGround(zoneDefs.forest, 0);
makeGround(zoneDefs.metal, 0);

// Bridges between zones
function makeBridge(x1, x2) {
    var w = x2 - x1;
    var geo = new THREE.BoxGeometry(w, 0.15, 3);
    var mat = new THREE.MeshStandardMaterial({ color: 0x8a7a6a, roughness: 0.7 });
    var mesh = new THREE.Mesh(geo, mat);
    mesh.position.set((x1 + x2) / 2, 0, 0);
    scene.add(mesh);
}

makeBridge(-12, -8);
makeBridge(8, 12);

// Occluder list — walls/floors/ceilings that block sound
var occluders = [];

// Cave walls
function makeWall(x, z, w, h, d, color, yOff) {
    var geo = new THREE.BoxGeometry(w, h, d);
    var mat = new THREE.MeshStandardMaterial({ color: color, roughness: 1.0 });
    var mesh = new THREE.Mesh(geo, mat);
    mesh.position.set(x, (yOff || 0) + h / 2, z);
    scene.add(mesh);
    occluders.push(mesh);
    return mesh;
}

// Cave enclosure
makeWall(-21, -10, 18, 6, 0.5, 0x2a2a2a); // back wall
makeWall(-21,  10, 18, 6, 0.5, 0x2a2a2a); // front wall
makeWall(-30,   0, 0.5, 6, 20, 0x2a2a2a); // left wall
makeWall(-12,   0, 0.5, 6, 20, 0x333333); // right wall (opening)
// Ceiling (also an occluder — blocks sound from above)
var ceilGeo = new THREE.PlaneGeometry(18, 20);
var ceilMat = new THREE.MeshStandardMaterial({ color: 0x222222, roughness: 1.0, side: THREE.DoubleSide });
var ceil = new THREE.Mesh(ceilGeo, ceilMat);
ceil.rotation.x = Math.PI / 2;
ceil.position.set(-21, 6, 0);
scene.add(ceil);
occluders.push(ceil);

// Cave stalactites (vertical cones hanging from ceiling)
for (var i = 0; i < 12; i++) {
    var sx = -28 + Math.random() * 14;
    var sz = -8 + Math.random() * 16;
    var sh = 0.5 + Math.random() * 2;
    var sGeo = new THREE.ConeGeometry(0.15, sh, 5);
    var sMat = new THREE.MeshStandardMaterial({ color: 0x4a4a4a });
    var stalactite = new THREE.Mesh(sGeo, sMat);
    stalactite.rotation.x = Math.PI;
    stalactite.position.set(sx, 6 - sh / 2, sz);
    scene.add(stalactite);
}

// Forest trees (cylinders + spheres)
for (var i = 0; i < 20; i++) {
    var tx = -7 + Math.random() * 14;
    var tz = -9 + Math.random() * 18;
    // Avoid center path
    if (Math.abs(tz) < 2) continue;
    var th = 3 + Math.random() * 3;
    var trunk = new THREE.Mesh(
        new THREE.CylinderGeometry(0.15, 0.2, th, 6),
        new THREE.MeshStandardMaterial({ color: 0x5a3a1a })
    );
    trunk.position.set(tx, th / 2, tz);
    scene.add(trunk);

    var foliage = new THREE.Mesh(
        new THREE.SphereGeometry(0.8 + Math.random() * 0.6, 6, 5),
        new THREE.MeshStandardMaterial({ color: 0x2a6a1a + Math.floor(Math.random() * 0x102010) })
    );
    foliage.position.set(tx, th + 0.5, tz);
    scene.add(foliage);
}

// Metal hall — reflective walls and pillars
makeWall(21, -10, 18, 8, 0.3, 0x5a5a6a);
makeWall(21,  10, 18, 8, 0.3, 0x5a5a6a);
makeWall(30,   0, 0.3, 8, 20, 0x5a5a6a);

for (var i = 0; i < 6; i++) {
    var px = 14 + i * 3;
    var pillar = new THREE.Mesh(
        new THREE.CylinderGeometry(0.3, 0.3, 8, 8),
        new THREE.MeshStandardMaterial({ color: 0x8888aa, metalness: 0.8, roughness: 0.2 })
    );
    pillar.position.set(px, 4, -9);
    scene.add(pillar);
    var p2 = pillar.clone();
    p2.position.z = 9;
    scene.add(p2);
}

// Metal hall ceiling
var mCeil = new THREE.Mesh(
    new THREE.PlaneGeometry(18, 20),
    new THREE.MeshStandardMaterial({ color: 0x4a4a5a, metalness: 0.6, roughness: 0.3, side: THREE.DoubleSide })
);
mCeil.rotation.x = Math.PI / 2;
mCeil.position.set(21, 8, 0);
scene.add(mCeil);
occluders.push(mCeil);

// ───────────────────────── Upper level: Sky Platform ─────────────────────────
// Floating platform above the forest with a crystalline audio source

var skyPlatGeo = new THREE.BoxGeometry(8, 0.3, 8);
var skyPlatMat = new THREE.MeshStandardMaterial({ color: 0x6688aa, metalness: 0.3, roughness: 0.5 });
var skyPlatform = new THREE.Mesh(skyPlatGeo, skyPlatMat);
skyPlatform.position.set(0, 8, 0);
scene.add(skyPlatform);
occluders.push(skyPlatform);

// Pillars holding up the sky platform
for (var i = 0; i < 4; i++) {
    var cx = (i % 2 === 0 ? -3 : 3);
    var cz = (i < 2 ? -3 : 3);
    var col = new THREE.Mesh(
        new THREE.CylinderGeometry(0.2, 0.2, 8, 6),
        new THREE.MeshStandardMaterial({ color: 0x556677 })
    );
    col.position.set(cx, 4, cz);
    scene.add(col);
}

// Crystal emitter on the sky platform
var crystalMarker = makeEmitter(0, 9, 0, 0xee88ff);

// Sky platform light
var skyLight = new THREE.PointLight(0xcc88ff, 3, 15);
skyLight.position.set(0, 9.5, 0);
scene.add(skyLight);

// ───────────────────────── Lower level: Underground Pool ─────────────────────────
// Below the forest, accessed by descending — a water-filled cavern

// Underground chamber floor
var underFloor = new THREE.Mesh(
    new THREE.PlaneGeometry(14, 14),
    new THREE.MeshStandardMaterial({ color: 0x1a2a3a, roughness: 0.3 })
);
underFloor.rotation.x = -Math.PI / 2;
underFloor.position.set(0, -4, 0);
scene.add(underFloor);

// Water surface (translucent)
var waterGeo = new THREE.PlaneGeometry(10, 10);
var waterMat = new THREE.MeshStandardMaterial({
    color: 0x2244aa, roughness: 0.1, metalness: 0.3,
    transparent: true, opacity: 0.6, side: THREE.DoubleSide
});
var waterSurface = new THREE.Mesh(waterGeo, waterMat);
waterSurface.rotation.x = -Math.PI / 2;
waterSurface.position.set(0, -2, 0);
scene.add(waterSurface);

// Underground walls
makeWall(0, -7, 14, 4, 0.3, 0x1a1a2a, -4);
makeWall(0,  7, 14, 4, 0.3, 0x1a1a2a, -4);
makeWall(-7, 0, 0.3, 4, 14, 0x1a1a2a, -4);
makeWall( 7, 0, 0.3, 4, 14, 0x1a1a2a, -4);

// Underground emitter — deep resonant pool
var poolMarker = makeEmitter(0, -3, 0, 0x2266ff);

// Underground light
var underLight = new THREE.PointLight(0x2255cc, 3, 12);
underLight.position.set(0, -2.5, 0);
scene.add(underLight);

// Point light inside cave (flickering torch feel)
var caveLight = new THREE.PointLight(0xff8844, 3, 15);
caveLight.position.set(-22, 3, 0);
scene.add(caveLight);

// Forest has more ambient + sky contribution
var forestLight = new THREE.PointLight(0x88ccff, 1, 20);
forestLight.position.set(0, 10, 0);
scene.add(forestLight);

// Metal hall has cold fluorescent feel
var metalLight = new THREE.PointLight(0xccddff, 4, 25);
metalLight.position.set(21, 7, 0);
scene.add(metalLight);

// Visible point-source markers (small glowing spheres for audio emitters)
function makeEmitter(x, y, z, color) {
    var geo = new THREE.SphereGeometry(0.15, 8, 6);
    var mat = new THREE.MeshBasicMaterial({ color: color });
    var mesh = new THREE.Mesh(geo, mat);
    mesh.position.set(x, y, z);
    scene.add(mesh);
    // Add a point light for visibility
    var light = new THREE.PointLight(color, 0.5, 5);
    light.position.set(x, y, z);
    scene.add(light);
    return mesh;
}

// Water drip emitter in cave
var dripMarker = makeEmitter(-24, 3.5, -3, 0x4488ff);

// Wind emitter in forest
var windMarker = makeEmitter(2, 2, 5, 0x88ffaa);

// Hum emitter in metal hall
var humMarker = makeEmitter(22, 2, 0, 0xffaa44);

// ───────────────────────── Camera controls ─────────────────────────

var yaw = 0, pitch = 0;
var keys = {};
var mouseDown = false;

document.addEventListener('keydown', function(e) {
    keys[e.key.toLowerCase()] = true;
    if (e.key === ' ' || e.key === 'Control') keys[e.key] = true;
    if (e.key === ' ') e.preventDefault();
});
document.addEventListener('keyup', function(e) {
    keys[e.key.toLowerCase()] = false;
    if (e.key === ' ' || e.key === 'Control') keys[e.key] = false;
});
document.addEventListener('mousedown', function(e) { mouseDown = true; });
document.addEventListener('mouseup', function(e) { mouseDown = false; });
document.addEventListener('mousemove', function(e) {
    if (!mouseDown) return;
    yaw   -= e.movementX * 0.003;
    pitch -= e.movementY * 0.003;
    pitch = Math.max(-1.2, Math.min(1.2, pitch));
});

var moveSpeed = 0.025;
var isMoving = false;

function updateCamera() {
    var fwd = new THREE.Vector3(Math.sin(yaw), 0, Math.cos(yaw));
    var right = new THREE.Vector3(-fwd.z, 0, fwd.x);

    isMoving = false;
    if (keys['w']) { camera.position.addScaledVector(fwd, moveSpeed); isMoving = true; }
    if (keys['s']) { camera.position.addScaledVector(fwd, -moveSpeed); isMoving = true; }
    if (keys['a']) { camera.position.addScaledVector(right, -moveSpeed); isMoving = true; }
    if (keys['d']) { camera.position.addScaledVector(right, moveSpeed); isMoving = true; }
    if (keys[' ']) { camera.position.y += moveSpeed; isMoving = true; }
    if (keys['Control']) { camera.position.y -= moveSpeed; isMoving = true; }

    // Keep player in bounds
    camera.position.x = Math.max(-29, Math.min(29, camera.position.x));
    camera.position.z = Math.max(-9, Math.min(9, camera.position.z));
    camera.position.y = Math.max(-3, Math.min(12, camera.position.y));

    // Apply look
    var lookTarget = new THREE.Vector3(
        camera.position.x + Math.sin(yaw) * Math.cos(pitch),
        camera.position.y + Math.sin(pitch),
        camera.position.z + Math.cos(yaw) * Math.cos(pitch)
    );
    camera.lookAt(lookTarget);
}

// ───────────────────────── Zone detection ─────────────────────────

function getZone(x) {
    if (x >= zoneDefs.cave.minX && x <= zoneDefs.cave.maxX) return 'cave';
    if (x >= zoneDefs.forest.minX && x <= zoneDefs.forest.maxX) return 'forest';
    if (x >= zoneDefs.metal.minX && x <= zoneDefs.metal.maxX) return 'metal';
    // On a bridge — blend based on proximity
    if (x > zoneDefs.cave.maxX && x < zoneDefs.forest.minX) return 'bridge_cave_forest';
    if (x > zoneDefs.forest.maxX && x < zoneDefs.metal.minX) return 'bridge_forest_metal';
    return 'forest';
}

// Returns blend weights {cave, forest, metal} based on position
function getZoneWeights(x) {
    var w = { cave: 0, forest: 0, metal: 0 };

    if (x <= -12) { w.cave = 1; }
    else if (x <= -8) {
        var t = (x - (-12)) / 4; // 0 at cave edge, 1 at forest edge
        w.cave = 1 - t; w.forest = t;
    }
    else if (x <= 8) { w.forest = 1; }
    else if (x <= 12) {
        var t = (x - 8) / 4;
        w.forest = 1 - t; w.metal = t;
    }
    else { w.metal = 1; }

    return w;
}

// ───────────────────────── Audio setup ─────────────────────────

var audioCtx = new AudioContext();
audioCtx.masterGain = 1.0;

// Create zone buses
var caveBus = audioCtx.createBus();
var forestBus = audioCtx.createBus();
var metalBus = audioCtx.createBus();

// Cave: heavy reverb + lowpass filter
audioCtx.setBusReverbEnabled(caveBus, true);
audioCtx.setBusReverbRoomSize(caveBus, 0.95);
audioCtx.setBusReverbDamping(caveBus, 0.3);
audioCtx.setBusReverbMix(caveBus, 0.6);
var caveFilterSlot = audioCtx.allocateBusFilterSlot(caveBus);
audioCtx.setBusFilterEnabled(caveBus, caveFilterSlot, true);
audioCtx.setBusFilterType(caveBus, caveFilterSlot, 'lowpass');
audioCtx.setBusFilterFrequency(caveBus, caveFilterSlot, 1200);
audioCtx.setBusFilterQ(caveBus, caveFilterSlot, 0.7);

// Forest: short delay + EQ with rolled-off highs
audioCtx.setBusDelayEnabled(forestBus, true);
audioCtx.setBusDelayTime(forestBus, 0.08);
audioCtx.setBusDelayFeedback(forestBus, 0.15);
audioCtx.setBusDelayMix(forestBus, 0.25);
audioCtx.setBusEqEnabled(forestBus, true);
audioCtx.setBusEqBandGain(forestBus, 0, 1.0);   // low: slight boost
audioCtx.setBusEqBandGain(forestBus, 1, 0.0);   // mid: flat
audioCtx.setBusEqBandGain(forestBus, 2, -4.0);  // high: cut

// Metal hall: bright reverb + subtle chorus
audioCtx.setBusReverbEnabled(metalBus, true);
audioCtx.setBusReverbRoomSize(metalBus, 0.4);
audioCtx.setBusReverbDamping(metalBus, 0.1);
audioCtx.setBusReverbMix(metalBus, 0.45);
audioCtx.setBusChorusEnabled(metalBus, true);
audioCtx.setBusChorusRate(metalBus, 0.8);
audioCtx.setBusChorusDepth(metalBus, 0.003);
audioCtx.setBusChorusMix(metalBus, 0.2);
audioCtx.setBusChorusFeedback(metalBus, 0.1);
audioCtx.setBusChorusBaseDelay(metalBus, 0.007);

// ── Footstep voice ──
// Noise burst with short ADSR, re-triggered on movement
var footstepVoice = audioCtx.createVoice();
audioCtx.setVoiceWaveform(footstepVoice, 'whitenoise');
audioCtx.setVoiceGain(footstepVoice, 3.0);
audioCtx.setVoiceAttack(footstepVoice, 0.005);
audioCtx.setVoiceDecay(footstepVoice, 0.06);
audioCtx.setVoiceSustain(footstepVoice, 0.0);
audioCtx.setVoiceRelease(footstepVoice, 0.04);
audioCtx.setVoiceFilterEnabled(footstepVoice, true);

var footstepLastTime = 0;
var footstepInterval = 0.35; // seconds between steps

function triggerFootstep(zone) {
    var now = audioCtx.currentTime;
    if (now - footstepLastTime < footstepInterval) return;
    footstepLastTime = now;

    // Material-based filter
    if (zone === 'cave' || zone === 'bridge_cave_forest') {
        audioCtx.setVoiceFilterType(footstepVoice, 'lowpass');
        audioCtx.setVoiceFilterFrequency(footstepVoice, 800);
        audioCtx.setVoiceBus(footstepVoice, caveBus);
    } else if (zone === 'metal' || zone === 'bridge_forest_metal') {
        audioCtx.setVoiceFilterType(footstepVoice, 'highpass');
        audioCtx.setVoiceFilterFrequency(footstepVoice, 2000);
        audioCtx.setVoiceBus(footstepVoice, metalBus);
    } else {
        audioCtx.setVoiceFilterType(footstepVoice, 'bandpass');
        audioCtx.setVoiceFilterFrequency(footstepVoice, 1200);
        audioCtx.setVoiceBus(footstepVoice, forestBus);
    }

    audioCtx.startVoice(footstepVoice, now);
    audioCtx.stopVoice(footstepVoice, now + 0.12);
}

// ── Ambient drones (one per zone, spatial) ──

function makeDrone(freq, x, y, z, busId, gain) {
    var v = audioCtx.createVoice();
    audioCtx.setVoiceWaveform(v, 'sine');
    audioCtx.setVoiceFrequency(v, freq);
    audioCtx.setVoiceGain(v, gain || 1.0);
    audioCtx.setVoiceAttack(v, 0.5);
    audioCtx.setVoiceDecay(v, 0.1);
    audioCtx.setVoiceSustain(v, 1.0);
    audioCtx.setVoiceRelease(v, 1.0);
    audioCtx.setVoicePersistent(v, true);
    audioCtx.setVoiceBus(v, busId);

    // Spatial positioning
    audioCtx.setVoiceSpatialEnabled(v, true);
    audioCtx.setVoiceSpatialPosition(v, x, y, z);
    audioCtx.setVoiceSpatialRefDistance(v, 3.0);
    audioCtx.setVoiceSpatialMaxDistance(v, 40.0);
    audioCtx.setVoiceSpatialRolloff(v, 1.0);

    audioCtx.startVoice(v, audioCtx.currentTime);
    return v;
}

// Cave drone — deep rumble
var caveDrone = makeDrone(60, -21, 2, 0, caveBus, 1.5);

// Forest drones — layered natural tones
var forestDrone1 = makeDrone(180, 0, 3, 0, forestBus, 0.6);
var forestDrone2 = makeDrone(220, -2, 2, 4, forestBus, 0.5);

// Metal hall — slightly detuned pair for beating
var metalDrone1 = makeDrone(150, 21, 3, 0, metalBus, 0.8);
var metalDrone2 = makeDrone(150.8, 21, 3, 2, metalBus, 0.8);

// ── Point sources ──

// Water drip in cave — periodic noise burst with bandpass
var dripVoice = audioCtx.createVoice();
audioCtx.setVoiceWaveform(dripVoice, 'whitenoise');
audioCtx.setVoiceGain(dripVoice, 2.0);
audioCtx.setVoiceAttack(dripVoice, 0.002);
audioCtx.setVoiceDecay(dripVoice, 0.04);
audioCtx.setVoiceSustain(dripVoice, 0.0);
audioCtx.setVoiceRelease(dripVoice, 0.08);
audioCtx.setVoiceFilterEnabled(dripVoice, true);
audioCtx.setVoiceFilterType(dripVoice, 'bandpass');
audioCtx.setVoiceFilterFrequency(dripVoice, 3000);
audioCtx.setVoiceFilterQ(dripVoice, 5);
audioCtx.setVoiceBus(dripVoice, caveBus);
audioCtx.setVoiceSpatialEnabled(dripVoice, true);
audioCtx.setVoiceSpatialPosition(dripVoice, -24, 3.5, -3);
audioCtx.setVoiceSpatialRefDistance(dripVoice, 1.0);
audioCtx.setVoiceSpatialMaxDistance(dripVoice, 25.0);
audioCtx.setVoiceSpatialRolloff(dripVoice, 1.5);

var dripLastTime = 0;
var dripNextInterval = 0.8;

function triggerDrip() {
    var now = audioCtx.currentTime;
    if (now - dripLastTime < dripNextInterval) return;
    dripLastTime = now;
    dripNextInterval = 0.6 + Math.random() * 1.2; // random timing
    // Vary pitch slightly
    audioCtx.setVoiceFilterFrequency(dripVoice, 2500 + Math.random() * 1500);
    audioCtx.startVoice(dripVoice, now);
    audioCtx.stopVoice(dripVoice, now + 0.1);
}

// Wind in forest — filtered noise, continuous with modulated filter
var windVoice = audioCtx.createVoice();
audioCtx.setVoiceWaveform(windVoice, 'pinknoise');
audioCtx.setVoiceGain(windVoice, 1.0);
audioCtx.setVoiceAttack(windVoice, 1.0);
audioCtx.setVoiceDecay(windVoice, 0.5);
audioCtx.setVoiceSustain(windVoice, 1.0);
audioCtx.setVoiceRelease(windVoice, 2.0);
audioCtx.setVoiceFilterEnabled(windVoice, true);
audioCtx.setVoiceFilterType(windVoice, 'lowpass');
audioCtx.setVoiceFilterFrequency(windVoice, 800);
audioCtx.setVoicePersistent(windVoice, true);
audioCtx.setVoiceBus(windVoice, forestBus);
audioCtx.setVoiceSpatialEnabled(windVoice, true);
audioCtx.setVoiceSpatialPosition(windVoice, 2, 2, 5);
audioCtx.setVoiceSpatialRefDistance(windVoice, 2.0);
audioCtx.setVoiceSpatialMaxDistance(windVoice, 30.0);
audioCtx.startVoice(windVoice, audioCtx.currentTime);

var windPhase = 0;

// Metal hum — resonant tone
var humVoice = audioCtx.createVoice();
audioCtx.setVoiceWaveform(humVoice, 'sawtooth');
audioCtx.setVoiceFrequency(humVoice, 100);
audioCtx.setVoiceGain(humVoice, 0.8);
audioCtx.setVoiceAttack(humVoice, 0.3);
audioCtx.setVoiceDecay(humVoice, 0.2);
audioCtx.setVoiceSustain(humVoice, 1.0);
audioCtx.setVoiceRelease(humVoice, 1.0);
audioCtx.setVoiceFilterEnabled(humVoice, true);
audioCtx.setVoiceFilterType(humVoice, 'lowpass');
audioCtx.setVoiceFilterFrequency(humVoice, 400);
audioCtx.setVoiceFilterQ(humVoice, 4);
audioCtx.setVoicePersistent(humVoice, true);
audioCtx.setVoiceBus(humVoice, metalBus);
audioCtx.setVoiceSpatialEnabled(humVoice, true);
audioCtx.setVoiceSpatialPosition(humVoice, 22, 2, 0);
audioCtx.setVoiceSpatialRefDistance(humVoice, 2.0);
audioCtx.setVoiceSpatialMaxDistance(humVoice, 30.0);
audioCtx.startVoice(humVoice, audioCtx.currentTime);

// ── Sky crystal — high shimmering tone above ──
var crystalVoice = audioCtx.createVoice();
audioCtx.setVoiceWaveform(crystalVoice, 'sine');
audioCtx.setVoiceFrequency(crystalVoice, 880);
audioCtx.setVoiceGain(crystalVoice, 0.6);
audioCtx.setVoiceAttack(crystalVoice, 0.5);
audioCtx.setVoiceDecay(crystalVoice, 0.3);
audioCtx.setVoiceSustain(crystalVoice, 1.0);
audioCtx.setVoiceRelease(crystalVoice, 1.0);
audioCtx.setVoiceFilterEnabled(crystalVoice, true);
audioCtx.setVoiceFilterType(crystalVoice, 'bandpass');
audioCtx.setVoiceFilterFrequency(crystalVoice, 900);
audioCtx.setVoiceFilterQ(crystalVoice, 3);
audioCtx.setVoicePersistent(crystalVoice, true);
audioCtx.setVoiceBus(crystalVoice, forestBus);
audioCtx.setVoiceSpatialEnabled(crystalVoice, true);
audioCtx.setVoiceSpatialPosition(crystalVoice, 0, 9, 0);
audioCtx.setVoiceSpatialRefDistance(crystalVoice, 2.0);
audioCtx.setVoiceSpatialMaxDistance(crystalVoice, 35.0);
audioCtx.startVoice(crystalVoice, audioCtx.currentTime);

// Second crystal — detuned for shimmer
var crystalVoice2 = audioCtx.createVoice();
audioCtx.setVoiceWaveform(crystalVoice2, 'sine');
audioCtx.setVoiceFrequency(crystalVoice2, 882.5);
audioCtx.setVoiceGain(crystalVoice2, 0.5);
audioCtx.setVoiceAttack(crystalVoice2, 0.5);
audioCtx.setVoiceDecay(crystalVoice2, 0.3);
audioCtx.setVoiceSustain(crystalVoice2, 1.0);
audioCtx.setVoiceRelease(crystalVoice2, 1.0);
audioCtx.setVoicePersistent(crystalVoice2, true);
audioCtx.setVoiceBus(crystalVoice2, forestBus);
audioCtx.setVoiceSpatialEnabled(crystalVoice2, true);
audioCtx.setVoiceSpatialPosition(crystalVoice2, 0, 9, 0);
audioCtx.setVoiceSpatialRefDistance(crystalVoice2, 2.0);
audioCtx.setVoiceSpatialMaxDistance(crystalVoice2, 35.0);
audioCtx.startVoice(crystalVoice2, audioCtx.currentTime);

// ── Underground pool — deep resonant rumble below ──
var poolDrone = audioCtx.createVoice();
audioCtx.setVoiceWaveform(poolDrone, 'sine');
audioCtx.setVoiceFrequency(poolDrone, 45);
audioCtx.setVoiceGain(poolDrone, 1.2);
audioCtx.setVoiceAttack(poolDrone, 1.0);
audioCtx.setVoiceDecay(poolDrone, 0.5);
audioCtx.setVoiceSustain(poolDrone, 1.0);
audioCtx.setVoiceRelease(poolDrone, 2.0);
audioCtx.setVoicePersistent(poolDrone, true);
audioCtx.setVoiceBus(poolDrone, caveBus);
audioCtx.setVoiceSpatialEnabled(poolDrone, true);
audioCtx.setVoiceSpatialPosition(poolDrone, 0, -3, 0);
audioCtx.setVoiceSpatialRefDistance(poolDrone, 2.0);
audioCtx.setVoiceSpatialMaxDistance(poolDrone, 30.0);
audioCtx.startVoice(poolDrone, audioCtx.currentTime);

// Pool bubbles — periodic noise bursts from below
var bubbleVoice = audioCtx.createVoice();
audioCtx.setVoiceWaveform(bubbleVoice, 'whitenoise');
audioCtx.setVoiceGain(bubbleVoice, 1.5);
audioCtx.setVoiceAttack(bubbleVoice, 0.003);
audioCtx.setVoiceDecay(bubbleVoice, 0.05);
audioCtx.setVoiceSustain(bubbleVoice, 0.0);
audioCtx.setVoiceRelease(bubbleVoice, 0.1);
audioCtx.setVoiceFilterEnabled(bubbleVoice, true);
audioCtx.setVoiceFilterType(bubbleVoice, 'bandpass');
audioCtx.setVoiceFilterFrequency(bubbleVoice, 1800);
audioCtx.setVoiceFilterQ(bubbleVoice, 6);
audioCtx.setVoiceBus(bubbleVoice, caveBus);
audioCtx.setVoiceSpatialEnabled(bubbleVoice, true);
audioCtx.setVoiceSpatialPosition(bubbleVoice, 0, -3, 0);
audioCtx.setVoiceSpatialRefDistance(bubbleVoice, 1.5);
audioCtx.setVoiceSpatialMaxDistance(bubbleVoice, 25.0);

var bubbleLastTime = 0;
var bubbleNextInterval = 0.5;

function triggerBubble() {
    var now = audioCtx.currentTime;
    if (now - bubbleLastTime < bubbleNextInterval) return;
    bubbleLastTime = now;
    bubbleNextInterval = 0.3 + Math.random() * 0.8;
    audioCtx.setVoiceFilterFrequency(bubbleVoice, 1200 + Math.random() * 2000);
    audioCtx.startVoice(bubbleVoice, now);
    audioCtx.stopVoice(bubbleVoice, now + 0.08);
}

// ───────────────────────── Occlusion raycaster ─────────────────────────
// Cast a ray from listener to each spatial source. If an occluder is hit,
// reduce that voice's gain and apply a lowpass filter to simulate blocking.

var raycaster = new THREE.Raycaster();
var occlusionDir = new THREE.Vector3();

// Spatial sources with their base gains and voice IDs for occlusion
var spatialSources = [
    { voice: caveDrone,    pos: new THREE.Vector3(-21, 2, 0),  baseGain: 1.5 },
    { voice: forestDrone1, pos: new THREE.Vector3(0, 3, 0),    baseGain: 0.6 },
    { voice: forestDrone2, pos: new THREE.Vector3(-2, 2, 4),   baseGain: 0.5 },
    { voice: metalDrone1,  pos: new THREE.Vector3(21, 3, 0),   baseGain: 0.8 },
    { voice: metalDrone2,  pos: new THREE.Vector3(21, 3, 2),   baseGain: 0.8 },
    { voice: dripVoice,    pos: new THREE.Vector3(-24, 3.5, -3), baseGain: 2.0 },
    { voice: windVoice,    pos: new THREE.Vector3(2, 2, 5),    baseGain: 1.0 },
    { voice: humVoice,     pos: new THREE.Vector3(22, 2, 0),   baseGain: 0.8 },
    { voice: crystalVoice, pos: new THREE.Vector3(0, 9, 0),    baseGain: 0.6 },
    { voice: crystalVoice2,pos: new THREE.Vector3(0, 9, 0),    baseGain: 0.5 },
    { voice: poolDrone,    pos: new THREE.Vector3(0, -3, 0),   baseGain: 1.2 },
    { voice: bubbleVoice,  pos: new THREE.Vector3(0, -3, 0),   baseGain: 1.5 }
];

function updateOcclusion() {
    var listenerPos = camera.position;

    for (var i = 0; i < spatialSources.length; i++) {
        var src = spatialSources[i];
        occlusionDir.copy(src.pos).sub(listenerPos);
        var dist = occlusionDir.length();
        if (dist < 0.1) continue;
        occlusionDir.normalize();

        raycaster.set(listenerPos, occlusionDir);
        raycaster.far = dist;
        var hits = raycaster.intersectObjects(occluders);

        if (hits.length > 0) {
            // Occluded: reduce gain and muffle
            var factor = Math.max(0.05, 1.0 - hits.length * 0.6);
            audioCtx.setVoiceGain(src.voice, src.baseGain * factor);
        } else {
            // Clear line of sight: full gain
            audioCtx.setVoiceGain(src.voice, src.baseGain);
        }
    }
}

// ───────────────────────── HUD ─────────────────────────

var zoneLabel = document.getElementById('zone-label');
var posLabel = document.getElementById('pos-label');
var currentZoneLabel = '---';

var zoneColors = {
    cave: '#8888aa',
    forest: '#66cc66',
    metal: '#ccccee',
    bridge_cave_forest: '#7799aa',
    bridge_forest_metal: '#99bbaa'
};

// ───────────────────────── Main loop ─────────────────────────

var frameCount = 0;

function animate() {
    requestAnimationFrame(animate);
    frameCount++;

    updateCamera();

    // Detect zone
    var zone = getZone(camera.position.x);
    var weights = getZoneWeights(camera.position.x);

    // Update zone bus gains based on proximity (cross-fade)
    audioCtx.setBusGain(caveBus, weights.cave);
    audioCtx.setBusGain(forestBus, weights.forest);
    audioCtx.setBusGain(metalBus, weights.metal);

    // Sync listener to camera
    var camDir = new THREE.Vector3();
    camera.getWorldDirection(camDir);
    audioCtx.setListenerPosition(camera.position.x, camera.position.y, camera.position.z);
    audioCtx.setListenerOrientation(camDir.x, camDir.y, camDir.z, 0, 1, 0);

    // Footsteps
    if (isMoving) {
        triggerFootstep(zone);
    }

    // Water drip (always ticking)
    triggerDrip();

    // Pool bubbles
    triggerBubble();

    // Modulate wind filter for organic feel
    windPhase += 0.02;
    var windFreq = 600 + Math.sin(windPhase) * 300 + Math.sin(windPhase * 0.3) * 200;
    audioCtx.setVoiceFilterFrequency(windVoice, windFreq);

    // Occlusion check (every 3 frames for perf)
    if (frameCount % 3 === 0) {
        updateOcclusion();
    }

    // Pulse emitter markers
    var pulse = 0.8 + Math.sin(frameCount * 0.05) * 0.2;
    dripMarker.material.opacity = pulse;
    windMarker.material.opacity = pulse;
    humMarker.material.opacity = pulse;
    crystalMarker.material.opacity = pulse;
    poolMarker.material.opacity = pulse;

    // Flicker cave light
    caveLight.intensity = 2.5 + Math.sin(frameCount * 0.13) * 0.8 + Math.sin(frameCount * 0.37) * 0.5;

    // Pulse sky light
    skyLight.intensity = 2.5 + Math.sin(frameCount * 0.03) * 0.8;

    // Wobble water surface
    waterSurface.position.y = -2 + Math.sin(frameCount * 0.02) * 0.05;

    // HUD update (every 5 frames for perf)
    if (frameCount % 5 === 0) {
        var label = zoneDefs[zone] ? zoneDefs[zone].label : zone.replace('bridge_', 'Bridge: ').replace('_', ' / ');
        if (label !== currentZoneLabel) {
            currentZoneLabel = label;
            zoneLabel.textContent = label;
            zoneLabel.style.color = zoneColors[zone] || '#fff';
        }
        posLabel.textContent = camera.position.x.toFixed(1) + ', ' +
                               camera.position.y.toFixed(1) + ', ' +
                               camera.position.z.toFixed(1);
    }

    renderer.render(scene, camera);
}

animate();

// ───────────────────────── Head Model Panel ─────────────────────────

var panel = document.getElementById('panel');
var panelToggle = document.getElementById('panel-toggle');
var panelVisible = true;

function bindSlider(id, fn) {
    var slider = document.getElementById(id);
    var valEl = document.getElementById(id + '-val');
    slider.addEventListener('input', function() {
        var v = parseFloat(slider.value);
        valEl.textContent = v;
        fn(v);
    });
}

// Enabled toggle
document.getElementById('hm-enabled').addEventListener('change', function() {
    audioCtx.setHeadModelEnabled(this.checked);
});

// ILD
bindSlider('hm-ild', function(v) { audioCtx.setHeadModelIldStrength(v); });

// Behind
bindSlider('hm-behind', function(v) { audioCtx.setHeadModelBehindAttenuation(v); });

// Near cutoff (paired — send both values together)
function updateNearCutoff() {
    var front = parseFloat(document.getElementById('hm-near-front').value);
    var behind = parseFloat(document.getElementById('hm-near-behind').value);
    audioCtx.setHeadModelNearCutoff(front, behind);
}
bindSlider('hm-near-front', updateNearCutoff);
bindSlider('hm-near-behind', updateNearCutoff);

// Far ratio
bindSlider('hm-far-ratio', function(v) { audioCtx.setHeadModelFarCutoffRatio(v); });

// Elevation
function updateElevation() {
    var n = parseFloat(document.getElementById('hm-elev-near').value);
    var f = parseFloat(document.getElementById('hm-elev-far').value);
    audioCtx.setHeadModelElevation(n, f);
}
bindSlider('hm-elev-near', updateElevation);
bindSlider('hm-elev-far', updateElevation);

// Cutoff range
function updateCutoffRange() {
    var min = parseFloat(document.getElementById('hm-min-cut').value);
    var max = parseFloat(document.getElementById('hm-max-cut').value);
    audioCtx.setHeadModelCutoffRange(min, max);
}
bindSlider('hm-min-cut', updateCutoffRange);
bindSlider('hm-max-cut', updateCutoffRange);

// Toggle panel with H key
document.addEventListener('keydown', function(e) {
    if (e.key === 'h' || e.key === 'H') {
        panelVisible = !panelVisible;
        panel.style.display = panelVisible ? 'block' : 'none';
        panelToggle.style.display = panelVisible ? 'none' : 'block';
    }
});
panelToggle.addEventListener('click', function() {
    panelVisible = true;
    panel.style.display = 'block';
    panelToggle.style.display = 'none';
});

console.log('[spatial-audio] Demo running. WASD to move, mouse drag to look. H to toggle panel.');
