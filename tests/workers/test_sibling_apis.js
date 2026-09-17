// A Worker realm carries the sibling compute APIs (bro.mesh / Mesh,
// bro.math, bro.image, bro.flora, bro.ai.game, bro.tensor, bro.lm, ...) and
// bro.server, built on the worker's own thread over the worker's own roots
// (host_bro_root.cpp installWorkerBroRoot, host_sibling_apis.cpp
// installWorkerSiblingApis). Two things are proved here: that the surface
// is there and works, and that it is the WORKER's — a class or root the
// main realm tags is not the one the worker sees, in either direction.

const workerPath = '../workers/worker_sibling_apis.js';

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 20000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

// Tag the main realm's root and Mesh before the worker exists.
bro.__mainRealmTag = 'main';
Mesh.__mainRealmTag = 'main';

// The main realm's own bro.server, the same shape.
assert(typeof bro.server === 'object' && typeof bro.server.stop === 'function', 'main bro.server present');
assert(typeof bro.server.tickrate === 'number', 'main bro.server.tickrate is a number');
assert(typeof bro.server.uptime === 'number', 'main bro.server.uptime is a number');

const w = new Worker(workerPath);
let reply = null;
w.onmessage = (e) => { reply = e.data; };
w.postMessage({ cmd: 'probe' });
pumpUntil(() => reply !== null);
assert(reply !== null, 'worker replied');
assert(reply.ok === true, 'worker probe ran: ' + (reply.error || ''));
const r = reply.result;

// ---- the surface ---------------------------------------------------------
assert(r.meshGlobal, 'Mesh is a global in the worker');
assert(r.meshNs, 'bro.mesh in the worker');
assert(r.boxIsMesh, 'Mesh.box() instance is a Mesh in the worker');
assert(r.boxVertexCount === 24, 'box has 24 vertices, got ' + r.boxVertexCount);
assert(r.sphereTriangles > 0, 'sphere has triangles');
assert(r.plantSurface, 'plant statics (flower, tree) in the worker');
assert(r.rigging, 'Skeleton / bro.rigging in the worker');
assert(r.rng && r.rngValue, 'bro.math Rng in the worker');
assert(r.spatialHash, 'SpatialHash3D in the worker');
assert(r.image, 'bro.image kernels in the worker');
assert(r.flora, 'bro.flora in the worker');
assert(r.floraWorld, 'bro.flora.createWorld() works in the worker');
assert(r.ai, 'bro.ai.game in the worker');
assert(r.tensor, 'bro.tensor / bro.gpu in the worker');
assert(r.lm, 'bro.lm in the worker');
assert(r.stt, 'bro.stt in the worker');
assert(r.diffusion, 'bro.diffusion in the worker');
assert(r.vision, 'bro.vision in the worker');
assert(r.motion, 'bro.motion in the worker');
assert(r.media, 'bro.media in the worker');
assert(r.net, 'bro.net in the worker');

// ---- what a worker must not have ------------------------------------------
assert(r.noWindow, 'no bro.window in the worker');
assert(r.noSettings, 'no bro.settings in the worker');
assert(r.noScene, 'no bro.scene in the worker');
assert(r.noAudio, 'no AudioContext in the worker');

// ---- bro.server names the worker's loop -----------------------------------
assert(r.server, 'bro.server in the worker');
assert(r.tickrateDefault === 60, 'worker tickrate defaults to 60, got ' + r.tickrateDefault);
assert(r.tickrateSet === 120, 'worker tickrate set to 120, got ' + r.tickrateSet);
assert(r.uptimeIsNumber, 'worker uptime is a number');

// ---- isolation, both directions --------------------------------------------
assert(r.mainTagLeaked === false, 'worker does not see the main realm root');
assert(r.meshTagLeaked === false, 'worker does not see the main realm Mesh');
assert(bro.__workerRealmTag === undefined, 'main realm does not see the worker root');
assert(Mesh.__workerRealmTag === undefined, 'main realm does not see the worker Mesh');
assert(bro.__mainRealmTag === 'main', 'main realm root tag intact');
assert(Mesh.__mainRealmTag === 'main', 'main realm Mesh tag intact');

// The main realm's Mesh still works after the worker built its own.
const mainBox = Mesh.box(1, 1, 1);
assert(mainBox instanceof Mesh && mainBox.vertexCount === 24, 'main realm Mesh intact');
assert(bro.server.tickrate !== 120, 'worker tickrate did not change the main realm');

// ---- Mesh crosses postMessage zero-copy through the transfer list ---------
const sent = Mesh.box(2, 2, 2);
let threw = null;
try { w.postMessage({ cmd: 'mesh', mesh: sent }); } catch (e) { threw = e; }
assert(threw !== null && /transferList/.test(String(threw)),
    'a Mesh outside the transfer list is refused: ' + String(threw));
assert(sent.vertexCount === 24, 'refused post leaves the Mesh intact');

const replies = [];
w.onmessage = (e) => { replies.push(e.data); };
w.postMessage({ cmd: 'mesh', mesh: sent }, [sent]);
assert(sent.vertexCount === 0, 'transferred Mesh is neutered in the sender');
pumpUntil(() => replies.length >= 2);
assert(replies.length >= 2 && replies[0].ok === true, 'mesh round trip ran: ' + (replies[0] && replies[0].error || ''));
assert(replies[0].gotMesh === true && replies[0].vertexCount === 24, 'worker received the transferred Mesh');
const back = replies[0].mesh;
assert(back instanceof Mesh, 'main realm receives a Mesh back');
assert(back.vertexCount === replies[0].outVertexCount && back.vertexCount > 0, 'received Mesh carries its geometry');
assert(replies[1].senderNeutered === true, 'worker-side transfer neutered its sender');
w.onmessage = (e) => { reply = e.data; };

// ---- bro.server.stop() ends the worker like close() ------------------------
w.postMessage({ cmd: 'stop' });
// A stopped worker no longer answers.
reply = null;
wallSleep(50);
w.postMessage({ cmd: 'probe' });
pumpUntil(() => reply !== null, 500);
assert(reply === null, 'a worker that called bro.server.stop() answers nothing more');
w.terminate();

// A second worker builds its own classes again on a fresh thread.
const w2 = new Worker(workerPath);
let reply2 = null;
w2.onmessage = (e) => { reply2 = e.data; };
w2.postMessage({ cmd: 'probe' });
pumpUntil(() => reply2 !== null);
assert(reply2 !== null && reply2.ok === true, 'second worker probe ran: ' + (reply2 && reply2.error || ''));
assert(reply2.result.boxIsMesh && reply2.result.mainTagLeaked === false, 'second worker has its own Mesh');
w2.terminate();
