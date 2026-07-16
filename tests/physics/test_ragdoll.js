// Test ragdolls — Physics.createRagdoll (Jolt Ragdoll + Skeleton): a
// humanoid-ish part tree joined by swing-twist constraints. Covers fall &
// settle (connected, on the ground, joint limits hold), part-body interop
// (impulses + raycasts against regular body tags), pose readout/teleport,
// motor drive toward a target pose (DriveToPoseUsingMotors) and the
// kinematic variant, sandbox-world form, destroy mid-sim (handle and
// part-body paths), the skinned-mesh integration recipe end-to-end
// (localPose -> bromesh Pose -> setSkinningMatrices, verified numerically
// and in pixels), and GC teardown without explicit destroy (worldRef
// gc_mark + ~JsWorld severing — the Debug leak assert is the gate).
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.createRagdoll === 'function', 'createRagdoll exists');

Physics.destroyAll();

const DEG = Math.PI / 180;
const RZM90 = { x: 0, y: 0, z: -Math.SQRT1_2, w: Math.SQRT1_2 }; // Rz(-90°): +Y -> +X

// --- tiny quaternion helpers (arrays [x,y,z,w]) ---
function qconj(q) { return [-q[0], -q[1], -q[2], q[3]]; }
function qmul(a, b) {
    return [
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    ];
}
function qangle(a, b) {   // rotation angle between two quats (rad)
    const d = Math.abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * Math.acos(Math.min(1, d));
}
function partQ(pose, i) { return [pose[i * 7 + 3], pose[i * 7 + 4], pose[i * 7 + 5], pose[i * 7 + 6]]; }
function partP(pose, i) { return [pose[i * 7 + 0], pose[i * 7 + 1], pose[i * 7 + 2]]; }
function dist(a, b) {
    return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// Humanoid-ish 5-part chain: pelvis - spine - head + one two-segment arm.
// Bind transforms are model space; `position` places the model origin.
function humanoidOpts(pos) {
    return {
        position: pos,
        parts: [
            { name: 'pelvis', shape: 'capsule', halfHeight: 0.10, radius: 0.12,
              position: { x: 0, y: 1.00, z: 0 } },
            { name: 'spine', parent: 'pelvis', shape: 'capsule', halfHeight: 0.12, radius: 0.11,
              position: { x: 0, y: 1.35, z: 0 },
              joint: { point: { x: 0, y: 1.15, z: 0 }, normalHalfConeAngle: 25 * DEG,
                       twistMin: -20 * DEG, twistMax: 20 * DEG } },
            { name: 'head', parent: 'spine', shape: 'sphere', radius: 0.11,
              position: { x: 0, y: 1.72, z: 0 },
              joint: { point: { x: 0, y: 1.55, z: 0 }, normalHalfConeAngle: 35 * DEG,
                       twistMin: -45 * DEG, twistMax: 45 * DEG } },
            { name: 'upperArm', parent: 'spine', shape: 'capsule', halfHeight: 0.10, radius: 0.05,
              position: { x: 0.36, y: 1.42, z: 0 }, rotation: RZM90,   // capsule axis -> +X
              joint: { point: { x: 0.24, y: 1.42, z: 0 }, twistAxis: { x: 1, y: 0, z: 0 },
                       normalHalfConeAngle: 60 * DEG, twistMin: -30 * DEG, twistMax: 30 * DEG } },
            { name: 'lowerArm', parent: 'upperArm', shape: 'capsule', halfHeight: 0.09, radius: 0.04,
              position: { x: 0.60, y: 1.42, z: 0 }, rotation: RZM90,
              joint: { point: { x: 0.47, y: 1.42, z: 0 }, twistAxis: { x: 1, y: 0, z: 0 },
                       normalHalfConeAngle: 45 * DEG, twistMin: -20 * DEG, twistMax: 20 * DEG } },
        ],
    };
}
const N_PARTS = 5;
// Bind rotations (model space) per part — arms are pre-rotated onto +X.
const BIND_ROT = [
    [0, 0, 0, 1], [0, 0, 0, 1], [0, 0, 0, 1],
    [0, 0, -Math.SQRT1_2, Math.SQRT1_2], [0, 0, -Math.SQRT1_2, Math.SQRT1_2],
];
// Sum of per-joint angular error (rad) between a pose and the bind pose's
// parent-relative rotations.
function jointError(rd, pose) {
    let err = 0;
    for (let i = 0; i < N_PARTS; i++) {
        const parent = rd.partParent(i);
        if (parent < 0) continue;
        const rel = qmul(qconj(partQ(pose, parent)), partQ(pose, i));
        const bindRel = qmul(qconj(BIND_ROT[parent]), BIND_ROT[i]);
        err += qangle(rel, bindRel);
    }
    return err;
}

// Big ground slab, top face at y = 0.
const ground = Physics.createBody({
    shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 0.8,
});

// =========================================================================
// Creation + introspection
// =========================================================================
const rd = Physics.createRagdoll(humanoidOpts({ x: 0, y: 0.5, z: 0 }));
assert(typeof rd === 'object', 'createRagdoll returns a handle');
assert(rd.partCount === N_PARTS, 'partCount is 5, got ' + rd.partCount);
assert(rd.partIndex('head') === 2, "partIndex('head') is 2");
assert(rd.partIndex('nope') === -1, 'unknown part name -> -1');
assert(rd.partParent(0) === -1, 'pelvis is the root');
assert(rd.partParent(2) === 1, 'head parent is spine');
assert(rd.partParent(4) === 3, 'lowerArm parent is upperArm');
assert(rd.partBody(9) === -1, 'out-of-range partBody -> -1');
assert(rd.isActive === true, 'ragdoll starts active');

const tags = [];
for (let i = 0; i < N_PARTS; i++) {
    const t = rd.partBody(i);
    assert(typeof t === 'number' && t >= 0, 'partBody(' + i + ') is a body tag');
    assert(tags.indexOf(t) < 0, 'part tags are distinct');
    tags.push(t);
    // Regular body APIs work on part bodies.
    const tr = Physics.getTransform(t);
    assert(tr && typeof tr.position.y === 'number', 'getTransform works on a part body');
}

const bind = rd.pose();
assert(bind instanceof Float32Array && bind.length === N_PARTS * 7,
       'pose() is a Float32Array of partCount*7');
assert(Math.abs(bind[1] - 1.5) < 1e-4, 'pelvis spawned at model y=1.0 + offset 0.5');

// Bad parts must throw, not crash.
let threw = false;
try { Physics.createRagdoll({ parts: [] }); } catch (e) { threw = true; }
assert(threw, 'empty parts array throws');
threw = false;
try {
    Physics.createRagdoll({ parts: [
        { name: 'a', parent: 'missing', position: { x: 0, y: 0, z: 0 } },
    ] });
} catch (e) { threw = true; }
assert(threw, 'unknown parent name throws');

// =========================================================================
// Fall & settle: connected, resting on the ground, joint limits hold
// =========================================================================
advanceTime(4000);

const settled = rd.pose();
for (let i = 0; i < N_PARTS; i++) {
    const v = Physics.getVelocity(tags[i]).linear;
    const speed = Math.hypot(v.x, v.y, v.z);
    assert(speed < 0.5, 'part ' + i + ' settled, |v|=' + speed.toFixed(3));
    assert(settled[i * 7 + 1] > 0.02,
           'part ' + i + ' rests above the ground, y=' + settled[i * 7 + 1].toFixed(3));
}

// Adjacent parts stay connected: distance bounded by the bind distance + slack.
const bindDist = [];
for (let i = 0; i < N_PARTS; i++) {
    const parent = rd.partParent(i);
    bindDist.push(parent < 0 ? 0 : dist(partP(bind, i), partP(bind, parent)));
    if (parent < 0) continue;
    const d = dist(partP(settled, i), partP(settled, parent));
    assert(d < bindDist[i] + 0.25,
           'part ' + i + ' still attached to ' + parent + ' (d=' + d.toFixed(3) +
           ', bind=' + bindDist[i].toFixed(3) + ')');
}

// Swing-twist limits approximately hold: the head can never end up anywhere
// near 180° from the spine (its cone is 35°, twist ±45°).
const headSpine = qangle(
    qmul(qconj(partQ(settled, 1)), partQ(settled, 2)),
    qmul(qconj(BIND_ROT[1]), BIND_ROT[2]));
assert(headSpine < 100 * DEG,
       'head-spine relative rotation within limits, ' + (headSpine / DEG).toFixed(1) + ' deg');

// =========================================================================
// Part-body interop: raycast reports parts, impulses move them
// =========================================================================
const px = settled[0], pz = settled[2];
const hit = Physics.raycastClosest(px, 3, pz, 0, -1, 0, 10);
assert(hit !== null, 'downward ray over the ragdoll hits something');
assert(tags.indexOf(hit.bodyId) >= 0, 'raycast reports a ragdoll part body');

// Impulse on one part through the regular body API.
Physics.addImpulse(tags[2], 0, 60, 0);   // pop the head upward
advanceTime(50);
const headV = Physics.getVelocity(tags[2]).linear;
assert(headV.y > 0.5, 'part impulse moves the part, vy=' + headV.y.toFixed(2));

// Whole-ragdoll impulse.
advanceTime(2000);   // re-settle
rd.addImpulse(0, 120, 0);
advanceTime(50);
let anyUp = false;
for (let i = 0; i < N_PARTS; i++)
    if (Physics.getVelocity(tags[i]).linear.y > 0.3) anyUp = true;
assert(anyUp, 'ragdoll impulse moves the body set');

// =========================================================================
// Sandbox world: setPose, kinematic drive, motor drive convergence
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64 });
w.createBody({
    shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 0.8,
});
const zr = w.createRagdoll(humanoidOpts({ x: 0, y: 0.5, z: 0 }));
assert(zr.partCount === N_PARTS, 'sandbox ragdoll created');
const bindWorld = zr.pose();

// Kinematic drive: shift the whole bind pose +1 m in x, reach it in one step.
const shifted = new Float32Array(bindWorld);
for (let i = 0; i < N_PARTS; i++) shifted[i * 7] += 1.0;
assert(zr.driveToPoseKinematic(shifted, 1 / 60) === true, 'driveToPoseKinematic accepts pose');
w.step(1 / 60);
const afterKin = zr.pose();
for (let i = 0; i < N_PARTS; i++) {
    assert(dist(partP(afterKin, i), partP(shifted, i)) < 0.05,
           'kinematic drive reached target for part ' + i);
}

// setPose: teleport straight back to the bind pose and read it back.
assert(zr.setPose(bindWorld) === true, 'setPose accepts pose');
const roundTrip = zr.pose();
for (let k = 0; k < roundTrip.length; k++) {
    assert(Math.abs(roundTrip[k] - bindWorld[k]) < 1e-3,
           'setPose round-trips [' + k + ']');
}

// Slump under gravity, then drive back toward the bind pose with motors.
for (let i = 0; i < 300; i++) w.step(1 / 60);
let err = jointError(zr, zr.pose());
assert(err > 0.15, 'ragdoll slumped (joint error ' + err.toFixed(3) + ' rad)');

w.setGravity(0, 0, 0);   // isolate the motors from gravity + ground fights
assert(zr.driveToPose(bindWorld, { frequency: 15, damping: 1 }) === true,
       'driveToPose accepts pose + motor override');
const errs = [err];
for (let s = 0; s < 5; s++) {
    for (let i = 0; i < 60; i++) w.step(1 / 60);
    errs.push(jointError(zr, zr.pose()));
}
const final = errs[errs.length - 1];
for (let s = 1; s < errs.length; s++) {
    assert(errs[s] < errs[s - 1] + 0.1,
           'drive error non-increasing-ish: ' + errs.map(e => e.toFixed(3)).join(' -> '));
}
assert(final < 0.5 * errs[0] && final < 0.5,
       'motors converge toward the bind pose: ' + errs[0].toFixed(3) + ' -> ' + final.toFixed(3));

zr.stopDrive();          // go limp again — must not throw
zr.deactivate();
assert(zr.isActive === false, 'deactivate puts the ragdoll to sleep');
zr.activate();
assert(zr.isActive === true, 'activate wakes it');

// =========================================================================
// Destroy mid-sim: handle path and part-body path
// =========================================================================
zr.destroy();
for (let i = 0; i < 30; i++) w.step(1 / 60);   // must not crash
assert(zr.pose() === null, 'pose() null after destroy');
assert(zr.partBody(0) === -1, 'partBody -1 after destroy');
zr.destroy();                                  // double destroy is a no-op

// Destroying any PART BODY destroys the whole ragdoll (bodies + constraints
// are one unit).
const zr2 = w.createRagdoll(humanoidOpts({ x: 3, y: 0.5, z: 0 }));
w.destroyBody(zr2.partBody(1));
for (let i = 0; i < 30; i++) w.step(1 / 60);   // must not crash
assert(zr2.pose() === null, 'ragdoll gone after a part body is destroyed');
w.destroy();

// Default-world: destroy mid-fall, then destroyAll with a live ragdoll.
const rd2 = Physics.createRagdoll(humanoidOpts({ x: 5, y: 2, z: 0 }));
advanceTime(200);
rd2.destroy();
advanceTime(200);                              // must not crash
const rd3 = Physics.createRagdoll(humanoidOpts({ x: -5, y: 2, z: 0 }));
Physics.destroyAll();
advanceTime(200);                              // must not crash
assert(rd3.pose() === null, 'destroyAll tears down live ragdolls');

// =========================================================================
// Skinned-mesh integration recipe, end-to-end (the docs recipe verbatim):
// a 2-part ragdoll mirrors a 2-bone skeleton; localPose() drops into the
// bromesh Pose, computeSkinningMatrices feeds setSkinningMatrices. Verified
// numerically against the analytic hinge and in pixels.
// =========================================================================
const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();
const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available — skipping ragdoll skinning recipe test');
} else {
    const FOV = 40, EYE = [0, 1, 8];
    scene.setCamera({ fov: FOV, near: 0.1, far: 100, position: EYE, target: [0, 1, 0] });
    scene.createLight({ type: 'directional', direction: [0, 0, -1],
                        color: [1, 1, 1], intensity: 2.5 });
    function project(x, y) {
        const k = 128 / (EYE[2] * Math.tan((FOV / 2) * Math.PI / 180));
        return [Math.round(128 + x * k), Math.round(128 - (y - 1) * k)];
    }
    function patchMaxAlpha(img, cx, cy, r) {
        let m = 0;
        for (let y = cy - r; y <= cy + r; y++)
            for (let x = cx - r; x <= cx + r; x++) {
                if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
                m = Math.max(m, img.data[(y * img.width + x) * 4 + 3]);
            }
        return m;
    }

    // Vertical strip split rigidly at y = 1 (the joint): lower half bone 0,
    // upper half bone 1 — same rig as tests/scene/test_skinned_mesh.js.
    const ROWS = 9;
    const positions = new Float32Array(ROWS * 2 * 3);
    const normals = new Float32Array(ROWS * 2 * 3);
    const boneW = new Float32Array(ROWS * 2 * 4);
    const boneI = new Uint32Array(ROWS * 2 * 4);
    for (let r = 0; r < ROWS; r++) {
        const y = r * 0.25;
        for (let c = 0; c < 2; c++) {
            const v = r * 2 + c;
            positions[v * 3] = c === 0 ? -0.2 : 0.2;
            positions[v * 3 + 1] = y;
            normals[v * 3 + 2] = 1;
            boneW[v * 4] = 1;
            boneI[v * 4] = y > 1.0 ? 1 : 0;
        }
    }
    const indices = new Uint32Array((ROWS - 1) * 6);
    for (let r = 0; r < ROWS - 1; r++) {
        const bl = r * 2, br = bl + 1, tl = bl + 2, tr = bl + 3;
        indices.set([bl, br, tr, bl, tr, tl], r * 6);
    }
    const node = scene.createSkinnedMesh({
        positions, normals, indices, color: 'red', roughness: 0.9,
        skin: new SkinData({
            boneWeights: boneW, boneIndices: boneI, boneCount: 2,
            // Bones sit at the ragdoll part centers (y=0.5 and y=1.5).
            inverseBindMatrices: new Float32Array([
                1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-0.5,0,1,
                1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1.5,0,1,
            ]),
        }),
    });

    // Skeleton mirroring the ragdoll parts (bone i == part i, same parents,
    // bones AT the part bind transforms).
    const skel = new Skeleton({ bones: [
        { name: 'lower', parent: -1, localT: [0, 0.5, 0],
          inverseBind: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-0.5,0,1] },
        { name: 'upper', parent: 0, localT: [0, 1, 0],
          inverseBind: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1.5,0,1] },
    ]});
    const pose = skel.bindPose();

    // Matching 2-part ragdoll in a zero-gravity sandbox (so setPose holds).
    const ws = Physics.createWorldHandle({ maxBodies: 8, gravity: { x: 0, y: 0, z: 0 } });
    const srd = ws.createRagdoll({
        parts: [
            { name: 'lower', shape: 'capsule', halfHeight: 0.3, radius: 0.15,
              position: { x: 0, y: 0.5, z: 0 } },
            { name: 'upper', parent: 'lower', shape: 'capsule', halfHeight: 0.3, radius: 0.15,
              position: { x: 0, y: 1.5, z: 0 },
              joint: { point: { x: 0, y: 1, z: 0 }, normalHalfConeAngle: 120 * DEG,
                       twistMin: -120 * DEG, twistMax: 120 * DEG } },
        ],
    });

    // THE RECIPE: ragdoll localPose -> Pose local joints -> skinning palette.
    function syncMeshToRagdoll() {
        const rp = srd.localPose();          // stride 7: [t3, q4] per part
        const pd = pose.data;                // stride 10: [t3, q4, s3] per bone
        for (let i = 0; i < srd.partCount; i++) {
            for (let k = 0; k < 7; k++) pd[i * 10 + k] = rp[i * 7 + k];
        }
        pose.data = pd;
        return pose.computeSkinningMatrices(skel);
    }

    // Bind pose: palette must be identity, strip renders vertical.
    let mats = syncMeshToRagdoll();
    const I = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
    for (let b = 0; b < 2; b++)
        for (let k = 0; k < 16; k++)
            assert(Math.abs(mats[b * 16 + k] - I[k]) < 1e-4,
                   'bind palette identity [' + b + '][' + k + ']');
    node.setSkinningMatrices(mats);
    const imgBind = scene.captureFrame();
    const [txs, tys] = project(0, 1.75);     // top of the vertical strip
    const [axs, ays] = project(-0.6, 1.05);  // where the bent arm will land
    assert(patchMaxAlpha(imgBind, txs, tys, 3) > 128, 'bind: strip top visible');
    assert(patchMaxAlpha(imgBind, axs, ays, 3) === 0, 'bind: arm region empty');

    // Bend the ragdoll 90° about Z at the joint (upper part center swings
    // from (0,1.5) to (-0.5,1)) and re-run the recipe.
    const bent = new Float32Array([
        0, 0.5, 0,  0, 0, 0, 1,
        -0.5, 1, 0,  0, 0, Math.SQRT1_2, Math.SQRT1_2,
    ]);
    srd.setPose(bent);
    mats = syncMeshToRagdoll();
    // Analytic hinge: M1 = T(0,1,0) * Rz(90°) * T(0,-1,0), column-major.
    const expected = [0,1,0,0, -1,0,0,0, 0,0,1,0, 1,1,0,1];
    for (let k = 0; k < 16; k++)
        assert(Math.abs(mats[16 + k] - expected[k]) < 1e-3,
               'bent palette matches analytic hinge [' + k + '] (' +
               mats[16 + k].toFixed(4) + ' vs ' + expected[k] + ')');
    node.setSkinningMatrices(mats);
    const imgBent = scene.captureFrame();
    assert(patchMaxAlpha(imgBent, txs, tys, 3) === 0, 'bent: strip top gone');
    assert(patchMaxAlpha(imgBent, axs, ays, 3) > 128, 'bent: horizontal arm visible');

    // Powered direction (animation -> physics): drive back to straight using
    // the mat4-per-part pose form (what getBoneWorldMatrix produces).
    const straightMats = new Float32Array([
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0.5,0,1,
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,1.5,0,1,
    ]);
    assert(srd.driveToPoseKinematic(straightMats, 1 / 60) === true,
           'driveToPoseKinematic accepts mat4-per-part poses');
    // Kinematic drive is a per-frame tracking tool — re-issue each step (a
    // 90° swing in one tick would exceed the max angular velocity clamp).
    for (let i = 0; i < 30; i++) {
        srd.driveToPoseKinematic(straightMats, 1 / 60);
        ws.step(1 / 60);
    }
    const back = srd.pose();
    assert(dist(partP(back, 1), [0, 1.5, 0]) < 0.05, 'mat4 drive straightened the upper part');
    assert(Math.abs(back[7 + 5]) < 0.05, 'upper part rotation back near identity');

    ws.destroy();
    console.log('ragdoll skinning recipe verified');
}

// =========================================================================
// Teardown/GC: leave live handles in globals, no explicit destroy. The
// Debug-build QuickJS leak assert is the real gate (worldRef gc_mark +
// ~JsWorld back-pointer severing).
// =========================================================================
globalThis.__ragdollGcWorld = Physics.createWorldHandle({ maxBodies: 32 });
globalThis.__ragdollGcWorld.createBody({
    shape: 'box', halfExtents: { x: 10, y: 0.5, z: 10 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
globalThis.__ragdollGcRagdoll =
    globalThis.__ragdollGcWorld.createRagdoll(humanoidOpts({ x: 0, y: 0.5, z: 0 }));
globalThis.__ragdollGcWorld.step(1 / 60);
globalThis.__ragdollGcDefault = Physics.createRagdoll(humanoidOpts({ x: 0, y: 0.5, z: 0 }));
advanceTime(50);

// No Physics.destroyAll() here on purpose: teardown must clean up the live
// ragdoll bodies/constraints and JS handles in arbitrary GC order.
console.log('ragdoll tests passed');
