// Worker for test_sibling_apis.js — exercises the sibling compute APIs a
// Worker realm carries (host_bro_root.cpp installWorkerBroRoot) and reports
// what it found. Every value it posts back is plain data: the classes and
// handles it made belong to this thread's runtime.

function probe() {
    const r = {};

    // bro.mesh and the Mesh class, both as the namespace and as a bare global.
    r.meshGlobal = typeof Mesh === 'function';
    r.meshNs = typeof bro.mesh === 'object' && typeof bro.mesh.box === 'function';
    const box = Mesh.box(1, 1, 1);
    r.boxIsMesh = box instanceof Mesh;
    r.boxVertexCount = box.vertexCount;
    const sphere = bro.mesh.sphere(0.5, 8, 8);
    r.sphereTriangles = sphere.triangleCount;
    r.plantSurface = typeof Mesh.flower === 'function' && typeof Mesh.tree === 'function';
    r.rigging = typeof Skeleton === 'function' && typeof bro.rigging === 'object';

    // bro.math: the classes and a function.
    r.rng = typeof Rng === 'function' && typeof bro.math.Rng === 'function';
    const rng = new Rng(7);
    r.rngValue = typeof rng.float01() === 'number';
    r.spatialHash = typeof SpatialHash3D === 'function';

    // bro.image kernels (brokit + broimage).
    r.image = typeof bro.image === 'object' && typeof bro.image.alloc === 'function';

    // The rest by presence: what the main realm has, a worker has, minus
    // the engine-bound namespaces.
    r.flora = typeof bro.flora === 'object' && typeof bro.flora.createWorld === 'function';
    if (r.flora) {
        const w = bro.flora.createWorld({ rngSeed: 1 });
        r.floraWorld = typeof w === 'object' && typeof w.step === 'function';
    }
    r.ai = typeof bro.ai === 'object' && typeof bro.ai.game === 'object';
    r.tensor = typeof bro.tensor === 'object' && typeof bro.gpu === 'object';
    r.lm = typeof bro.lm === 'object';
    r.stt = typeof bro.stt === 'object';
    r.diffusion = typeof bro.diffusion === 'object';
    r.vision = typeof bro.vision === 'object';
    r.motion = typeof bro.motion === 'object';
    r.media = typeof bro.media === 'object';
    r.net = typeof bro.net === 'object' && typeof bro.net.connect === 'function';

    // Engine-bound namespaces must NOT be here.
    r.noWindow = bro.window === undefined;
    r.noSettings = bro.settings === undefined;
    r.noScene = bro.scene === undefined;
    r.noAudio = typeof AudioContext === 'undefined';

    // bro.server names THIS loop.
    r.server = typeof bro.server === 'object' && typeof bro.server.stop === 'function';
    r.tickrateDefault = bro.server.tickrate;
    bro.server.tickrate = 120;
    r.tickrateSet = bro.server.tickrate;
    r.uptimeIsNumber = typeof bro.server.uptime === 'number' && bro.server.uptime >= 0;

    // Isolation: the main realm tags its `bro` before starting this worker;
    // a worker that saw the main realm's root would see the tag.
    r.mainTagLeaked = bro.__mainRealmTag !== undefined;
    r.meshTagLeaked = Mesh.__mainRealmTag !== undefined;
    bro.__workerRealmTag = 'worker';
    Mesh.__workerRealmTag = 'worker';

    return r;
}

self.onmessage = (e) => {
    const data = e.data;
    if (data && data.cmd === 'probe') {
        try {
            self.postMessage({ ok: true, result: probe() });
        } catch (err) {
            self.postMessage({ ok: false, error: String(err && err.stack ? err.stack : err) });
        }
    } else if (data && data.cmd === 'mesh') {
        // A Mesh arrives through the transfer list as a worker-realm Mesh;
        // send a derived one back the same way.
        try {
            const m = data.mesh;
            const out = Mesh.sphere(0.5, 8, 6);
            self.postMessage({
                ok: true,
                gotMesh: m instanceof Mesh,
                vertexCount: m ? m.vertexCount : -1,
                mesh: out,
                outVertexCount: out.vertexCount,
            }, [out]);
            self.postMessage({ ok: true, senderNeutered: out.vertexCount === 0 });
        } catch (err) {
            self.postMessage({ ok: false, error: String(err && err.stack ? err.stack : err) });
        }
    } else if (data && data.cmd === 'stop') {
        // bro.server.stop() ends this worker's loop like close() does.
        bro.server.stop();
    }
};
