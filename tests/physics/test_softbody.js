// Test soft bodies — Physics.createSoftBody (Jolt SoftBody, XPBD cloth +
// pressurized volumes). Covers cloth creation/topology (grid order, faces),
// pinned corners holding while the rest drapes, draping OVER a static box
// (collision), a pressurized ball that bounces and stays inflated, body-tag
// interop (raycast reports the soft body, impulses through the regular body
// API move it), runtime setVertex/pin, the sandbox-world form, destroy
// mid-sim (handle and body-tag paths), destroyAll with a live soft body,
// the scene-sync recipe from docs/physics-api.js verified in pixels
// (updateMesh + recomputeNormals — lit, not black, and moving), and GC
// teardown without explicit destroy (worldRef gc_mark + ~JsWorld severing —
// the Debug leak assert is the gate).
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.createSoftBody === 'function', 'createSoftBody exists');

Physics.destroyAll();

function meanY(v) {
    let s = 0;
    for (let i = 0; i < v.length; i += 3) s += v[i + 1];
    return s / (v.length / 3);
}
function minY(v) {
    let m = Infinity;
    for (let i = 0; i < v.length; i += 3) m = Math.min(m, v[i + 1]);
    return m;
}
function centroid(v) {
    let x = 0, y = 0, z = 0;
    const n = v.length / 3;
    for (let i = 0; i < n; i++) { x += v[i * 3]; y += v[i * 3 + 1]; z += v[i * 3 + 2]; }
    return [x / n, y / n, z / n];
}
// Max per-vertex displacement between two snapshots (rest detection).
function maxMove(a, b) {
    let m = 0;
    for (let i = 0; i < a.length; i += 3)
        m = Math.max(m, Math.hypot(a[i] - b[i], a[i + 1] - b[i + 1], a[i + 2] - b[i + 2]));
    return m;
}

// Big ground slab, top face at y = 0.
const ground = Physics.createBody({
    shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 0.8,
});

// =========================================================================
// Creation + topology (cloth)
// =========================================================================
const GX = 10, GZ = 10, SP = 0.1;
const cloth = Physics.createSoftBody({
    cloth: { gridX: GX, gridZ: GZ, spacing: SP, mass: 1, pinned: [0, GX - 1] },
    position: { x: 0, y: 2, z: 0 },
    linearDamping: 3.0,   // hammock-pinned cloth swings forever undamped
});
assert(typeof cloth === 'object', 'createSoftBody returns a handle');
assert(cloth.vertexCount === GX * GZ, 'vertexCount = gridX*gridZ');
assert(typeof cloth.body === 'number' && cloth.body >= 0, 'body is a regular body tag');

const topo = cloth.topology();
assert(topo.positions instanceof Float32Array && topo.positions.length === GX * GZ * 3,
       'topology positions = vertexCount*3');
assert(topo.indices instanceof Uint32Array &&
       topo.indices.length === (GX - 1) * (GZ - 1) * 6,
       'topology indices = 2 tris per grid quad');
assert(topo.gridX === GX && topo.gridZ === GZ, 'topology reports the grid');
for (let i = 0; i < topo.indices.length; i++)
    assert(topo.indices[i] < GX * GZ, 'topology indices in range');
// Vertex order: (x,z) at index z*gridX + x, rest positions LOCAL, centered.
assert(Math.abs(topo.positions[0] - (-0.45)) < 1e-5 &&
       Math.abs(topo.positions[2] - (-0.45)) < 1e-5,
       'vertex 0 at local (-half, 0, -half)');
assert(topo.positions[(GZ - 1) * GX * 3 + 2] > 0.44,
       'vertex (0, gridZ-1) at local +Z edge');

// World-space readout: created at y=2, flat.
const v0 = cloth.vertices();
assert(v0 instanceof Float32Array && v0.length === GX * GZ * 3,
       'vertices() is a Float32Array of vertexCount*3');
assert(Math.abs(meanY(v0) - 2) < 1e-4, 'cloth starts flat at y=2');

// Regular body APIs work on the tag.
const tr = Physics.getTransform(cloth.body);
assert(tr && typeof tr.position.y === 'number', 'getTransform works on the soft body tag');

// Bad options must throw, not crash.
let threw = false;
try { Physics.createSoftBody({}); } catch (e) { threw = true; }
assert(threw, 'missing cloth/mesh throws');
threw = false;
try { Physics.createSoftBody({ cloth: { gridX: 1, gridZ: 5 } }); } catch (e) { threw = true; }
assert(threw, 'bad grid throws');
threw = false;
try { Physics.createSoftBody({ mesh: { vertices: new Float32Array(3) } }); } catch (e) { threw = true; }
assert(threw, 'mesh without indices throws');

// =========================================================================
// Pinned corners hold while the rest drapes
// =========================================================================
advanceTime(3000);
const v1 = cloth.vertices();
// Pinned vertices exactly at their start positions.
for (const p of [0, GX - 1]) {
    for (let k = 0; k < 3; k++)
        assert(Math.abs(v1[p * 3 + k] - v0[p * 3 + k]) < 1e-4,
               'pinned vertex ' + p + ' held exactly (' + 'xyz'[k] + ')');
}
assert(meanY(v1) < meanY(v0) - 0.2, 'unpinned cloth sagged, meanY ' +
       meanY(v0).toFixed(3) + ' -> ' + meanY(v1).toFixed(3));
// Comes to near-rest.
advanceTime(3000);
const vA = cloth.vertices();
advanceTime(500);
const vB = cloth.vertices();
assert(maxMove(vA, vB) < 0.05, 'pinned cloth near rest, moved ' +
       maxMove(vA, vB).toFixed(4) + ' in 0.5s');

// =========================================================================
// Unpinned cloth drapes flat onto the ground — collision, no tunnelling
// =========================================================================
const flat = Physics.createSoftBody({
    cloth: { gridX: 12, gridZ: 12, spacing: 0.1, mass: 1 },
    position: { x: -4, y: 0.5, z: 0 },
});
advanceTime(3000);
const fv = flat.vertices();
assert(minY(fv) > -0.05, 'no vertex below the ground, minY=' + minY(fv).toFixed(3));
assert(meanY(fv) < 0.1, 'cloth lies on the ground, meanY=' + meanY(fv).toFixed(3));

// =========================================================================
// Cloth draping OVER a static box: center rests on top, edges hang lower
// =========================================================================
const boxTop = 0.6;
Physics.createBody({
    shape: 'box', halfExtents: { x: 0.3, y: boxTop / 2, z: 0.3 },
    position: { x: 5, y: boxTop / 2, z: 0 }, static: true, friction: 0.8,
});
const drape = Physics.createSoftBody({
    cloth: { gridX: 15, gridZ: 15, spacing: 0.1, mass: 1 },
    position: { x: 5, y: 1.0, z: 0 },
});
advanceTime(4000);
const dv = drape.vertices();
const centerIdx = 7 * 15 + 7;   // grid center over the box
const cornerIdx = 0;
assert(Math.abs(dv[centerIdx * 3 + 1] - boxTop) < 0.1,
       'center vertex rests near the box top, y=' + dv[centerIdx * 3 + 1].toFixed(3));
assert(dv[cornerIdx * 3 + 1] < boxTop - 0.3,
       'corner hangs well below the top, y=' + dv[cornerIdx * 3 + 1].toFixed(3));
assert(minY(dv) > -0.05, 'draped cloth never tunnels the ground');

// =========================================================================
// Pressurized ball: bounces, stays inflated, body impulse moves it
// =========================================================================
function uvSphere(r, segs, rings) {
    const pos = [], idx = [];
    pos.push(0, r, 0);
    for (let ri = 1; ri < rings; ri++) {
        const phi = Math.PI * ri / rings;
        for (let si = 0; si < segs; si++) {
            const th = 2 * Math.PI * si / segs;
            pos.push(r * Math.sin(phi) * Math.cos(th), r * Math.cos(phi),
                     r * Math.sin(phi) * Math.sin(th));
        }
    }
    pos.push(0, -r, 0);
    const bottom = pos.length / 3 - 1;
    for (let si = 0; si < segs; si++) idx.push(0, 1 + (si + 1) % segs, 1 + si);
    for (let ri = 0; ri < rings - 2; ri++) {
        for (let si = 0; si < segs; si++) {
            const a = 1 + ri * segs + si, b = 1 + ri * segs + (si + 1) % segs;
            idx.push(a, b, b + segs, a, b + segs, a + segs);
        }
    }
    const base = 1 + (rings - 2) * segs;
    for (let si = 0; si < segs; si++)
        idx.push(bottom, base + si, base + (si + 1) % segs);
    return { positions: new Float32Array(pos), indices: new Uint32Array(idx) };
}
const REST_R = 0.5;
const sph = uvSphere(REST_R, 12, 8);
const ball = Physics.createSoftBody({
    mesh: { vertices: sph.positions, indices: sph.indices, pressure: 2000, mass: 2 },
    position: { x: -8, y: 2, z: 0 },
    restitution: 0.6, friction: 0.4,
});
assert(ball.vertexCount === sph.positions.length / 3, 'ball vertex count matches input');
const btopo = ball.topology();
assert(btopo.indices.length === sph.indices.length, 'ball topology mirrors the input mesh');
assert(btopo.gridX === 0 && btopo.gridZ === 0, 'mesh soft bodies report no grid');

// Drop it: it must touch down and bounce back up (restitution + pressure).
let touched = false, bouncePeak = -Infinity;
for (let t = 0; t < 240; t++) {
    advanceTime(1000 / 60);
    const cy = centroid(ball.vertices())[1];
    if (!touched && cy < REST_R * 1.1) touched = true;
    else if (touched) bouncePeak = Math.max(bouncePeak, cy);
}
assert(touched, 'ball reached the ground');
assert(bouncePeak > 0.8, 'ball bounced, peak centroid y=' + bouncePeak.toFixed(3));

// Stays inflated: mean radius from the centroid within a band of rest radius.
const bv = ball.vertices();
const [cx, cy, cz] = centroid(bv);
let meanR = 0;
for (let i = 0; i < ball.vertexCount; i++)
    meanR += Math.hypot(bv[i * 3] - cx, bv[i * 3 + 1] - cy, bv[i * 3 + 2] - cz);
meanR /= ball.vertexCount;
assert(meanR > REST_R * 0.7 && meanR < REST_R * 1.5,
       'ball inflated, mean radius ' + meanR.toFixed(3) + ' (rest ' + REST_R + ')');

// =========================================================================
// Interop: raycast hits the soft body and reports its body tag
// =========================================================================
// Straight down through the resting ball — its top face is the first thing
// the ray meets, well clear of the ground.
const rayHit = Physics.raycastClosest(cx, cy + 5, cz, 0, -1, 0, 10);
assert(rayHit !== null, 'downward ray over the ball hits something');
assert(rayHit.bodyId === ball.body, 'raycast reports the soft body tag');
assert(rayHit.position.y > cy, 'hit lands on the ball top, y=' +
       rayHit.position.y.toFixed(3));

// Impulse through the REGULAR body API moves the whole soft body.
Physics.addImpulse(ball.body, 20, 0, 0);
advanceTime(500);
const cxAfter = centroid(ball.vertices())[0];
assert(cxAfter > cx + 1.0, 'body impulse moved the ball, x ' +
       cx.toFixed(2) + ' -> ' + cxAfter.toFixed(2));

// setVertex / pin at runtime: freeze a vertex at a lifted position — the
// draped cloth becomes a tent.
const liftIdx = centerIdx;
assert(drape.pin(liftIdx, true) === true, 'pin returns true');
assert(drape.setVertex(liftIdx, 5, 1.5, 0) === true, 'setVertex returns true');
advanceTime(1000);
const dv2 = drape.vertices();
assert(Math.hypot(dv2[liftIdx * 3] - 5, dv2[liftIdx * 3 + 1] - 1.5,
                  dv2[liftIdx * 3 + 2]) < 1e-3,
       'pinned+placed vertex stays where it was put');
// The tent apex is far above the box top — a downward ray reports the cloth.
const tentHit = Physics.raycastClosest(5, 3, 0, 0, -1, 0, 10);
assert(tentHit !== null && tentHit.bodyId === drape.body,
       'raycast reports the lifted cloth');
assert(tentHit.position.y > boxTop + 0.3,
       'hit lands on the tent, well above the box, y=' +
       tentHit.position.y.toFixed(3));
// Neighbors got dragged upward with it.
assert(dv2[(liftIdx + 1) * 3 + 1] > dv[(liftIdx + 1) * 3 + 1] + 0.2,
       'neighbor pulled up by the lifted vertex');
// Unpin: it falls again.
assert(drape.pin(liftIdx, false) === true, 'unpin returns true');
advanceTime(1500);
const dv3 = drape.vertices();
assert(dv3[liftIdx * 3 + 1] < 1.2, 'released vertex fell, y=' +
       dv3[liftIdx * 3 + 1].toFixed(3));
// setVertexVelocity: kick a resting vertex upward.
assert(drape.setVertexVelocity(0, 0, 3, 0) === true, 'setVertexVelocity returns true');
advanceTime(100);
assert(drape.vertices()[1] > dv3[1] + 0.05, 'velocity kick lifted the vertex');

// =========================================================================
// Sandbox world form + destroy paths
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64 });
w.createBody({ shape: 'box', halfExtents: { x: 20, y: 0.5, z: 20 },
               position: { x: 0, y: -0.5, z: 0 }, static: true });
const sb = w.createSoftBody({
    cloth: { gridX: 8, gridZ: 8, spacing: 0.1, mass: 1, pinned: 'corners' },
    position: { x: 0, y: 1, z: 0 },
});
assert(sb.vertexCount === 64, 'sandbox soft body created');
const sv0 = sb.vertices();
for (let i = 0; i < 60; i++) w.step(1 / 60);
const sv1 = sb.vertices();
assert(meanY(sv1) < meanY(sv0) - 0.005, 'sandbox cloth sags under w.step');
// All four corners held ('corners' pin form).
for (const p of [0, 7, 56, 63])
    assert(Math.abs(sv1[p * 3 + 1] - 1) < 1e-3, 'corner ' + p + ' pinned');

// Destroy mid-sim via the handle.
sb.destroy();
for (let i = 0; i < 30; i++) w.step(1 / 60);      // must not crash
assert(sb.vertices() === null, 'vertices() null after destroy');
assert(sb.body === -1, 'body tag -1 after destroy');
sb.destroy();                                     // double destroy is a no-op

// Destroy via the BODY TAG (generic destroyBody must evict the registry).
const sb2 = w.createSoftBody({
    cloth: { gridX: 6, gridZ: 6, spacing: 0.1 }, position: { x: 2, y: 1, z: 0 },
});
w.destroyBody(sb2.body);
for (let i = 0; i < 30; i++) w.step(1 / 60);      // must not crash
assert(sb2.vertices() === null, 'soft body gone after its body tag is destroyed');
w.destroy();

// Default world: destroy mid-fall, then destroyAll with a live soft body.
const sb3 = Physics.createSoftBody({
    cloth: { gridX: 6, gridZ: 6, spacing: 0.1 }, position: { x: 8, y: 2, z: 0 },
});
advanceTime(200);
sb3.destroy();
advanceTime(200);                                 // must not crash
const sb4 = Physics.createSoftBody({
    cloth: { gridX: 6, gridZ: 6, spacing: 0.1 }, position: { x: 8, y: 2, z: 0 },
});
Physics.destroyAll();
advanceTime(200);                                 // must not crash
assert(sb4.vertices() === null, 'destroyAll tears down live soft bodies');

// =========================================================================
// Scene-sync recipe (docs/physics-api.js) verified in pixels: the cloth
// drives a MeshNode via topology() + per-frame updateMesh with
// recomputeNormals — the lit mesh is bright (normals sane, not black) and
// the pixels change as it falls.
// =========================================================================
Physics.createBody({ shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
                     position: { x: 0, y: -0.5, z: 0 }, static: true });
const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();
const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available — skipping soft-body scene sync test');
} else {
    scene.setCamera({ fov: 45, near: 0.1, far: 100,
                      position: [0, 2.5, 2.5], target: [0, 1.2, 0] });
    scene.createLight({ type: 'directional', direction: [0, -1, -0.3],
                        color: [1, 1, 1], intensity: 2.5 });

    // THE RECIPE: topology() once for the render mesh blueprint, then stream
    // vertices() (world space — keep the node at identity) per frame.
    const scloth = Physics.createSoftBody({
        cloth: { gridX: 12, gridZ: 12, spacing: 0.12, mass: 1, pinned: 'corners' },
        position: { x: 0, y: 1.5, z: 0 },
    });
    const stopo = scloth.topology();
    const node = scene.createMesh({
        positions: scloth.vertices(), indices: stopo.indices,
        recomputeNormals: true, color: 'red', roughness: 0.9,
    });
    function stats(img) {
        let covered = 0, maxR = 0;
        for (let i = 0; i < img.width * img.height; i++) {
            if (img.data[i * 4 + 3] > 0) {
                covered++;
                maxR = Math.max(maxR, img.data[i * 4]);
            }
        }
        return { covered, maxR };
    }
    const imgFlat = scene.captureFrame();
    const sFlat = stats(imgFlat);
    assert(sFlat.covered > 2000, 'flat cloth visible, ' + sFlat.covered + ' px');
    assert(sFlat.maxR > 100, 'lit cloth is bright (normals sane), maxR=' + sFlat.maxR);

    for (let t = 0; t < 90; t++) {
        advanceTime(1000 / 60);
        node.updateMesh({ positions: scloth.vertices(), indices: stopo.indices },
                        { recomputeNormals: true });
    }
    const imgSag = scene.captureFrame();
    const sSag = stats(imgSag);
    assert(sSag.maxR > 100, 'sagged cloth still lit, maxR=' + sSag.maxR);
    let diff = 0;
    for (let i = 0; i < imgFlat.data.length; i += 4)
        if (Math.abs(imgFlat.data[i] - imgSag.data[i]) > 12 ||
            Math.abs(imgFlat.data[i + 3] - imgSag.data[i + 3]) > 12) diff++;
    assert(diff > 500, 'pixels changed as the cloth fell (' + diff + ' px)');
    console.log('soft-body scene sync verified (' + diff + ' px moved)');
}

// =========================================================================
// Teardown/GC: leave live handles in globals, no explicit destroy. The
// Debug-build QuickJS leak assert is the real gate (worldRef gc_mark +
// ~JsWorld back-pointer severing).
// =========================================================================
globalThis.__softGcWorld = Physics.createWorldHandle({ maxBodies: 32 });
globalThis.__softGcWorld.createBody({
    shape: 'box', halfExtents: { x: 10, y: 0.5, z: 10 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
globalThis.__softGcSoft = globalThis.__softGcWorld.createSoftBody({
    cloth: { gridX: 6, gridZ: 6, spacing: 0.1 }, position: { x: 0, y: 1, z: 0 },
});
globalThis.__softGcWorld.step(1 / 60);
globalThis.__softGcDefault = Physics.createSoftBody({
    cloth: { gridX: 6, gridZ: 6, spacing: 0.1 }, position: { x: 0, y: 2, z: 0 },
});
advanceTime(50);

// No Physics.destroyAll() here on purpose: teardown must clean up the live
// soft bodies and JS handles in arbitrary GC order.
console.log('soft body tests passed');
