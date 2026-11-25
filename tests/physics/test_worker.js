// Physics in a Worker: the worker's own world steps there (a body falls as it
// does on the page), and a call outside any world the worker entered finds
// none (the engine's world is the engine thread's): createBody answers -1.
// Exercises installWorkerBroRoot (host_bro_root.cpp) + enginePhysicsWorld.

const src = `
const stray = Physics.createBody({ shape: 'box', size: [1, 1, 1], position: [0, 1, 0], mass: 1 });
const W = Physics.createWorldHandle({ maxBodies: 16, gravity: { x: 0, y: -10, z: 0 } });
const b = W.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 100, z: 0 }, mass: 1 });
for (let i = 0; i < 60; i++) W.step(1 / 60);
postMessage({ stray, y: W.getTransform(b).position.y, type: typeof Physics });
`;

function run(source) {
    const url = URL.createObjectURL(new Blob([source], { type: 'text/javascript' }));
    const w = new Worker(url);
    let got;
    w.onmessage = (e) => { got = e.data; };
    w.onerror = (e) => { got = { error: String(e.message ?? e) }; };
    const deadline = Date.now() + 15000;
    while (got === undefined && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    w.terminate();
    return got;
}

const inWorker = run(src);
assert(inWorker && !inWorker.error, 'worker ran: ' + JSON.stringify(inWorker));
assert(inWorker.type === 'object', 'Physics in a Worker: ' + inWorker.type);
assert(inWorker.stray === -1, 'no default world in a Worker: createBody answered ' + inWorker.stray);

// The same fall on the page: bit for bit alike.
const W = Physics.createWorldHandle({ maxBodies: 16, gravity: { x: 0, y: -10, z: 0 } });
const b = W.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 100, z: 0 }, mass: 1 });
for (let i = 0; i < 60; i++) W.step(1 / 60);
const y = W.getTransform(b).position.y;
assert(y < 96 && y > 94, 'the page world falls: ' + y);
assert(inWorker.y === y, 'the worker steps as the page does: ' + inWorker.y + ' vs ' + y);
console.log('PASS physics in a worker (fell to ' + y.toFixed(3) + ' m)');
