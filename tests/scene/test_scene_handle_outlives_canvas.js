// A JS handle to a scene-owned helper must survive its SceneGraph being
// reclaimed.
//
// The engine destroys a SceneGraph the instant its canvas leaves the DOM
// (Engine::pruneDetachedSceneGraphs). SceneNode wrappers already survive that —
// they hold {weak liveness token, node id} and re-resolve on every call, see
// the design note at the top of src/js/scene_bindings_internal.h. The three
// scene-side helper objects did NOT: TileWorld, ClipmapTerrain and
// TerrainManager each held a `SceneGraph&`, so a JS handle that outlived its
// canvas by one frame left them pointing at freed memory, and their own
// destructors then called graph.destroyNode() through it — a hard segfault at
// context teardown, which is where a JS handle is finally released.
//
// That is what "removing a live scene canvas from the DOM without destroying
// its scene nodes segfaults the engine" actually was: not the removal, the
// surviving handle. All three now hold the same weak liveness token.
//
// A crash test — the process dying before the final console.log is the failure.

function makeCanvas(id) {
    const holder = document.createElement('div');
    holder.id = id;
    document.body.appendChild(holder);
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '256');
    cv.setAttribute('height', '256');
    holder.appendChild(cv);
    flush();
    return { holder, cv };
}

function tick(n) { for (let i = 0; i < n; ++i) { advanceTime(16); flush(); } }

const probe = makeCanvas('probe');
const probeScene = probe.cv.getContext('scene');
if (!probeScene) {
    console.log('scene context not available (no GPU)');
} else {
    probe.holder.remove();
    flush();

    // Handles deliberately kept alive past their canvas, so they are released
    // at context teardown — after the graph is long gone.
    const survivors = [];

    // --- TileWorld -------------------------------------------------------
    {
        const { holder, cv } = makeCanvas('tw');
        const s = cv.getContext('scene');
        s.setCamera({ fov: 50, near: 0.1, far: 500, position: [30, 30, 30], target: [0, 0, 0] });
        const tw = s.createTileWorld({ width: 32, height: 32, tileSize: 1, shape: 'hex' });
        tw.fillTile(0, 0, 31, 31, 1);
        flush();

        // The canvas goes with its container's markup — the shape a
        // full-re-render UI takes on every view switch.
        holder.innerHTML = '<p>replaced</p>';
        flush();
        tick(4);

        // Driving the orphaned world must no-op, not crash: the nodes it would
        // author into are gone.
        tw.fillTile(0, 0, 8, 8, 3);
        tw.setTile(1, 1, 2);
        tw.setElevation(2, 2, 3);
        flush();
        tick(4);
        assert(tw.getTile(1, 1, 0) === 2, 'grid data still readable after the graph died');
        assert(tw.getTile(4, 4, 0) === 3, 'and the fill landed too');

        survivors.push(tw, s);
        holder.remove();
        flush();
    }

    // --- ClipmapTerrain --------------------------------------------------
    {
        const { holder, cv } = makeCanvas('cm');
        const s = cv.getContext('scene');
        s.setCamera({ fov: 50, near: 0.1, far: 5000, position: [0, 200, 0], target: [0, 0, 0] });
        const cm = typeof s.createClipmapTerrain === 'function'
            ? s.createClipmapTerrain({ levels: 4, resolution: 32, cellSize: 1.0 })
            : null;
        flush();
        if (cm) {
            holder.innerHTML = '';
            flush();
            tick(4);
            if (typeof cm.update === 'function') cm.update(0, 200, 0);
            flush();
            tick(4);
            survivors.push(cm, s);
        }
        holder.remove();
        flush();
    }

    // --- TerrainManager --------------------------------------------------
    {
        const { holder, cv } = makeCanvas('tr');
        const s = cv.getContext('scene');
        s.setCamera({ fov: 50, near: 0.1, far: 500, position: [30, 30, 30], target: [0, 0, 0] });
        const tr = typeof s.createTerrain === 'function'
            ? s.createTerrain({ chunkSize: [16, 24, 16], cellSize: 1.0,
                                loadRadius: 2, unloadRadius: 3,
                                baseHeight: 8, heightAmplitude: 6 })
            : null;
        if (tr && typeof tr.update === 'function') tr.update(0, 0, 0);   // stream chunks in
        flush();
        if (tr) {
            holder.innerHTML = '';
            flush();
            tick(4);
            if (typeof tr.update === 'function') tr.update(40, 0, 40);   // would stream more
            flush();
            tick(4);
            survivors.push(tr, s);
        }
        holder.remove();
        flush();
    }

    // --- The canvas comes back ------------------------------------------
    // A fresh scene on a fresh canvas must still work with the dead handles
    // above still referenced.
    {
        const { holder, cv } = makeCanvas('again');
        const s = cv.getContext('scene');
        assert(s !== null && s !== undefined, 'a new canvas still gets a scene');
        s.setCamera({ fov: 50, near: 0.1, far: 500, position: [20, 20, 20], target: [0, 0, 0] });
        const tw2 = s.createTileWorld({ width: 16, height: 16, tileSize: 1, shape: 'square' });
        tw2.fillTile(0, 0, 15, 15, 1);
        flush();
        tick(4);
        assert(tw2.getTile(3, 3, 0) === 1, 'the new TileWorld meshes and reads back');
        survivors.push(tw2, s);
        holder.remove();
        flush();
        tick(4);
    }

    assert(survivors.length >= 4, 'kept handles alive past their graphs');
    console.log('scene handles outliving their canvas: ' + survivors.length + ' held, no crash');
}
