// An instanced mesh must cost the same wherever its geometry sits relative to
// its own local origin.
//
// SceneGraph::render() runs a 2D overlay walk after render3D() for the
// non-world-anchored Shape/Sprite/Particle nodes. That walk used to select its
// nodes with a BLACKLIST — "anything that is not a Mesh and not an Html node"
// — which let InstancedMeshNode through, and InstancedMeshNode::onRender() IS
// the instanced draw. So every instanced node in the scene was drawn a second
// time per frame, outside the 3D pass: program 0 bound (no vertex shader),
// framebuffer 0, and the tonemap pass's renderScale-sized viewport still
// current. The driver rasterized it anyway, taking attribute 0 as clip space,
// so raw mesh-LOCAL vertex coordinates became NDC. A mesh authored across its
// own origin therefore landed inside the NDC cube and filled the viewport once
// per instance, every frame; the same mesh authored clear of the origin landed
// outside NDC and was clipped for free. Invisible either way (the compositor
// overwrites framebuffer 0 straight after), so it showed up only as GPU time:
// ~3.3 us per instance per frame vs ~0.009, a ~370x gap that capped a 33k
// instance TileWorld at 21 fps.
//
// The two nodes below are the same geometry in the same world place, built two
// ways: A authored across the origin, B authored 8 m up with the instance
// translation pulled 8 m down. They must render identical pixels AND cost the
// same. The pixel check matters as much as the timing one: "make A cheap" must
// not be satisfiable by drawing less.

const W = 1280, H = 720;
const N = 10000;

const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(W));
canvas.setAttribute('height', String(H));
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping');
} else {
    scene.setCamera({
        fov: 60, near: 0.1, far: 4000,
        position: [60, 80, 60], target: [50, 0, 50], up: [0, 1, 0],
    });

    // 16-float instance record: 4x3 affine rows (translation in the last
    // column of each row) then RGBA. `ty` shifts every instance in Y.
    function instances(ty) {
        const a = new Float32Array(N * 16);
        for (let i = 0; i < N; i++) {
            const o = i * 16;
            a[o + 0] = 1; a[o + 3] = i % 100;
            a[o + 5] = 1; a[o + 7] = ty;
            a[o + 10] = 1; a[o + 11] = Math.floor(i / 100);
            a[o + 12] = a[o + 13] = a[o + 14] = a[o + 15] = 1;
        }
        return a;
    }

    // Mean GPU ms per frame. perf.gpuFrameMs() is a GL_TIME_ELAPSED query
    // around the scene render and blocks on the result, so each sample is an
    // isolated frame rather than a pipelined average.
    function gpuMs(n = 12) {
        for (let i = 0; i < 6; i++) flush();
        perf.gpuFrameMs();               // drain the pending query
        let sum = 0;
        for (let i = 0; i < n; i++) { flush(); sum += perf.gpuFrameMs(); }
        return sum / n;
    }

    // Cheap whole-frame fingerprint: coverage + summed RGB.
    function pixels() {
        const img = scene.toImageData();
        let covered = 0, sum = 0;
        for (let i = 0; i < img.data.length; i += 4) {
            if (img.data[i + 3] > 0) covered++;
            sum += img.data[i] + img.data[i + 1] + img.data[i + 2];
        }
        return { covered, sum };
    }

    let live = [];
    function show(mesh, ty) {
        for (const n of live) scene.destroyNode(n);
        live = [scene.createInstancedMesh({
            mesh, instances: instances(ty), color: [0.6, 0.6, 0.6, 1],
        })];
        flush(); flush();
    }

    // ---- plain instanced nodes -------------------------------------------
    show(Mesh.box(1.259, 2, 0.14), 0);
    const straddlePix = pixels();
    const straddleMs = gpuMs();

    show(Mesh.box(1.259, 2, 0.14).translate(0, 8, 0), -8);
    const liftedPix = pixels();
    const liftedMs = gpuMs();

    console.log(`instanced ${N}: origin-straddling ${straddleMs.toFixed(3)} ms, ` +
                `lifted ${liftedMs.toFixed(3)} ms ` +
                `(${(straddleMs / Math.max(liftedMs, 1e-6)).toFixed(1)}x)`);

    assert(straddlePix.covered > 0, 'the instances actually render');
    assert(straddlePix.covered === liftedPix.covered &&
           straddlePix.sum === liftedPix.sum,
           `both authorings render the same pixels ` +
           `(${straddlePix.covered}/${straddlePix.sum} vs ` +
           `${liftedPix.covered}/${liftedPix.sum})`);

    // Timing only means something where the query works. gpuFrameMs returns
    // -1 until a real measurement lands.
    if (liftedMs > 0) {
        // Generous: the bug was 165x here. 5x plus a 1 ms floor absorbs
        // machine variance, scheduler noise and a slow GPU without letting a
        // recurrence through.
        const budget = liftedMs * 5 + 1.0;
        assert(straddleMs <= budget,
               `origin-straddling instanced mesh costs no more than lifted ` +
               `(${straddleMs.toFixed(3)} ms vs budget ${budget.toFixed(3)} ms)`);
    }

    for (const n of live) scene.destroyNode(n);
    live = [];
    flush();

    // ---- the same thing through TileWorld object kinds --------------------
    // This is the shape the bug was found in: a TileWorld registers one
    // InstancedMeshNode per object kind, and a wall/floor mesh naturally
    // straddles its own origin.
    const world = scene.createTileWorld({
        width: 128, height: 128, tileSize: 1, topology: 'square',
    });
    for (let y = 0; y < 128; y++) {
        for (let x = 0; x < 128; x++) world.setTile(x, y, 1, 0);
    }
    world.rebuildAll();
    flush();

    function kindMs(mesh, yOffset) {
        const kind = world.addObjectKind(mesh, { color: [0.6, 0.6, 0.6, 1] });
        for (let i = 0; i < N; i++) {
            world.addObject(kind, i % 100, Math.floor(i / 100), { yOffset, scale: 1 });
        }
        world.rebuildObjects();
        flush(); flush();
        const ms = gpuMs();
        world.clearObjects(kind);
        world.rebuildObjects();
        flush();
        return ms;
    }

    const tileStraddleMs = kindMs(Mesh.box(1.259, 2, 0.14), 0);
    const tileLiftedMs = kindMs(Mesh.box(1.259, 2, 0.14).translate(0, 8, 0), -8);
    console.log(`tileworld ${N}: origin-straddling ${tileStraddleMs.toFixed(3)} ms, ` +
                `lifted ${tileLiftedMs.toFixed(3)} ms ` +
                `(${(tileStraddleMs / Math.max(tileLiftedMs, 1e-6)).toFixed(1)}x)`);

    if (tileLiftedMs > 0) {
        const budget = tileLiftedMs * 5 + 1.0;
        assert(tileStraddleMs <= budget,
               `origin-straddling TileWorld object kind costs no more than ` +
               `lifted (${tileStraddleMs.toFixed(3)} ms vs budget ` +
               `${budget.toFixed(3)} ms)`);
    }

    // ---- the 2D overlay walk still runs for the nodes it is for -----------
    // The fix narrowed that walk to a whitelist; a Shape node must still be
    // visited by it.
    const shape = scene.createShape({ type: 'rect', width: 40, height: 40 });
    assert(shape !== null, '2D shape node still creates');
    flush();
    console.log('instanced origin cost: OK');
}
