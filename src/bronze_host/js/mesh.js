// mesh.js — the public `Mesh` and `MeshBVH` classes and `bro.mesh.*`
// (docs/mesh-api.js), assembled over the natives under `__bro_native.mesh`
// (src/bronze_host/native_mesh.cpp; host_natives.h states the convention
// and the class arrangement).
//
// `Mesh` is a JavaScript class whose constructor RETURNS the native handle
// `new __bro_native.mesh.Mesh(...)` makes, and whose prototype sits above
// the native class's own prototype (published by C++ as
// `__bro_native.mesh.MeshPrototype`). So a handle — from the constructor,
// from `Mesh.box()`, from `clone()` — is an instance: `instanceof Mesh`,
// the accessors and methods below, one object rather than a wrapper around
// a pointer. Every member calls its native by the FULL dotted path with
// `this` as the class-typed first argument, which the runtime checks by
// tag: a plain object or a MeshBVH where a Mesh is required is a TypeError
// naming the class, from the native call itself.
//
// Attribute arrays (`positions` ...) are COPIES: the mesh's vectors are its
// own and the next operation may reallocate them, so writing into a
// returned array changes nothing; assign the property to write back.
// Fresh results (`triangleAreas`, `computeUVDistortion`, `buildMeshlets`,
// `encodeDraco`) are the native's own block, viewed in place.
(function () {
    'use strict';

    const N = 'Mesh';  // for messages only; natives are spelled in full below

    const f32 = (v) => (v === undefined || v === null) ? new Float32Array(0) : v;
    const u32 = (v) => (v === undefined || v === null) ? new Uint32Array(0) : v;
    const num = (v, d) => (v === undefined ? d : +v);
    const int = (v, d) => (v === undefined ? d : (v | 0));
    const flat3 = (arr, what) => {
        // A flat xyz list, or a list of [x, y, z] / {x, y, z}.
        if (arr instanceof Float32Array) return arr;
        if (!Array.isArray(arr)) throw new TypeError(N + '.' + what + ': expected an array of points');
        if (arr.length === 0 || typeof arr[0] === 'number') return Float32Array.from(arr);
        const out = new Float32Array(arr.length * 3);
        for (let i = 0; i < arr.length; i++) {
            const p = arr[i];
            if (Array.isArray(p)) { out[i * 3] = +p[0]; out[i * 3 + 1] = +p[1]; out[i * 3 + 2] = +p[2]; }
            else { out[i * 3] = +p.x; out[i * 3 + 1] = +p.y; out[i * 3 + 2] = +p.z; }
        }
        return out;
    };
    const vec3 = (v, what) => {
        if (v === undefined || v === null) throw new TypeError(N + '.' + what + ': expected [x, y, z]');
        if (Array.isArray(v) || ArrayBuffer.isView(v)) return [+v[0], +v[1], +v[2]];
        return [+v.x, +v.y, +v.z];
    };
    const parseHit = (s) => (s === '' ? null : JSON.parse(s));

    const projections = { box: 0, planarXY: 1, planarXZ: 2, planarYZ: 3, cylindrical: 4, spherical: 5 };

    class Mesh {
        /** @param {MeshOptions} [opts] */
        constructor(opts) {
            const o = opts || {};
            return new __bro_native.mesh.Mesh(f32(o.positions), f32(o.normals), f32(o.uvs),
                                              f32(o.colors), u32(o.indices));
        }

        // ---- attributes (copies; assign to write back) -----------------------
        get positions() { return __bro_native.mesh.positions(this); }
        set positions(v) { __bro_native.mesh.setPositions(this, f32(v)); }
        get normals() { return __bro_native.mesh.normals(this); }
        set normals(v) { __bro_native.mesh.setNormals(this, f32(v)); }
        get uvs() { return __bro_native.mesh.uvs(this); }
        set uvs(v) { __bro_native.mesh.setUVs(this, f32(v)); }
        get colors() { return __bro_native.mesh.colors(this); }
        set colors(v) { __bro_native.mesh.setColors(this, f32(v)); }
        get indices() { return __bro_native.mesh.indices(this); }
        set indices(v) { __bro_native.mesh.setIndices(this, u32(v)); }

        get vertexCount() { return __bro_native.mesh.vertexCount(this); }
        get triangleCount() { return __bro_native.mesh.triangleCount(this); }
        get hasNormals() { return __bro_native.mesh.hasNormals(this); }
        get hasUVs() { return __bro_native.mesh.hasUVs(this); }
        get hasColors() { return __bro_native.mesh.hasColors(this); }
        get empty() { return __bro_native.mesh.empty(this); }

        // ---- primitives --------------------------------------------------------
        static box(halfW, halfH, halfD) {
            const w = num(halfW, 0.5);
            return __bro_native.mesh.box(w, num(halfH, w), num(halfD, w));
        }
        static sphere(radius, segments, rings) {
            return __bro_native.mesh.sphere(num(radius, 1), int(segments, 16), int(rings, 12));
        }
        static cylinder(radius, halfHeight, segments) {
            return __bro_native.mesh.cylinder(num(radius, 0.5), num(halfHeight, 1), int(segments, 16));
        }
        static capsule(radius, halfHeight, segments) {
            return __bro_native.mesh.capsule(num(radius, 0.5), num(halfHeight, 1), int(segments, 16), 8);
        }
        static cone(radius, height, segments) {
            return __bro_native.mesh.cone(num(radius, 0.5), num(height, 1), int(segments, 16), 4);
        }
        static plane(halfW, halfH, segW, segH) {
            const w = num(halfW, 1);
            return __bro_native.mesh.plane(w, num(halfH, w), int(segW, 1), int(segH, 1));
        }
        static torus(radius, tubeRadius, segments, tubeSegments) {
            return __bro_native.mesh.torus(num(radius, 1), num(tubeRadius, 0.3),
                                           int(segments, 24), int(tubeSegments, 12));
        }
        static icosahedron(radius) { return __bro_native.mesh.icosahedron(num(radius, 1)); }
        static dodecahedron(radius) { return __bro_native.mesh.dodecahedron(num(radius, 1)); }
        static octahedron(radius) { return __bro_native.mesh.octahedron(num(radius, 1)); }
        static tetrahedron(radius) { return __bro_native.mesh.tetrahedron(num(radius, 1)); }
        static disk(radius, segments) { return __bro_native.mesh.disk(num(radius, 1), int(segments, 16)); }
        static geodesicSphere(radius, subdivisions) {
            return __bro_native.mesh.geodesicSphere(num(radius, 1), int(subdivisions, 2));
        }
        static rock(radius, seed, subdivisions) {
            return __bro_native.mesh.rock(num(radius, 1), int(seed, 42), int(subdivisions, 2));
        }
        /** @param {MeshBlobOptions} [opts] */
        static blob(opts) {
            const o = opts || {};
            const s = o.scale === undefined ? [1, 1, 1] : (typeof o.scale === 'number' ? [o.scale, o.scale, o.scale] : vec3(o.scale, 'blob'));
            const c = o.center === undefined ? [0, 0, 0] : vec3(o.center, 'blob');
            return __bro_native.mesh.blob(num(o.radius, 1), int(o.seed, 42), int(o.nsub, 2),
                                          s[0], s[1], s[2], c[0], c[1], c[2]);
        }
        static tube(points, radius, segments) {
            return __bro_native.mesh.tube(flat3(points, 'tube'), num(radius, 0.1), int(segments, 8));
        }
        /** @param {Array<Mesh>} meshes */
        static merge(meshes) {
            __bro_native.mesh.mergeBegin();
            for (let i = 0; i < meshes.length; i++) __bro_native.mesh.mergeAdd(meshes[i]);
            return __bro_native.mesh.mergeEnd();
        }
        static marchingCubes(values, dimX, dimY, dimZ, isoLevel) {
            return __bro_native.mesh.marchingCubes(values instanceof Float32Array ? values : Float32Array.from(values),
                                                   dimX | 0, dimY | 0, dimZ | 0, num(isoLevel, 0));
        }
        static surfaceNets(values, dimX, dimY, dimZ, isoLevel) {
            return __bro_native.mesh.surfaceNets(values instanceof Float32Array ? values : Float32Array.from(values),
                                                 dimX | 0, dimY | 0, dimZ | 0, num(isoLevel, 0));
        }
        static dualContouring(values, dimX, dimY, dimZ, isoLevel) {
            return __bro_native.mesh.dualContouring(values instanceof Float32Array ? values : Float32Array.from(values),
                                                    dimX | 0, dimY | 0, dimZ | 0, num(isoLevel, 0));
        }
        /** @param {ArrayBufferView} bytes @returns {MeshDracoDecoded} */
        static decodeDraco(bytes) {
            const u8 = bytes instanceof Uint8Array ? bytes
                     : ArrayBuffer.isView(bytes) ? new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)
                     : new Uint8Array(bytes);
            const err = __bro_native.mesh.dracoDecode(u8);
            if (err !== '') throw new Error('Mesh.decodeDraco: ' + err);
            const mesh = __bro_native.mesh.dracoTakeMesh();
            const kinds = [Float32Array, Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];
            const kindNames = ['float32', 'int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32'];
            const attributes = [];
            const n = __bro_native.mesh.dracoAttributeCount();
            for (let i = 0; i < n; i++) {
                const info = JSON.parse(__bro_native.mesh.dracoAttributeInfo(i));
                const raw = __bro_native.mesh.dracoAttributeBytes(i);
                const T = kinds[info.kind] || Uint8Array;
                attributes.push({
                    type: info.type, uniqueId: info.uniqueId, components: info.components,
                    count: info.count, kind: kindNames[info.kind] || 'uint8',
                    data: new T(raw.buffer, 0, raw.byteLength / T.BYTES_PER_ELEMENT),
                });
            }
            return { positions: mesh.positions, normals: mesh.normals, uvs: mesh.uvs, colors: mesh.colors,
                     indices: mesh.indices, attributes, mesh };
        }
        /** @param {Object|Mesh} meshData @param {MeshDracoEncodeOptions} [opts] @returns {ArrayBuffer} */
        static encodeDraco(meshData, opts) {
            const m = meshData instanceof Mesh ? meshData : new Mesh(meshData);
            const o = opts || {};
            const bytes = __bro_native.mesh.dracoEncode(m, int(o.positionBits, 14), int(o.normalBits, 10),
                                                         int(o.uvBits, 12), int(o.colorBits, 8),
                                                         int(o.compressionLevel, 7));
            return bytes;
        }

        // ---- in-place operations (chainable) ------------------------------------
        clone() { return __bro_native.mesh.clone(this); }
        translate(dx, dy, dz) { __bro_native.mesh.translate(this, num(dx, 0), num(dy, 0), num(dz, 0)); return this; }
        scale(sx, sy, sz) {
            const x = num(sx, 1);
            __bro_native.mesh.scale(this, x, num(sy, x), num(sz, x));
            return this;
        }
        rotate(ax, ay, az, angle) {
            __bro_native.mesh.rotate(this, num(ax, 0), num(ay, 1), num(az, 0), num(angle, 0));
            return this;
        }
        center() { __bro_native.mesh.center(this); return this; }
        fitToBox(size) { __bro_native.mesh.fitToBox(this, num(size, 1)); return this; }
        /** @param {Array<number>} matrix 16 numbers, column-major */
        transform(matrix) {
            __bro_native.mesh.transform(this, matrix instanceof Float32Array ? matrix : Float32Array.from(matrix));
            return this;
        }
        computeNormals(creaseAngle) { __bro_native.mesh.computeNormals(this, num(creaseAngle, 0)); return this; }
        invertNormals() { __bro_native.mesh.invertNormals(this); return this; }
        flipFaces() { __bro_native.mesh.flipFaces(this); return this; }
        weld(threshold) { __bro_native.mesh.weld(this, num(threshold, 1e-5)); return this; }
        simplify(ratio, targetError) {
            __bro_native.mesh.simplify(this, num(ratio, 0.5), num(targetError, 0.01));
            return this;
        }
        subdivideLoop(iterations) { __bro_native.mesh.subdivideLoop(this, int(iterations, 1)); return this; }
        subdivideCatmullClark(iterations) {
            __bro_native.mesh.subdivideCatmullClark(this, int(iterations, 1));
            return this;
        }
        smooth(lambda, iterations) {
            __bro_native.mesh.smooth(this, num(lambda, 0.5), int(iterations, 1));
            return this;
        }
        remesh(targetEdgeLength) { __bro_native.mesh.remesh(this, num(targetEdgeLength, 0)); return this; }
        repair() { __bro_native.mesh.repair(this); return this; }
        optimize() { __bro_native.mesh.optimize(this); return this; }

        /** @returns {Array<Mesh>} */
        splitComponents() {
            const n = __bro_native.mesh.splitComponents(this);
            const out = [];
            for (let i = 0; i < n; i++) out.push(__bro_native.mesh.takePiece(i));
            return out;
        }
        convexHull() { return __bro_native.mesh.convexHull(this); }
        /** @param {MeshConvexDecompParams} [params] @returns {Array<Mesh>} */
        convexDecomposition(params) {
            const p = params || {};
            const n = __bro_native.mesh.convexDecomposition(this, int(p.maxHulls, 0), int(p.maxVerticesPerHull, 0),
                                                            num(p.resolution, 0), num(p.minVolumePerHull, 0));
            const out = [];
            for (let i = 0; i < n; i++) out.push(__bro_native.mesh.takePiece(i));
            return out;
        }
        booleanUnion(other) { return __bro_native.mesh.booleanUnion(this, other); }
        booleanDifference(other) { return __bro_native.mesh.booleanDifference(this, other); }
        booleanIntersection(other) { return __bro_native.mesh.booleanIntersection(this, other); }
        csgUnion(other) { return __bro_native.mesh.booleanUnion(this, other); }
        csgSubtract(other) { return __bro_native.mesh.booleanDifference(this, other); }
        csgIntersect(other) { return __bro_native.mesh.booleanIntersection(this, other); }

        // ---- UVs -------------------------------------------------------------------
        /** @param {string} [method] 'unwrap' (xatlas), or a projection name */
        generateUVs(method) {
            const m = method === undefined ? 'unwrap' : String(method);
            if (m === 'unwrap' || m === 'xatlas') {
                __bro_native.mesh.unwrapUVs(this);
            } else if (projections[m] !== undefined) {
                __bro_native.mesh.projectUVs(this, projections[m], 1);
            } else {
                throw new TypeError('Mesh.generateUVs: unknown method "' + m + '"');
            }
            return this;
        }
        /** @param {string} [projection] box|planarXY|planarXZ|planarYZ|cylindrical|spherical */
        projectUVs(projection) {
            const p = projection === undefined ? 'box' : String(projection);
            if (projections[p] === undefined) throw new TypeError('Mesh.projectUVs: unknown projection "' + p + '"');
            __bro_native.mesh.projectUVs(this, projections[p], 1);
            return this;
        }
        /** @returns {Array<MeshUVDistortionResult>} */
        computeUVDistortion() {
            const flat = __bro_native.mesh.computeUVDistortion(this);
            const out = [];
            for (let i = 0; i + 2 < flat.length; i += 3) {
                out.push({ stretch: flat[i], areaDistortion: flat[i + 1], angleDistortion: flat[i + 2] });
            }
            return out;
        }
        /** @returns {MeshUVQualityResult} bromesh's UVMetrics, with `coverage` and `avgAngleError` as the doc names them */
        measureUVQuality() {
            const q = JSON.parse(__bro_native.mesh.measureUVQuality(this));
            q.coverage = q.uvSpaceUsage;
            q.avgAngleError = q.avgAngleDistortion;
            q.maxAreaRatio = q.maxAreaDistortion;
            return q;
        }

        // ---- analysis ---------------------------------------------------------------
        /** @returns {{min: Array<number>, max: Array<number>}} */
        bounds() {
            const b = __bro_native.mesh.bounds(this);
            return { min: [b[0], b[1], b[2]], max: [b[3], b[4], b[5]] };
        }
        surfaceArea() { return __bro_native.mesh.surfaceArea(this); }
        volume() { return __bro_native.mesh.volume(this); }
        isManifold() { return __bro_native.mesh.isManifold(this); }
        /** @returns {Float32Array} one area per triangle (the native's own block) */
        triangleAreas() { return __bro_native.mesh.triangleAreas(this); }
        /** @returns {MeshBVHIntersectResult|null} */
        raycast(origin, direction, maxDist) {
            const o = vec3(origin, 'raycast'), d = vec3(direction, 'raycast');
            return parseHit(__bro_native.mesh.raycast(this, o[0], o[1], o[2], d[0], d[1], d[2], num(maxDist, 0)));
        }
        buildBVH(leafSize) { return new MeshBVH(this, leafSize); }
        /** @returns {MeshletGroupResult} `meshlets` holds one 64-byte record per meshlet:
         *  u32 vertexOffset, vertexCount, triangleOffset (into `triangles`), triangleCount;
         *  f32 center[3], radius, coneApex[3], coneAxis[3], coneCutoff; u32 pad. */
        buildMeshlets(maxVertices, maxTriangles) {
            const count = __bro_native.mesh.buildMeshlets(this, int(maxVertices, 64), int(maxTriangles, 124), 0.5);
            const vertices = __bro_native.mesh.meshletVertices();
            const triangles = __bro_native.mesh.meshletTriangles();
            const records = __bro_native.mesh.meshletRecords();
            return { meshletCount: count, meshlets: records.buffer, vertices, triangles };
        }
    }

    class MeshBVH {
        /** @param {Mesh} mesh @param {number} [leafSize] */
        constructor(mesh, leafSize) {
            return new __bro_native.mesh.MeshBVH(mesh, int(leafSize, 8));
        }
        get empty() { return __bro_native.mesh.bvhEmpty(this); }
        get nodeCount() { return __bro_native.mesh.bvhNodeCount(this); }
        get triangleCount() { return __bro_native.mesh.bvhTriangleCount(this); }
        bounds() {
            const b = __bro_native.mesh.bvhBounds(this);
            return { min: [b[0], b[1], b[2]], max: [b[3], b[4], b[5]] };
        }
        /** @returns {MeshBVHIntersectResult|null} */
        raycast(origin, direction, maxDist) {
            const o = vec3(origin, 'raycast'), d = vec3(direction, 'raycast');
            return parseHit(__bro_native.mesh.bvhRaycast(this, o[0], o[1], o[2], d[0], d[1], d[2], num(maxDist, 0)));
        }
        raycastTest(origin, direction, maxDist) {
            const o = vec3(origin, 'raycast'), d = vec3(direction, 'raycast');
            return __bro_native.mesh.bvhRaycastTest(this, o[0], o[1], o[2], d[0], d[1], d[2], num(maxDist, 0));
        }
    }

    // The native prototypes under the public ones: every handle the natives
    // make is thereby an instance of the public class.
    Object.setPrototypeOf(__bro_native.mesh.MeshPrototype, Mesh.prototype);
    Object.setPrototypeOf(__bro_native.mesh.MeshBVHPrototype, MeshBVH.prototype);

    const ns = bro.mesh;
    ns.Mesh = Mesh;
    ns.MeshBVH = MeshBVH;
    const factories = ['box', 'sphere', 'cylinder', 'capsule', 'cone', 'plane', 'torus', 'icosahedron',
                       'dodecahedron', 'octahedron', 'tetrahedron', 'disk', 'geodesicSphere', 'rock', 'blob',
                       'tube', 'merge', 'marchingCubes', 'surfaceNets', 'dualContouring', 'decodeDraco',
                       'encodeDraco'];
    for (let i = 0; i < factories.length; i++) ns[factories[i]] = Mesh[factories[i]];

    globalThis.Mesh = Mesh;
    globalThis.MeshBVH = MeshBVH;
})();
