// Mesh file I/O by path (OBJ / PLY / STL / splat-PLY round trips, VOX + FBX
// loaders present), polygon triangulation, point-cloud reconstruction, and
// the PolyMesh half-edge class. A relative save path lands
// where fs.* would put it, so the round trip goes through a directory the
// test makes with fs and reads back with fs.

const fs = require('fs');
const os = require('os');
const path = require('path');

const tmpDir = path.join(os.tmpdir(), 'bro_mesh_io_' + process.pid);
try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) {}
fs.mkdirSync(tmpDir, { recursive: true });

try {
    // ---- statics exist on Mesh and are forwarded onto bro.mesh -------------
    for (const name of ['loadOBJ', 'loadPLY', 'loadSTL', 'loadVOX', 'loadFBX', 'loadGLTF']) {
        assert(typeof Mesh[name] === 'function', 'Mesh.' + name);
        assert(typeof bro.mesh[name] === 'function', 'bro.mesh.' + name);
    }
    const box = Mesh.box(1, 2, 3);
    for (const name of ['saveOBJ', 'savePLY', 'saveSTL', 'saveGLTF']) {
        assert(typeof box[name] === 'function', 'mesh.' + name);
    }

    // ---- OBJ round trip -------------------------------------------------
    const objPath = path.join(tmpDir, 'box.obj');
    assert(box.saveOBJ(objPath) === true, 'saveOBJ returns true');
    assert(fs.existsSync(objPath), 'saveOBJ wrote the file');
    const objText = fs.readFileSync(objPath, 'utf-8');
    assert(/^v /m.test(objText) && /^f /m.test(objText), 'OBJ has v and f records');
    const objBack = Mesh.loadOBJ(objPath);
    assert(objBack instanceof Mesh, 'loadOBJ gives a Mesh');
    assert(objBack.triangleCount === box.triangleCount,
        'OBJ round trip keeps the triangle count: ' + objBack.triangleCount + ' vs ' + box.triangleCount);
    const bb = objBack.bounds();
    assert(Math.abs(bb.max[0] - bb.min[0] - 2) < 1e-4 && Math.abs(bb.max[2] - bb.min[2] - 6) < 1e-4,
        'OBJ round trip keeps the extents: ' + JSON.stringify(bb));

    // ---- PLY + STL round trips ------------------------------------------
    const plyPath = path.join(tmpDir, 'box.ply');
    assert(box.savePLY(plyPath) === true && fs.existsSync(plyPath), 'savePLY wrote the file');
    const plyBack = Mesh.loadPLY(plyPath);
    assert(plyBack.triangleCount === box.triangleCount, 'PLY round trip keeps the triangle count');

    const stlPath = path.join(tmpDir, 'box.stl');
    assert(box.saveSTL(stlPath) === true && fs.existsSync(stlPath), 'saveSTL wrote the file');
    const stlBack = Mesh.loadSTL(stlPath);
    assert(stlBack.triangleCount === box.triangleCount, 'STL round trip keeps the triangle count');

    // ---- a missing file is an empty result, not a throw --------------------
    const missing = Mesh.loadOBJ(path.join(tmpDir, 'nope.obj'));
    assert(missing instanceof Mesh && missing.vertexCount === 0, 'loadOBJ of a missing file is an empty Mesh');
    const fbx = Mesh.loadFBX(path.join(tmpDir, 'nope.fbx'));
    assert(Array.isArray(fbx) && fbx.length === 0, 'loadFBX of a missing file is an empty list');
    const vox = Mesh.loadVOX(path.join(tmpDir, 'nope.vox'));
    assert(vox && vox.sizeX === 0 && vox.voxels instanceof Uint8Array && vox.palette instanceof Float32Array
        && vox.palette.length === 1024, 'loadVOX shape: ' + JSON.stringify(Object.keys(vox)));

    // ---- splat clouds: a plain object in, the same shape out ----------------
    for (const name of ['loadSplatPLY', 'saveSplatPLY', 'polygon2D', 'polygon3D', 'reconstruct']) {
        assert(typeof Mesh[name] === 'function' && typeof bro.mesh[name] === 'function', 'Mesh.' + name);
    }
    const cloud = {
        positions: new Float32Array([0, 0, 0,  1, 0, 0,  0, 1, 0]),
        scales: new Float32Array([0.1, 0.1, 0.1,  0.2, 0.2, 0.2,  0.3, 0.3, 0.3]),
        rotations: new Float32Array([0, 0, 0, 1,  0, 0, 0, 1,  0, 0, 0, 1]),
        opacities: new Float32Array([1, 0.5, 0.25]),
        sh: new Float32Array(9),
        shDegree: 0,
    };
    const splatPath = path.join(tmpDir, 'cloud.ply');
    assert(Mesh.saveSplatPLY(splatPath, cloud) === true && fs.existsSync(splatPath), 'saveSplatPLY wrote the file');
    const cloudBack = Mesh.loadSplatPLY(splatPath);
    assert(cloudBack.count === 3 && cloudBack.positions instanceof Float32Array && cloudBack.positions.length === 9,
        'loadSplatPLY round trip: ' + JSON.stringify(Object.keys(cloudBack)) + ' count=' + cloudBack.count);
    assert(Math.abs(cloudBack.opacities[1] - 0.5) < 1e-5 && cloudBack.rotations.length === 12, 'splat streams survive');
    assert(Mesh.loadSplatPLY(path.join(tmpDir, 'nope.ply')).count === 0, 'missing splat file is an empty cloud');
    let threw = false;
    try { Mesh.saveSplatPLY(splatPath, { positions: new Float32Array(3), opacities: new Float32Array(2) }); } catch (e) { threw = true; }
    assert(threw, 'saveSplatPLY rejects mismatched streams');

    // ---- polygon triangulation --------------------------------------------
    const square = Mesh.polygon2D([0, 0,  1, 0,  1, 1,  0, 1]);
    assert(square instanceof Mesh && square.triangleCount === 2 && square.vertexCount === 4, 'polygon2D square: ' + square.triangleCount);
    const ring = Mesh.polygon2D([0, 0,  4, 0,  4, 4,  0, 4], [[1, 1,  1, 3,  3, 3,  3, 1]], 2);
    assert(ring.triangleCount === 8 && ring.vertexCount === 8, 'polygon2D with a hole: ' + ring.triangleCount);
    assert(Math.abs(ring.positions[2] - 2) < 1e-6, 'polygon2D z applied');
    const tri3 = Mesh.polygon3D([0, 0, 0,  2, 0, 0,  2, 0, 2], [], [0, 1, 0]);
    assert(tri3.triangleCount === 1 && tri3.vertexCount === 3, 'polygon3D triangle');
    assert(Math.abs(Math.abs(tri3.normals[1]) - 1) < 1e-6, 'polygon3D fills normals with the plane normal');
    assert(Mesh.polygon2D([0, 0,  1, 1]).triangleCount === 0, 'a 2-point outline is an empty Mesh');

    // ---- point-cloud reconstruction ---------------------------------------
    const points = Mesh.sphere(1, 24, 16);
    points.computeNormals();
    const surf = Mesh.reconstruct(points, { gridResolution: 24 });
    assert(surf instanceof Mesh && surf.triangleCount > 100, 'reconstruct a sphere point cloud: ' + surf.triangleCount);
    const sb = surf.bounds();
    assert(sb.max[0] - sb.min[0] > 1.5 && sb.max[0] - sb.min[0] < 2.6, 'reconstructed extents: ' + JSON.stringify(sb));

    // ---- a relative path resolves like fs.* (against the app dir) ----------
    const relDir = 'mesh_io_rel_' + process.pid;
    fs.mkdirSync(relDir, { recursive: true });
    try {
        const rel = relDir + '/rel.obj';
        assert(box.saveOBJ(rel) === true, 'saveOBJ with a relative path');
        assert(fs.existsSync(rel), 'the relative save landed where fs sees it');
        assert(Mesh.loadOBJ(rel).triangleCount === box.triangleCount, 'loadOBJ with a relative path');
    } finally {
        try { fs.rmSync(relDir, { recursive: true, force: true }); } catch (e) {}
    }

    // ---- PolyMesh ----------------------------------------------------------
    assert(typeof PolyMesh === 'function' && bro.mesh.PolyMesh === PolyMesh, 'PolyMesh is a global and on bro.mesh');
    const empty = new PolyMesh();
    assert(empty instanceof PolyMesh && empty.faceCount === 0 && empty.vertexCount === 0, 'empty PolyMesh');
    const et = empty.tessellate();
    assert(et.indices instanceof Uint32Array && et.indices.length === 0, 'empty tessellation');

    // Two triangles making one quad, both tagged group 7: merged back into
    // one 4-gon, tessellated to two triangles again.
    const P = new Float32Array([0, 0, 0,  1, 0, 0,  1, 1, 0,  0, 1, 0]);
    const I = new Uint32Array([0, 1, 2,  0, 2, 3]);
    const pm = PolyMesh.fromMeshData(P, I, new Int32Array([7, 7]));
    assert(pm.faceCount === 2 && pm.vertexCount === 4, 'fromMeshData: 2 faces, 4 verts');
    assert(pm.faceVertexCount(0) === 3, 'faces start as triangles');
    assert(pm.faceGroup(0) === 7 && pm.faceGroup(1) === 7, 'groups carried');
    assert(pm.facesInGroup(7).length === 2, 'facesInGroup');
    pm.mergeFacesByGroup();
    pm.compact();
    assert(pm.faceCount === 1, 'merged to one face, got ' + pm.faceCount);
    assert(pm.faceVertexCount(0) === 4, 'merged face is a quad');
    assert(pm.faceVertices(0).length === 4, 'faceVertices of the quad');
    const n = pm.computeFaceNormal(0);
    assert(Math.abs(Math.abs(n[2]) - 1) < 1e-5, 'quad normal is +-Z: ' + JSON.stringify(n));
    const v = pm.validate();
    assert(v.valid === true && typeof v.isClosed === 'boolean' && Array.isArray(v.errors), 'validate shape');
    const t = pm.tessellate();
    assert(t.indices.length === 6 && t.positions.length === 12 && t.normals.length === 12,
        'quad tessellates to 2 triangles');
    assert(t.triToFace instanceof Int32Array && t.triToFace.length === 2 && t.triToGroup[0] === 7,
        'triToFace / triToGroup');
    const asMesh = pm.toMesh();
    assert(asMesh instanceof Mesh && asMesh.triangleCount === 2, 'toMesh');
    const loops = pm.findGroupBoundary(7);
    assert(loops.length === 1 && loops[0].length === 4, 'group boundary is one 4-loop');
    const fromMesh = PolyMesh.fromMesh(Mesh.box(1, 1, 1));
    assert(fromMesh.faceCount === 12, 'fromMesh: a box is 12 triangles');
    assert(pm.getVertex(2).length === 3 && pm.getVertex(2)[0] === 1, 'getVertex');

    // Surgery: extrude the quad into a slab.
    const r = pm.extrudeFace(0, [0, 0, 1], true);
    assert(r.bridgeFaces instanceof Int32Array && r.bridgeFaces.length === 4, 'extrude: 4 bridge faces');
    assert(r.backFace >= 0, 'extrude: back face made');
    assert(pm.faceCount === 6, 'extruded quad is a closed box of 6 faces, got ' + pm.faceCount);
    assert(pm.validate().isClosed === true, 'extruded slab is closed');

    const poly = PolyMesh.fromPolygon(new Float32Array([0, 0, 0,  2, 0, 0,  2, 2, 0,  1, 3, 0,  0, 2, 0]), [0, 0, 1], 3);
    assert(poly.faceCount === 1 && poly.faceVertexCount(0) === 5 && poly.faceGroup(0) === 3, 'fromPolygon pentagon');
    assert(poly.tessellate().indices.length === 9, 'pentagon tessellates to 3 triangles');
    const pi = poly.insetFace(0, 0.25, true);
    assert(pi.innerFace >= 0 && pi.bridgeFaces.length === 5, 'insetFace: ' + JSON.stringify(pi.innerFace));
} finally {
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) {}
}
