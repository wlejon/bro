// SceneGraph.unprojectLocal(x, y) -> { origin, dir } and its inverse
// projectLocal(x, y, z) -> { x, y, depth, behind }, for every way a camera is
// set: scene.setCamera (perspective + orthographic) and a camera node. The
// bronze port had turned unprojectLocal into unprojectLocal(node, [x, y]),
// which answered [] under setCamera.

const canvas = document.createElement('canvas');
canvas.style.width = '400px';
canvas.style.height = '300px';
canvas.setAttribute('width', '400');
canvas.setAttribute('height', '300');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU); skipping');
} else {
    const near = (a, b, eps) => Math.abs(a - b) < (eps || 1e-3);
    const len = (v) => Math.hypot(v[0], v[1], v[2]);
    // Where a ray meets the plane y = h.
    const atY = (ray, h) => {
        const t = (h - ray.origin[1]) / ray.dir[1];
        return [ray.origin[0] + ray.dir[0] * t, h, ray.origin[2] + ray.dir[2] * t];
    };

    function roundTrip(label) {
        for (const p of [[0, 0, 0], [2, 0, -1], [-3, 0.5, 2]]) {
            const s = scene.projectLocal(p[0], p[1], p[2]);
            assert(s && !s.behind, label + ': ' + p + ' projects in front: ' + JSON.stringify(s));
            const ray = scene.unprojectLocal(s.x, s.y);
            assert(ray && near(len(ray.dir), 1), label + ': unit ray at ' + s.x + ',' + s.y);
            const w = atY(ray, p[1]);
            assert(near(w[0], p[0], 1e-2) && near(w[2], p[2], 1e-2),
                   label + ': ray through ' + p + ' lands at ' + w);
        }
    }

    // --- setCamera, perspective ---------------------------------------------------
    scene.setCamera({ fov: 60, near: 0.1, far: 200, position: [0, 10, 12], target: [0, 0, 0], up: [0, 1, 0] });
    flush();
    {
        const ray = scene.unprojectLocal(200, 150);
        assert(ray && Array.isArray(ray.origin) && Array.isArray(ray.dir), 'setCamera: { origin, dir }');
        assert(near(ray.origin[0], 0) && near(ray.origin[1], 10) && near(ray.origin[2], 12),
               'perspective ray starts at the eye: ' + ray.origin);
        const hit = atY(ray, 0);
        assert(near(hit[0], 0, 1e-2) && near(hit[2], 0, 1e-2), 'centre pixel looks at the target: ' + hit);
        const c = scene.projectLocal([0, 0, 0]);
        assert(near(c.x, 200, 0.5) && near(c.y, 150, 0.5), 'target projects to the canvas centre: ' + c.x + ',' + c.y);
        assert(near(c.depth, Math.hypot(10, 12), 1e-2), 'depth is the forward distance: ' + c.depth);
        assert(scene.projectLocal(0, 20, 30).behind, 'a point behind the eye is behind');
        roundTrip('perspective');

        // Agrees with viewMatrix * projectionMatrix.
        const V = scene.viewMatrix, P = scene.projectionMatrix;
        const mul = (m, v) => [0, 1, 2, 3].map((i) => m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3]);
        const q = mul(P, mul(V, [2, 1, -1, 1]));
        const s = scene.projectLocal(2, 1, -1);
        assert(near(s.x, (q[0] / q[3] * 0.5 + 0.5) * 400, 0.5) && near(s.y, (0.5 - q[1] / q[3] * 0.5) * 300, 0.5),
               'projectLocal matches the engine matrices');
    }

    // --- setCamera, orthographic --------------------------------------------------
    scene.setCamera({ mode: 'orthographic', size: 20, near: 0.1, far: 400, position: [16, 18, 16], target: [0, 0, 0], up: [0, 1, 0] });
    flush();
    {
        const a = scene.unprojectLocal(10, 10), b = scene.unprojectLocal(390, 290);
        assert(a && b, 'ortho rays exist');
        assert(near(a.dir[0], b.dir[0]) && near(a.dir[1], b.dir[1]) && near(a.dir[2], b.dir[2]), 'ortho rays are parallel');
        assert(!near(a.origin[0], b.origin[0]), 'ortho ray origins slide across the view plane');
        roundTrip('orthographic');
    }

    // --- camera node ----------------------------------------------------------------
    const cam = scene.createCamera({ fov: 50, near: 0.1, far: 100, position: [5, 6, 9], target: [0, 0, 0], active: true });
    advanceTime(16);
    flush();
    {
        const ray = scene.unprojectLocal(200, 150);
        assert(ray && near(ray.origin[0], 5, 1e-2) && near(ray.origin[1], 6, 1e-2) && near(ray.origin[2], 9, 1e-2),
               'camera node: ray from the node eye: ' + (ray && ray.origin));
        roundTrip('camera node');
    }
    cam.destroy();

    let threw = false;
    try { scene.unprojectLocal(); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'missing coordinates throw a TypeError');
    console.log('unproject_local: ok');
}
