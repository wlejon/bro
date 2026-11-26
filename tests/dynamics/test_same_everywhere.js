// A world steps the same bit for bit on every platform: one scene (a wall of
// 120 boxes on a slab, a ball driven into it, a capsule dropped on it), 600
// steps, and a hash of every body's pose bits that Windows x64, Linux x64 and
// macOS arm64 all arrive at. Peers that send inputs and not state rely on it.

const w = bro.dynamics.createWorld();
w.addBox({ half: [30, 0.5, 30], position: [0, -0.5, 0], mass: 0 });
const bodies = [];
for (let y = 0; y < 8; y++)
    for (let x = 0; x < 5; x++)
        for (let z = 0; z < 3; z++)
            bodies.push(w.addBox({ half: [0.5, 0.5, 0.5], position: [x * 1.02 - 2, 0.5 + y * 1.01, z * 1.02 - 1], mass: 20 }));
bodies.push(w.addSphere({ radius: 0.8, position: [-25, 3, 0.2], mass: 400, velocity: [30, 2, 0.3] }));
bodies.push(w.addCapsule({ radius: 0.3, halfHeight: 0.6, position: [0.3, 14, 0.1], mass: 15, angular: [1, 2, 3] }));

const f = new Float32Array(7), u = new Uint32Array(f.buffer);
let h = 2166136261 >>> 0;
const at = {};
for (let i = 1; i <= 600; i++) {
    w.step(1 / 60);
    if (i % 100) continue;
    for (const b of bodies) {
        f.set(w.pose(b), 0);
        for (let k = 0; k < 7; k++) h = Math.imul(h ^ u[k], 16777619) >>> 0;
    }
    at[i] = h.toString(16);
}
w.destroy();

assert(at[100] === '238d0f71', 'the wall at step 100: ' + at[100]);
assert(at[300] === '1a5778b', 'the wall at step 300: ' + at[300]);
assert(at[600] === '3b215e90', 'the wall at step 600: ' + at[600]);
console.log('test_same_everywhere: PASS ' + at[600]);
