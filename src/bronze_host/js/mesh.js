// mesh.js — the public `Mesh`, `MeshBVH`, and `ProgressiveMesh` classes and `bro.mesh.*`
// (docs/mesh-api.js), assembled over the natives under `__bro_native.mesh`
// (src/bronze_host/native_mesh_*.cpp; host_natives.h states the convention
// and the class arrangement).
(function () {
    'use strict';

    const N = 'Mesh';

    const f32 = (v) => (v === undefined || v === null) ? new Float32Array(0) : (v instanceof Float32Array ? v : Float32Array.from(v));
    const u32 = (v) => (v === undefined || v === null) ? new Uint32Array(0) : (v instanceof Uint32Array ? v : Uint32Array.from(v));
    const u8  = (v) => (v === undefined || v === null) ? new Uint8Array(0) : (v instanceof Uint8Array ? v : new Uint8Array(v.buffer || v, v.byteOffset || 0, v.byteLength || v.length));
    const i32 = (v) => (v === undefined || v === null) ? new Int32Array(0) : (v instanceof Int32Array ? v : Int32Array.from(v));
    const num = (v, d) => (v === undefined ? d : +v);
    const int = (v, d) => (v === undefined ? d : (v | 0));

    const flat3 = (arr, what) => {
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

    const parseHit = (s) => (s === '' || !s ? null : JSON.parse(s));

    const readTexBuffer = () => {
        const w = __bro_native.mesh.texBufferWidth();
        const h = __bro_native.mesh.texBufferHeight();
        const ch = __bro_native.mesh.texBufferChannels();
        const px = __bro_native.mesh.texBufferPixels();
        return { width: w, height: h, channels: ch, pixels: px };
    };

    const projections = { box: 0, planarXY: 1, planarXZ: 2, planarYZ: 3, cylindrical: 4, spherical: 5 };

    class Mesh {
        /** @param {MeshOptions} [opts] */
        constructor(opts) {
            const o = opts || {};
            return new __bro_native.mesh.Mesh(f32(o.positions), f32(o.normals), f32(o.uvs),
                                              f32(o.colors), u32(o.indices));
        }

        // ---- Attributes (copies; assign to write back) -----------------------
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

        // ---- Primitives --------------------------------------------------------
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
        static capsule(radius, halfHeight, segments, rings) {
            return __bro_native.mesh.capsule(num(radius, 0.5), num(halfHeight, 1), int(segments, 16), int(rings, 8));
        }
        static cone(radius, height, segments, stacks) {
            return __bro_native.mesh.cone(num(radius, 0.5), num(height, 1), int(segments, 16), int(stacks, 4));
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
        static disc(radius, segments) { return Mesh.disk(radius, segments); }
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
        static heightmapGrid(heights, width, depth, scale, border) {
            return __bro_native.mesh.heightmapGrid(f32(heights), int(width, 0), int(depth, 0),
                                                   num(scale, 1), int(border, 0));
        }
        /** @param {Array<Mesh>} meshes */
        static merge(meshes) {
            __bro_native.mesh.mergeBegin();
            for (let i = 0; i < meshes.length; i++) __bro_native.mesh.mergeAdd(meshes[i]);
            return __bro_native.mesh.mergeEnd();
        }
        static marchingCubes(values, dimX, dimY, dimZ, isoLevel) {
            return __bro_native.mesh.marchingCubes(f32(values), dimX | 0, dimY | 0, dimZ | 0, num(isoLevel, 0));
        }
        static surfaceNets(values, dimX, dimY, dimZ, isoLevel) {
            return __bro_native.mesh.surfaceNets(f32(values), dimX | 0, dimY | 0, dimZ | 0, num(isoLevel, 0));
        }
        static dualContouring(values, dimX, dimY, dimZ, isoLevel) {
            return __bro_native.mesh.dualContouring(f32(values), dimX | 0, dimY | 0, dimZ | 0, num(isoLevel, 0));
        }
        static dualContour(values, dimX, dimY, dimZ, isoLevel) {
            return Mesh.dualContouring(values, dimX, dimY, dimZ, isoLevel);
        }
        static transvoxel(values, gridSize, lod, neighborLods, isoLevel, cellSize) {
            return __bro_native.mesh.transvoxel(f32(values), int(gridSize, 16), int(lod, 0),
                                                i32(neighborLods), num(isoLevel, 0), num(cellSize, 1));
        }
        static greedyMesh(voxels, gx, gy, gz, cellSize) {
            return __bro_native.mesh.greedyMesh(u8(voxels), int(gx, 0), int(gy, 0), int(gz, 0), num(cellSize, 1));
        }
        static stripify(indices, vertexCount) {
            return __bro_native.mesh.stripify(u32(indices), int(vertexCount, 0));
        }
        static unstripify(strip) {
            return __bro_native.mesh.unstripify(u32(strip));
        }
        /** @param {ArrayBufferView} bytes @returns {MeshDracoDecoded} */
        static decodeDraco(bytes) {
            const b = u8(bytes);
            const err = __bro_native.mesh.dracoDecode(b);
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
        static decode(enc) {
            const vdata = u8(enc.vertexData);
            const idata = u8(enc.indexData);
            let hasNormals = enc.hasNormals;
            let hasUVs = enc.hasUVs;
            let hasColors = enc.hasColors;
            const stride = int(enc.vertexSize, 0);
            if (hasNormals === undefined && hasUVs === undefined && hasColors === undefined && stride > 0) {
                if (stride === 24) { hasNormals = true; }
                else if (stride === 32) { hasNormals = true; hasUVs = true; }
                else if (stride === 48) { hasNormals = true; hasUVs = true; hasColors = true; }
                else if (stride === 20) { hasUVs = true; }
                else if (stride === 28) { hasColors = true; }
            }
            return __bro_native.mesh.decodeMesh(
                vdata, idata,
                int(enc.vertexCount, 0), stride, int(enc.indexCount, 0),
                Boolean(hasNormals), Boolean(hasUVs), Boolean(hasColors)
            );
        }
        static splitByPlane(mesh, nx, ny, nz, d) {
            __bro_native.mesh.splitByPlane(mesh, num(nx, 0), num(ny, 1), num(nz, 0), num(d, 0));
            return [__bro_native.mesh.takeSplit(0), __bro_native.mesh.takeSplit(1)];
        }
        static booleanUnion(a, b) { return __bro_native.mesh.booleanUnion(a, b); }
        static booleanDifference(a, b) { return __bro_native.mesh.booleanDifference(a, b); }
        static booleanIntersection(a, b) { return __bro_native.mesh.booleanIntersection(a, b); }
        static union(a, b) { return Mesh.booleanUnion(a, b); }
        static subtract(a, b) { return Mesh.booleanDifference(a, b); }
        static intersect(a, b) { return Mesh.booleanIntersection(a, b); }

        // ---- In-place operations (chainable) ------------------------------------
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
        mirror(axis) { __bro_native.mesh.mirror(this, int(axis, 0)); return this; }
        computeNormals(creaseAngle) { __bro_native.mesh.computeNormals(this, num(creaseAngle, 0)); return this; }
        computeFlatNormals() { return __bro_native.mesh.computeFlatNormals(this); }
        computeCreaseNormals(angle) { __bro_native.mesh.computeCreaseNormals(this, num(angle, 30)); return this; }
        invertNormals() { __bro_native.mesh.invertNormals(this); return this; }
        flipFaces() { __bro_native.mesh.flipFaces(this); return this; }
        weld(threshold) { __bro_native.mesh.weld(this, num(threshold, 1e-5)); return this; }
        removeDegenerateTriangles(eps) { __bro_native.mesh.removeDegenerateTriangles(this, num(eps, 1e-8)); return this; }
        removeDuplicateTriangles() { __bro_native.mesh.removeDuplicateTriangles(this); return this; }
        fillHoles(maxEdges) { __bro_native.mesh.fillHoles(this, int(maxEdges, 64)); return this; }
        simplify(ratio, targetError) {
            __bro_native.mesh.simplify(this, num(ratio, 0.5), num(targetError, 0.01));
            return this;
        }
        simplifyToTriangleCount(count, targetError) {
            __bro_native.mesh.simplifyToTriangleCount(this, int(count, 0), num(targetError, 0.01));
            return this;
        }
        generateLODChain(ratios) {
            const arr = f32(ratios);
            const n = __bro_native.mesh.generateLODChain(this, arr);
            const out = [];
            for (let i = 0; i < n; i++) out.push(__bro_native.mesh.takeLOD(i));
            return out;
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
        smoothLaplacian(lambda, iterations) {
            __bro_native.mesh.smoothLaplacian(this, num(lambda, 0.5), int(iterations, 1));
            return this;
        }
        smoothTaubin(lambda, mu, iterations) {
            __bro_native.mesh.smoothTaubin(this, num(lambda, 0.5), num(mu, -0.53), int(iterations, 1));
            return this;
        }
        remesh(targetEdgeLength) { __bro_native.mesh.remesh(this, num(targetEdgeLength, 0)); return this; }
        remeshIsotropic(targetEdgeLen, iters) {
            __bro_native.mesh.remeshIsotropic(this, num(targetEdgeLen, 0), int(iters, 5));
            return this;
        }
        repair() { __bro_native.mesh.repair(this); return this; }
        optimize() { __bro_native.mesh.optimize(this); return this; }
        optimizeVertexCache() { __bro_native.mesh.optimizeVertexCache(this); return this; }
        optimizeVertexFetch() { __bro_native.mesh.optimizeVertexFetch(this); return this; }
        optimizeOverdraw(threshold) { __bro_native.mesh.optimizeOverdraw(this, num(threshold, 1.05)); return this; }
        spatialSortTriangles() { __bro_native.mesh.spatialSortTriangles(this); return this; }
        spatialSortVertices() { __bro_native.mesh.spatialSortVertices(this); return this; }
        generateShadowIndexBuffer() { return __bro_native.mesh.generateShadowIndexBuffer(this); }
        encode() {
            __bro_native.mesh.encodeMesh(this);
            const vdata = __bro_native.mesh.encodedVertexData();
            const idata = __bro_native.mesh.encodedIndexData();
            return {
                vertexData: vdata,
                indexData: idata,
                vertexCount: __bro_native.mesh.encodedVertexCount(),
                vertexSize: __bro_native.mesh.encodedVertexSize(),
                indexCount: __bro_native.mesh.encodedIndexCount(),
                hasNormals: this.hasNormals,
                hasUVs: this.hasUVs,
                hasColors: this.hasColors,
            };
        }

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
        shrinkwrap(target, method, maxDist, offset, axis) {
            let mode = 0;
            if (typeof method === 'string') {
                const l = method.toLowerCase();
                if (l.includes('norm')) mode = 1;
                else if (l.includes('axis')) mode = 2;
                else mode = 0;
            } else if (typeof method === 'number') {
                mode = method | 0;
            }
            const ax = axis ? flat3(axis, 'shrinkwrap') : new Float32Array(0);
            __bro_native.mesh.shrinkwrap(this, target, mode, num(maxDist, 0), num(offset, 0), ax);
            return this;
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
                this.unwrapUVs();
            } else if (projections[m] !== undefined) {
                __bro_native.mesh.projectUVs(this, projections[m], 1);
            } else {
                throw new TypeError('Mesh.generateUVs: unknown method "' + m + '"');
            }
            return this;
        }
        /** @param {string} [projection] box|planarXY|planarXZ|planarYZ|cylindrical|spherical */
        projectUVs(projection, scale) {
            const p = projection === undefined ? 'box' : String(projection);
            if (projections[p] === undefined) throw new TypeError('Mesh.projectUVs: unknown projection "' + p + '"');
            __bro_native.mesh.projectUVs(this, projections[p], num(scale, 1));
            return this;
        }
        unwrapUVs() {
            const s = __bro_native.mesh.unwrapUVs(this);
            return JSON.parse(s);
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
        /** @returns {MeshUVQualityResult} bromesh's UVMetrics */
        measureUVQuality() {
            const q = JSON.parse(__bro_native.mesh.measureUVQuality(this));
            q.coverage = q.uvSpaceUsage;
            q.avgAngleError = q.avgAngleDistortion;
            q.maxAreaRatio = q.maxAreaDistortion;
            return q;
        }

        // ---- Analysis ---------------------------------------------------------------
        computeBBox() {
            const b = __bro_native.mesh.bounds(this);
            const minX = b[0], minY = b[1], minZ = b[2];
            const maxX = b[3], maxY = b[4], maxZ = b[5];
            return {
                centerX: (minX + maxX) * 0.5,
                centerY: (minY + maxY) * 0.5,
                centerZ: (minZ + maxZ) * 0.5,
                extentX: maxX - minX,
                extentY: maxY - minY,
                extentZ: maxZ - minZ,
                minX, minY, minZ,
                maxX, maxY, maxZ,
                min: [minX, minY, minZ],
                max: [maxX, maxY, maxZ]
            };
        }
        bounds() { return this.computeBBox(); }
        surfaceArea() { return __bro_native.mesh.surfaceArea(this); }
        volume() { return __bro_native.mesh.volume(this); }
        computeVolume() { return this.volume(); }
        isManifold() { return __bro_native.mesh.isManifold(this); }
        /** @returns {Float32Array} one area per triangle */
        triangleAreas() { return __bro_native.mesh.triangleAreas(this); }
        sampleSurface(count, seed) {
            return __bro_native.mesh.sampleSurface(this, int(count, 1000), int(seed, 0));
        }
        /** @returns {MeshBVHIntersectResult|null} */
        raycast(origin, direction, maxDist) {
            const o = vec3(origin, 'raycast'), d = vec3(direction, 'raycast');
            return parseHit(__bro_native.mesh.raycast(this, o[0], o[1], o[2], d[0], d[1], d[2], num(maxDist, 0)));
        }
        raycastTest(origin, direction, maxDist) {
            const o = vec3(origin, 'raycastTest'), d = vec3(direction, 'raycastTest');
            return __bro_native.mesh.raycastTest(this, o[0], o[1], o[2], d[0], d[1], d[2], num(maxDist, 0));
        }
        raycastAll(origin, direction, maxDist) {
            const o = vec3(origin, 'raycastAll'), d = vec3(direction, 'raycastAll');
            return JSON.parse(__bro_native.mesh.raycastAll(this, o[0], o[1], o[2], d[0], d[1], d[2], num(maxDist, 0)));
        }
        closestPoint(point) {
            const p = vec3(point, 'closestPoint');
            return parseHit(__bro_native.mesh.closestPoint(this, p[0], p[1], p[2]));
        }
        buildBVH(leafSize) { return new MeshBVH(this, leafSize); }

        analyzeVertexCache(cacheSize) {
            return JSON.parse(__bro_native.mesh.analyzeVertexCache(this, int(cacheSize, 16)));
        }
        analyzeVertexFetch(vertexSize) {
            return JSON.parse(__bro_native.mesh.analyzeVertexFetch(this, int(vertexSize, 32)));
        }
        analyzeOverdraw() {
            return JSON.parse(__bro_native.mesh.analyzeOverdraw(this));
        }

        buildMeshlets(maxVerticesOrOpts, maybeMaxTriangles, maybeConeWeight) {
            let maxV = 64, maxT = 124, coneW = 0.5;
            if (typeof maxVerticesOrOpts === 'object' && maxVerticesOrOpts !== null) {
                maxV = int(maxVerticesOrOpts.maxVertices, 64);
                maxT = int(maxVerticesOrOpts.maxTriangles, 124);
                coneW = num(maxVerticesOrOpts.coneWeight, 0.5);
            } else {
                maxV = int(maxVerticesOrOpts, 64);
                maxT = int(maybeMaxTriangles, 124);
                coneW = num(maybeConeWeight, 0.5);
            }
            const count = __bro_native.mesh.buildMeshlets(this, maxV, maxT, coneW);
            const vertices = __bro_native.mesh.meshletVertices();
            const triangles = __bro_native.mesh.meshletTriangles();
            const records = __bro_native.mesh.meshletRecords();
            const dv = new DataView(records.buffer, records.byteOffset, records.byteLength);
            const resultList = [];
            for (let i = 0; i < count; i++) {
                const off = i * 64;
                const vOff = dv.getUint32(off, true);
                const vCount = dv.getUint32(off + 4, true);
                const tOff = dv.getUint32(off + 8, true);
                const tCount = dv.getUint32(off + 12, true);
                const cx = dv.getFloat32(off + 16, true);
                const cy = dv.getFloat32(off + 20, true);
                const cz = dv.getFloat32(off + 24, true);
                const radius = dv.getFloat32(off + 28, true);
                const ax = dv.getFloat32(off + 32, true);
                const ay = dv.getFloat32(off + 36, true);
                const az = dv.getFloat32(off + 40, true);
                const cdirX = dv.getFloat32(off + 44, true);
                const cdirY = dv.getFloat32(off + 48, true);
                const cdirZ = dv.getFloat32(off + 52, true);
                const cutoff = dv.getFloat32(off + 56, true);
                const mVerts = vertices.subarray(vOff, vOff + vCount);
                const mTris = triangles.subarray(tOff, tOff + tCount * 3);
                resultList.push({
                    vertices: mVerts,
                    triangles: mTris,
                    bounds: {
                        center: [cx, cy, cz],
                        radius,
                        coneApex: [ax, ay, az],
                        coneAxis: [cdirX, cdirY, cdirZ],
                        coneCutoff: cutoff,
                    }
                });
            }
            resultList.meshletCount = count;
            resultList.meshlets = records.buffer;
            resultList.vertices = vertices;
            resultList.triangles = triangles;
            return resultList;
        }

        // ---- Baking ----------------------------------------------------------------
        bakeAmbientOcclusion(numRays, maxDist) {
            __bro_native.mesh.bakeAmbientOcclusion(this, int(numRays, 64), num(maxDist, 0));
            return this;
        }
        bakeCurvature(scale) {
            __bro_native.mesh.bakeCurvature(this, num(scale, 1));
            return this;
        }
        bakeThickness(numRays, maxDist) {
            __bro_native.mesh.bakeThickness(this, int(numRays, 32), num(maxDist, 0));
            return this;
        }
        bakeAOToTexture(w, h, numRays, maxDist) {
            __bro_native.mesh.bakeAOToTexture(this, int(w, 32), int(h, 32), int(numRays, 64), num(maxDist, 0));
            return readTexBuffer();
        }
        bakeCurvatureToTexture(w, h, scale) {
            __bro_native.mesh.bakeCurvatureToTexture(this, int(w, 32), int(h, 32), num(scale, 1));
            return readTexBuffer();
        }
        bakeThicknessToTexture(w, h, numRays, maxDist) {
            __bro_native.mesh.bakeThicknessToTexture(this, int(w, 32), int(h, 32), int(numRays, 32), num(maxDist, 0));
            return readTexBuffer();
        }
        bakeNormalsToTexture(w, h) {
            __bro_native.mesh.bakeNormalsToTexture(this, int(w, 32), int(h, 32));
            return readTexBuffer();
        }
        bakePositionToTexture(w, h) {
            __bro_native.mesh.bakePositionToTexture(this, int(w, 32), int(h, 32));
            return readTexBuffer();
        }
        bakeNormalsFromReference(ref, w, h, searchDist) {
            __bro_native.mesh.bakeNormalsFromReference(this, ref, int(w, 32), int(h, 32), num(searchDist, 0));
            return readTexBuffer();
        }
        bakeAOFromReference(ref, w, h, numRays, maxDist) {
            __bro_native.mesh.bakeAOFromReference(this, ref, int(w, 32), int(h, 32), int(numRays, 64), num(maxDist, 0));
            return readTexBuffer();
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
            const minX = b[0], minY = b[1], minZ = b[2];
            const maxX = b[3], maxY = b[4], maxZ = b[5];
            return {
                extentX: maxX - minX,
                extentY: maxY - minY,
                extentZ: maxZ - minZ,
                centerX: (minX + maxX) * 0.5,
                centerY: (minY + maxY) * 0.5,
                centerZ: (minZ + maxZ) * 0.5,
                minX, minY, minZ,
                maxX, maxY, maxZ,
                min: [minX, minY, minZ],
                max: [maxX, maxY, maxZ]
            };
        }
        /** @returns {MeshBVHIntersectResult|null} */
        raycast(originOrMesh, directionOrOrigin, maxDistOrDirection, maybeMaxDist) {
            let o, d, maxDist;
            if (originOrMesh instanceof Mesh || arguments.length >= 4) {
                o = vec3(directionOrOrigin, 'raycast');
                d = vec3(maxDistOrDirection, 'raycast');
                maxDist = num(maybeMaxDist, 0);
            } else {
                o = vec3(originOrMesh, 'raycast');
                d = vec3(directionOrOrigin, 'raycast');
                maxDist = num(maxDistOrDirection, 0);
            }
            return parseHit(__bro_native.mesh.bvhRaycast(this, o[0], o[1], o[2], d[0], d[1], d[2], maxDist));
        }
        raycastTest(originOrMesh, directionOrOrigin, maxDistOrDirection, maybeMaxDist) {
            let o, d, maxDist;
            if (originOrMesh instanceof Mesh || arguments.length >= 4) {
                o = vec3(directionOrOrigin, 'raycastTest');
                d = vec3(maxDistOrDirection, 'raycastTest');
                maxDist = num(maybeMaxDist, 0);
            } else {
                o = vec3(originOrMesh, 'raycastTest');
                d = vec3(directionOrOrigin, 'raycastTest');
                maxDist = num(maxDistOrDirection, 0);
            }
            return __bro_native.mesh.bvhRaycastTest(this, o[0], o[1], o[2], d[0], d[1], d[2], maxDist);
        }
        closestPoint(pointOrMesh, maybePoint) {
            const p = vec3(maybePoint !== undefined ? maybePoint : pointOrMesh, 'closestPoint');
            return parseHit(__bro_native.mesh.bvhClosestPoint(this, p[0], p[1], p[2]));
        }
    }

    class ProgressiveMesh {
        /** @param {Mesh} mesh */
        constructor(mesh) {
            return new __bro_native.mesh.ProgressiveMesh(mesh);
        }
        get maxTriangles() { return __bro_native.mesh.pmMaxTriangles(this); }
        get minTriangles() { return __bro_native.mesh.pmMinTriangles(this); }
        atRatio(ratio) { return __bro_native.mesh.pmAtRatio(this, num(ratio, 1)); }
        atTriangleCount(count) { return __bro_native.mesh.pmAtTriangleCount(this, int(count, 0)); }
        serialize() { return __bro_native.mesh.pmSerialize(this); }
        static deserialize(bytes) {
            return __bro_native.mesh.pmDeserialize(u8(bytes));
        }
    }

    // Native prototypes chained under public classes
    if (__bro_native.mesh.MeshPrototype) {
        Object.setPrototypeOf(__bro_native.mesh.MeshPrototype, Mesh.prototype);
    }
    if (__bro_native.mesh.MeshBVHPrototype) {
        Object.setPrototypeOf(__bro_native.mesh.MeshBVHPrototype, MeshBVH.prototype);
    }
    if (__bro_native.mesh.ProgressiveMeshPrototype) {
        Object.setPrototypeOf(__bro_native.mesh.ProgressiveMeshPrototype, ProgressiveMesh.prototype);
    }

    const ns = bro.mesh;
    ns.Mesh = Mesh;
    ns.MeshBVH = MeshBVH;
    ns.ProgressiveMesh = ProgressiveMesh;

    const factories = ['box', 'sphere', 'cylinder', 'capsule', 'cone', 'plane', 'torus', 'icosahedron',
                       'dodecahedron', 'octahedron', 'tetrahedron', 'disk', 'disc', 'geodesicSphere', 'rock', 'blob',
                       'tube', 'heightmapGrid', 'merge', 'marchingCubes', 'surfaceNets', 'dualContouring',
                       'dualContour', 'transvoxel', 'greedyMesh', 'stripify', 'unstripify', 'decode',
                       'splitByPlane', 'booleanUnion', 'booleanDifference', 'booleanIntersection',
                       'union', 'subtract', 'intersect', 'decodeDraco', 'encodeDraco'];
    for (let i = 0; i < factories.length; i++) ns[factories[i]] = Mesh[factories[i]];

    globalThis.Mesh = Mesh;
    globalThis.MeshBVH = MeshBVH;
    globalThis.ProgressiveMesh = ProgressiveMesh;
})();
