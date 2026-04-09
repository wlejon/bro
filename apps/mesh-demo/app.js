// =============================================================================
// Mesh API Demo — exercises the standalone Mesh class extensively
// =============================================================================

var canvas = document.getElementById('canvas');
var scene = canvas.getContext('scene');
var info = document.getElementById('info');
var paused = false;
var time = 0;
var currentScene = 0;
var sceneNodes = [];

function log(msg) {
    console.log(msg);
    info.textContent = msg;
}

function clearScene() {
    for (var i = 0; i < sceneNodes.length; i++) {
        sceneNodes[i].destroy();
    }
    sceneNodes = [];
}

function addMesh(mesh, opts) {
    opts = opts || {};
    opts.data = mesh;
    var node = scene.createMesh(opts);
    sceneNodes.push(node);
    return node;
}

// =============================================================================
// Scene 1: Primitives Gallery
// =============================================================================

function scenePrimitives() {
    clearScene();
    log('Scene 1: All Mesh primitives — box, sphere, cylinder, capsule, plane, torus');

    scene.setCamera({
        fov: 50,
        position: [0, 8, 18],
        target: [0, 0, 0],
        aspect: 1024 / 768
    });

    // Box
    var box = Mesh.box(0.8, 0.8, 0.8);
    log('box: ' + box.vertexCount + ' verts, ' + box.triangleCount + ' tris, hasNormals=' + box.hasNormals);
    addMesh(box, { x: -6, y: 1, color: '#e74c3c', name: 'box' });

    // Sphere
    var sphere = Mesh.sphere(1, 24, 16);
    addMesh(sphere, { x: -3, y: 1, color: '#3498db', name: 'sphere' });

    // Cylinder
    var cyl = Mesh.cylinder(0.6, 1, 20);
    addMesh(cyl, { x: 0, y: 1, color: '#2ecc71', name: 'cylinder' });

    // Capsule
    var cap = Mesh.capsule(0.5, 0.6, 16, 8);
    addMesh(cap, { x: 3, y: 1, color: '#f39c12', name: 'capsule' });

    // Torus
    var tor = Mesh.torus(0.8, 0.3, 24, 12);
    addMesh(tor, { x: 6, y: 1, color: '#9b59b6', name: 'torus' });

    // Plane (ground)
    var ground = Mesh.plane(10, 8, 4, 4);
    addMesh(ground, { y: -0.01, color: '#2c3e50', name: 'ground' });

    log('Primitives: box=' + box.triangleCount + 't, sphere=' + sphere.triangleCount +
        't, cyl=' + cyl.triangleCount + 't, cap=' + cap.triangleCount +
        't, torus=' + tor.triangleCount + 't');
}

// =============================================================================
// Scene 2: Transforms + Cloning
// =============================================================================

function sceneTransforms() {
    clearScene();
    log('Scene 2: Transforms — translate, scale, rotate, mirror, center, clone');

    scene.setCamera({
        fov: 50,
        position: [0, 10, 20],
        target: [0, 2, 0],
        aspect: 1024 / 768
    });

    // Original box
    var original = Mesh.box(0.5, 0.5, 0.5);
    addMesh(original.clone(), { x: -6, y: 1, color: '#ecf0f1', name: 'original' });

    // Translated
    var translated = original.clone().translate(0, 0.5, 0);
    addMesh(translated, { x: -3.5, y: 1, color: '#e74c3c', name: 'translated' });

    // Scaled non-uniform
    var scaled = original.clone().scale(2, 0.5, 1);
    addMesh(scaled, { x: -1, y: 1, color: '#3498db', name: 'scaled' });

    // Rotated 45 degrees around Y
    var rotated = original.clone().rotate(0, 1, 0, Math.PI / 4);
    addMesh(rotated, { x: 1.5, y: 1, color: '#2ecc71', name: 'rotated' });

    // Mirrored on X
    var mirrored = Mesh.capsule(0.3, 0.5).mirror(0);
    addMesh(mirrored, { x: 4, y: 1, color: '#f39c12', name: 'mirrored' });

    // Centered (create off-center mesh, then center it)
    var offCenter = Mesh.box(0.5, 0.5, 0.5);
    offCenter.translate(2, 3, 1);
    var bbox1 = offCenter.computeBBox();
    offCenter.center();
    var bbox2 = offCenter.computeBBox();
    addMesh(offCenter, { x: 6, y: 1, color: '#9b59b6', name: 'centered' });

    log('center() moved bbox from [' + bbox1.centerX.toFixed(1) + ',' + bbox1.centerY.toFixed(1) + '] to [' +
        bbox2.centerX.toFixed(1) + ',' + bbox2.centerY.toFixed(1) + ']');

    // Ground
    addMesh(Mesh.plane(10, 8), { y: -0.01, color: '#2c3e50' });
}

// =============================================================================
// Scene 3: CSG Booleans
// =============================================================================

function sceneCSG() {
    clearScene();
    log('Scene 3: CSG booleans — union, subtract, intersect');

    scene.setCamera({
        fov: 50,
        position: [0, 8, 16],
        target: [0, 1, 0],
        aspect: 1024 / 768
    });

    var sphereA = Mesh.sphere(1.2, 24, 16);
    var sphereB = Mesh.sphere(1.2, 24, 16);
    sphereB.translate(1, 0, 0);

    // Show originals (ghost)
    addMesh(sphereA.clone(), { x: -8, y: 1.5, color: [0.3, 0.3, 0.8, 1] });
    addMesh(sphereB.clone(), { x: -8, y: 1.5, color: [0.8, 0.3, 0.3, 1] });

    // Union
    var u = Mesh.union(sphereA, sphereB);
    addMesh(u, { x: -3, y: 1.5, color: '#2ecc71', name: 'union' });

    // Subtract
    var s = Mesh.subtract(sphereA, sphereB);
    addMesh(s, { x: 0.5, y: 1.5, color: '#e74c3c', name: 'subtract' });

    // Intersect
    var inter = Mesh.intersect(sphereA, sphereB);
    addMesh(inter, { x: 4, y: 1.5, color: '#3498db', name: 'intersect' });

    // Split by plane
    var toSplit = Mesh.sphere(1.2, 24, 16);
    var halves = Mesh.splitByPlane(toSplit, 0, 1, 0, 0);
    addMesh(halves[0], { x: 7, y: 2.5, color: '#f39c12', name: 'top-half' });
    addMesh(halves[1], { x: 7, y: 0.5, color: '#9b59b6', name: 'bottom-half' });

    log('CSG: union=' + u.triangleCount + 't, subtract=' + s.triangleCount +
        't, intersect=' + inter.triangleCount + 't, split=[' +
        halves[0].triangleCount + ',' + halves[1].triangleCount + ']t');

    addMesh(Mesh.plane(12, 8), { y: -0.01, color: '#2c3e50' });
}

// =============================================================================
// Scene 4: Simplification + LOD
// =============================================================================

function sceneSimplify() {
    clearScene();
    log('Scene 4: Simplification and LOD chain');

    scene.setCamera({
        fov: 50,
        position: [0, 5, 16],
        target: [0, 1, 0],
        aspect: 1024 / 768
    });

    // High-res sphere
    var hiRes = Mesh.sphere(1.5, 48, 32);
    addMesh(hiRes.clone(), { x: -6, y: 2, color: '#ecf0f1', name: 'original' });

    // simplify to 50%
    var mid = hiRes.clone().simplify(0.5);
    addMesh(mid, { x: -2, y: 2, color: '#3498db', name: 'simplify-50' });

    // simplifyToTriangleCount
    var low = hiRes.clone().simplifyToTriangleCount(100);
    addMesh(low, { x: 2, y: 2, color: '#e74c3c', name: 'simplify-100t' });

    // LOD chain
    var lods = hiRes.clone().generateLODChain(new Float32Array([0.5, 0.25, 0.1]));
    for (var i = 0; i < lods.length; i++) {
        addMesh(lods[i], { x: 6, y: 2 + i * 2.5, color: [0.2 + i * 0.3, 0.8 - i * 0.2, 0.3, 1], name: 'lod-' + i });
    }

    log('Original: ' + hiRes.triangleCount + 't | 50%: ' + mid.triangleCount +
        't | 100t target: ' + low.triangleCount + 't | LODs: ' +
        lods.map(function(l) { return l.triangleCount; }).join(', ') + 't');

    addMesh(Mesh.plane(10, 8), { y: -0.01, color: '#2c3e50' });
}

// =============================================================================
// Scene 5: Subdivision + Smoothing
// =============================================================================

function sceneSubdivide() {
    clearScene();
    log('Scene 5: Subdivision and smoothing');

    scene.setCamera({
        fov: 50,
        position: [0, 6, 16],
        target: [0, 1.5, 0],
        aspect: 1024 / 768
    });

    // Start with a low-poly box
    var base = Mesh.box(1, 1, 1);
    addMesh(base.clone(), { x: -6, y: 2, color: '#ecf0f1', name: 'box-original' });

    // Loop subdivision
    var loopSub = base.clone().subdivideLoop(2);
    addMesh(loopSub, { x: -2.5, y: 2, color: '#3498db', name: 'loop-subdiv' });

    // Catmull-Clark subdivision
    var ccSub = base.clone().subdivideCatmullClark(2);
    addMesh(ccSub, { x: 1, y: 2, color: '#2ecc71', name: 'catmull-clark' });

    // Midpoint subdivision
    var midSub = base.clone().subdivideMidpoint(2);
    addMesh(midSub, { x: 4.5, y: 2, color: '#f39c12', name: 'midpoint' });

    // Smoothing demo: sphere with noise, then smooth
    var noisy = Mesh.sphere(1.2, 16, 12);
    var positions = noisy.positions;
    for (var i = 0; i < positions.length; i += 3) {
        var len = Math.sqrt(positions[i]*positions[i] + positions[i+1]*positions[i+1] + positions[i+2]*positions[i+2]);
        if (len > 0) {
            var noise = 1.0 + (Math.random() - 0.5) * 0.4;
            positions[i] *= noise;
            positions[i+1] *= noise;
            positions[i+2] *= noise;
        }
    }
    noisy.positions = positions;
    noisy.computeNormals();
    addMesh(noisy.clone(), { x: -4, y: 5, color: '#e74c3c', name: 'noisy' });

    // Laplacian smooth
    var smoothed = noisy.clone().smoothLaplacian(0.5, 3);
    addMesh(smoothed, { x: -0.5, y: 5, color: '#3498db', name: 'laplacian' });

    // Taubin smooth
    var taubin = noisy.clone().smoothTaubin(0.5, -0.53, 3);
    addMesh(taubin, { x: 3, y: 5, color: '#2ecc71', name: 'taubin' });

    log('Subdiv: base=' + base.triangleCount + 't -> loop=' + loopSub.triangleCount +
        't, cc=' + ccSub.triangleCount + 't, mid=' + midSub.triangleCount + 't');

    addMesh(Mesh.plane(10, 8), { y: -0.01, color: '#2c3e50' });
}

// =============================================================================
// Scene 6: Isosurface (Marching Cubes)
// =============================================================================

function sceneIsosurface() {
    clearScene();
    log('Scene 6: Isosurface extraction — marching cubes + dual contouring');

    scene.setCamera({
        fov: 50,
        position: [0, 8, 20],
        target: [0, 2, 0],
        aspect: 1024 / 768
    });

    // Generate a scalar field: sphere + sine waves
    var gridSize = 32;
    var field = new Float32Array(gridSize * gridSize * gridSize);
    for (var z = 0; z < gridSize; z++) {
        for (var y = 0; y < gridSize; y++) {
            for (var x = 0; x < gridSize; x++) {
                var fx = (x / gridSize - 0.5) * 4;
                var fy = (y / gridSize - 0.5) * 4;
                var fz = (z / gridSize - 0.5) * 4;
                // Sphere SDF + sine perturbation
                var dist = Math.sqrt(fx*fx + fy*fy + fz*fz) - 1.5;
                dist += Math.sin(fx * 3) * 0.15 + Math.sin(fy * 4) * 0.1 + Math.sin(fz * 2.5) * 0.12;
                field[z * gridSize * gridSize + y * gridSize + x] = -dist;
            }
        }
    }

    // Marching cubes
    var cellSize = 4.0 / gridSize;
    var mc = Mesh.marchingCubes(field, gridSize, gridSize, gridSize, 0, cellSize);
    mc.center();
    mc.computeNormals();
    addMesh(mc, { x: -4, y: 3, color: '#3498db', name: 'marching-cubes' });

    // Dual contouring
    var dc = Mesh.dualContour(field, gridSize, gridSize, gridSize, 0, cellSize);
    dc.center();
    dc.computeNormals();
    addMesh(dc, { x: 4, y: 3, color: '#e74c3c', name: 'dual-contour' });

    log('Isosurface (' + gridSize + '^3 grid): marchingCubes=' + mc.triangleCount +
        't, dualContour=' + dc.triangleCount + 't');

    addMesh(Mesh.plane(12, 8), { y: -0.01, color: '#2c3e50' });
}

// =============================================================================
// Scene 7: Analysis (BBox + Raycast + Manifold)
// =============================================================================

function sceneAnalysis() {
    clearScene();
    log('Scene 7: Analysis — bbox, volume, manifold, raycast, intersections');

    scene.setCamera({
        fov: 50,
        position: [0, 8, 18],
        target: [0, 2, 0],
        aspect: 1024 / 768
    });

    // Test mesh
    var sphere = Mesh.sphere(2, 24, 16);

    // BBox
    var bbox = sphere.computeBBox();

    // Volume
    var vol = sphere.computeVolume();
    var expectedVol = (4/3) * Math.PI * 2 * 2 * 2; // 4/3 pi r^3

    // Manifold check
    var manifold = sphere.isManifold();

    // Raycast
    var hit = sphere.raycast([0, 0, 10], [0, 0, -1]);
    var hitAll = sphere.raycastAll([0, 0, 10], [0, 0, -1]);
    var hitTest = sphere.raycastTest([0, 0, 10], [0, 0, -1]);

    // Closest point
    var closest = sphere.closestPoint([5, 0, 0]);

    // Self-intersection check
    var selfIntersect = sphere.hasSelfIntersections();

    // Display the sphere
    addMesh(sphere.clone(), { x: -3, y: 2.5, color: '#3498db', name: 'test-sphere' });

    // Show raycast hit point as small sphere
    if (hit) {
        var marker = Mesh.sphere(0.1, 8, 6);
        addMesh(marker, {
            x: -3 + hit.position[0], y: 2.5 + hit.position[1], z: hit.position[2],
            color: '#e74c3c', emissive: 1, name: 'hit-marker'
        });
    }

    // Show closest point marker
    if (closest) {
        var cpMarker = Mesh.sphere(0.1, 8, 6);
        addMesh(cpMarker, {
            x: -3 + closest.position[0], y: 2.5 + closest.position[1], z: closest.position[2],
            color: '#f39c12', emissive: 1, name: 'closest-marker'
        });
    }

    // Two spheres for intersection test
    var sA = Mesh.sphere(1, 16, 12);
    var sB = Mesh.sphere(1, 16, 12);
    sB.translate(1.5, 0, 0);
    var intersects = sA.intersectsMesh(sB);
    addMesh(sA, { x: 4, y: 2.5, color: '#2ecc71', name: 'intersect-A' });
    addMesh(sB, { x: 4, y: 2.5, color: '#9b59b6', name: 'intersect-B' });

    // Merge demo
    var merged = Mesh.merge([Mesh.box(0.5, 0.5, 0.5), Mesh.sphere(0.3, 12, 8)]);
    addMesh(merged, { x: 0, y: 5, color: '#f39c12', name: 'merged' });

    var lines = [
        'bbox: [' + bbox.min.map(function(v){return v.toFixed(1)}).join(',') + '] to [' + bbox.max.map(function(v){return v.toFixed(1)}).join(',') + ']',
        'volume: ' + vol.toFixed(2) + ' (expected: ' + expectedVol.toFixed(2) + ')',
        'manifold: ' + manifold,
        'raycast hit: ' + (hit ? 'd=' + hit.distance.toFixed(2) + ' tri=' + hit.triangleIndex : 'miss'),
        'raycastAll: ' + hitAll.length + ' hits, raycastTest: ' + hitTest,
        'closest to [5,0,0]: d=' + (closest ? closest.distance.toFixed(2) : 'n/a'),
        'selfIntersect: ' + selfIntersect + ', meshesIntersect: ' + intersects,
        'merged: ' + merged.vertexCount + ' verts, ' + merged.triangleCount + ' tris'
    ];
    log(lines.join(' | '));

    addMesh(Mesh.plane(12, 8), { y: -0.01, color: '#2c3e50' });
}

// =============================================================================
// Scene dispatch + animation
// =============================================================================

var scenes = [scenePrimitives, sceneTransforms, sceneCSG, sceneSimplify, sceneSubdivide, sceneIsosurface, sceneAnalysis];

function loadScene(idx) {
    currentScene = idx;
    scenes[idx]();
}

document.addEventListener('keydown', function(e) {
    var key = e.key;
    if (key >= '1' && key <= '7') {
        loadScene(parseInt(key) - 1);
    } else if (key === ' ') {
        paused = !paused;
    }
});

// Gentle camera orbit
var baseTime = 0;
function animate() {
    if (!paused) {
        time += 16;
        var t = time * 0.001;

        // Slowly orbit camera
        var cam = null;
        if (currentScene === 0) {
            cam = { fov: 50, position: [Math.sin(t*0.3)*18, 8, Math.cos(t*0.3)*18], target: [0,0,0], aspect: 1024/768 };
        } else if (currentScene === 2) {
            cam = { fov: 50, position: [Math.sin(t*0.2)*16, 8, Math.cos(t*0.2)*16], target: [0,1,0], aspect: 1024/768 };
        } else if (currentScene === 5) {
            cam = { fov: 50, position: [Math.sin(t*0.25)*20, 8, Math.cos(t*0.25)*20], target: [0,2,0], aspect: 1024/768 };
        }
        if (cam) scene.setCamera(cam);
    }

    requestAnimationFrame(animate);
}

// Start
log('Mesh API Demo — press 1-7 to switch scenes');
loadScene(0);
animate();
