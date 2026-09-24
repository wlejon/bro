// tile_world.js — the public shape of bro.tile_world.TileWorld, assembled over the
// natives under __bro_native.tile_world.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_U8 = new Uint8Array(0);
    const EMPTY_F64 = new Float64Array(0);
    const bufferOf = (a) => a.byteOffset === 0 && a.byteLength === a.buffer.byteLength ? a.buffer : a.buffer.slice(a.byteOffset, a.byteOffset + a.byteLength);
    const bytesOf = (v) => v instanceof Uint8Array ? v : v instanceof ArrayBuffer ? new Uint8Array(v) : new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});

    // ---- bro.tile_world.TileWorld --------------------------------------------
    function TileWorld() {
        throw new TypeError("bro.tile_world.TileWorld is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.tile_world.TileWorldProto;
        if (proto === undefined) throw new Error("bro.tile_world.TileWorld: native class prototype not published (registerNatives_tile_world did not run)");
        Object.setPrototypeOf(proto, TileWorld.prototype);
    }
    fn(mount(bro, "tile_world"), "TileWorld", TileWorld);
    globalThis.TileWorld = TileWorld;

    accessor(TileWorld.prototype, "node",
        function () {
            return __bro_native.tile_world.TileWorld_node_get(this);
        },
        undefined);
    accessor(TileWorld.prototype, "width",
        function () {
            return __bro_native.tile_world.TileWorld_width_get(this);
        },
        undefined);
    accessor(TileWorld.prototype, "height",
        function () {
            return __bro_native.tile_world.TileWorld_height_get(this);
        },
        undefined);
    accessor(TileWorld.prototype, "chunkCount",
        function () {
            return __bro_native.tile_world.TileWorld_chunkCount_get(this);
        },
        undefined);
    accessor(TileWorld.prototype, "chunks",
        function () {
            return __bro_native.tile_world.TileWorld_chunks_get(this);
        },
        undefined);
    accessor(TileWorld.prototype, "vertexCount",
        function () {
            return __bro_native.tile_world.TileWorld_vertexCount_get(this);
        },
        undefined);
    accessor(TileWorld.prototype, "triangleCount",
        function () {
            return __bro_native.tile_world.TileWorld_triangleCount_get(this);
        },
        undefined);
    fn(TileWorld.prototype, "setTile", function setTile(x, y, tileId, layer) {
        if (layer === undefined) layer = 0;
        __bro_native.tile_world.TileWorld_setTile(this, layer, x, y, tileId);
    });
    fn(TileWorld.prototype, "getTile", function getTile(x, y, layer) {
        if (layer === undefined) layer = 0;
        return __bro_native.tile_world.TileWorld_getTile(this, layer, x, y);
    });
    fn(TileWorld.prototype, "fillTile", function fillTile(x0, y0, x1, y1, tileId, layer) {
        if (layer === undefined) layer = 0;
        __bro_native.tile_world.TileWorld_fillTile(this, x0, y0, x1, y1, tileId, layer);
    });
    fn(TileWorld.prototype, "fillRect", function fillRect(layer, x, y, w, h, tileId) {
        __bro_native.tile_world.TileWorld_fillRect(this, layer, x, y, w, h, tileId);
    });
    fn(TileWorld.prototype, "setElevation", function setElevation(x, y, level) {
        __bro_native.tile_world.TileWorld_setElevation(this, x, y, level);
    });
    fn(TileWorld.prototype, "getElevation", function getElevation(x, y) {
        return __bro_native.tile_world.TileWorld_getElevation(this, x, y);
    });
    fn(TileWorld.prototype, "fillElevation", function fillElevation(x0, y0, x1, y1, level) {
        __bro_native.tile_world.TileWorld_fillElevation(this, x0, y0, x1, y1, level);
    });
    fn(TileWorld.prototype, "setFlag", function setFlag(x, y, bit, on) {
        __bro_native.tile_world.TileWorld_setFlag(this, x, y, bit, !!on);
    });
    fn(TileWorld.prototype, "hasFlag", function hasFlag(x, y, bit) {
        return __bro_native.tile_world.TileWorld_hasFlag(this, x, y, bit);
    });
    fn(TileWorld.prototype, "setTint", function setTint(x, y, r, g, b, a) {
        __bro_native.tile_world.TileWorld_setTint(this, x, y, r, g, b, a === undefined ? 1.0 : a);
    });
    fn(TileWorld.prototype, "fillTint", function fillTint(x0, y0, x1, y1, r, g, b, a) {
        __bro_native.tile_world.TileWorld_fillTint(this, x0, y0, x1, y1, r, g, b, a === undefined ? 1.0 : a);
    });
    fn(TileWorld.prototype, "getTint", function getTint(x, y) {
        const s = __bro_native.tile_world.TileWorld_getTint(this, x, y);
        return s ? JSON.parse(s) : { r: 1, g: 1, b: 1, a: 1 };
    });
    fn(TileWorld.prototype, "worldToCell", function worldToCell(wx, wz) {
        const s = __bro_native.tile_world.TileWorld_worldToCell(this, wx, wz);
        return s ? JSON.parse(s) : null;
    });
    fn(TileWorld.prototype, "cellCenterWorldXZ", function cellCenterWorldXZ(cx, cy) {
        const s = __bro_native.tile_world.TileWorld_cellCenterWorldXZ(this, cx, cy);
        return s ? JSON.parse(s) : null;
    });
    fn(TileWorld.prototype, "worldBounds", function worldBounds() {
        const s = __bro_native.tile_world.TileWorld_worldBounds(this);
        return s ? JSON.parse(s) : null;
    });
    fn(TileWorld.prototype, "sampleHeight", function sampleHeight(wx, wz) {
        const y = __bro_native.tile_world.TileWorld_sampleHeight(this, wx, wz);
        return y <= -1e8 ? null : y;
    });
    fn(TileWorld.prototype, "raycastCell", function raycastCell(origin, dir, maxDist) {
        maxDist = maxDist === undefined ? 1e6 : maxDist;
        const s = __bro_native.tile_world.TileWorld_raycastCell(this, origin[0], origin[1], origin[2], dir[0], dir[1], dir[2], maxDist);
        return s ? JSON.parse(s) : null;
    });
    fn(TileWorld.prototype, "isWalkable", function isWalkable(x, y, mask) {
        return __bro_native.tile_world.TileWorld_isWalkable(this, x, y, mask || 0);
    });
    fn(TileWorld.prototype, "toNavGrid", function toNavGrid(opts) {
        if (!bro.ai || !bro.ai.game || !bro.ai.game.createNavGrid) return null;
        const b = this.worldBounds();
        if (!b) return null;
        const ng = bro.ai.game.createNavGrid({ minX: b.minX, minZ: b.minZ, maxX: b.maxX, maxZ: b.maxZ, cellSize: 1.0 });
        this.syncNavGrid(ng, opts);
        return ng;
    });
    fn(TileWorld.prototype, "syncNavGrid", function syncNavGrid(ng, opts) {
        if (!ng || typeof ng.addObstacle !== 'function') return 0;
        const mask = (opts && opts.blockMask) || 0;
        const padding = (opts && opts.padding) || 0;
        let blocked = 0;
        const w = this.width;
        const h = this.height;
        for (let y = 0; y < h; y++) {
            for (let x = 0; x < w; x++) {
                if (!this.isWalkable(x, y, mask)) {
                    const center = this.cellCenterWorldXZ(x, y);
                    if (center) {
                        ng.addObstacle({ cx: center.x, cz: center.z, hw: 0.49, hd: 0.49 }, padding);
                        blocked++;
                    }
                }
            }
        }
        return blocked;
    });
    fn(TileWorld.prototype, "clearLayer", function clearLayer(layer) {
        if (layer === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.clearLayer: layer is required");
        __bro_native.tile_world.TileWorld_clearLayer(this, layer);
    });
    fn(TileWorld.prototype, "pickTile", function pickTile(worldX, worldZ) {
        if (worldX === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.pickTile: worldX is required");
        if (worldZ === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.pickTile: worldZ is required");
        if (!__bro_native.tile_world.TileWorld_pickTile(this, worldX, worldZ)) return null;
        return { tileX: __bro_native.tile_world.TileWorld_pickTile_tileX(), tileY: __bro_native.tile_world.TileWorld_pickTile_tileY(), layer: __bro_native.tile_world.TileWorld_pickTile_layer(), tileId: __bro_native.tile_world.TileWorld_pickTile_tileId(), worldPosition: Array.from(__bro_native.tile_world.TileWorld_pickTile_worldPosition()) };
    });
    fn(TileWorld.prototype, "findPath", function findPath(startX, startY, endX, endY, opts) {
        if (startX === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.findPath: startX is required");
        if (startY === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.findPath: startY is required");
        if (endX === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.findPath: endX is required");
        if (endY === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.findPath: endY is required");
        __bro_native.tile_world.TileWorld_findPathJson(this, startX, startY, endX, endY, opts ? JSON.stringify(opts) : "{}");
        const raw = JSON.parse(__bro_native.tile_world.TileWorld_findPath_path() || "[]");
        const path = raw.map(p => Array.isArray(p) ? { x: p[0], y: p[1] } : p);
        path.reachable = __bro_native.tile_world.TileWorld_findPath_reachable();
        path.totalCost = __bro_native.tile_world.TileWorld_findPath_totalCost();
        return path;
    });
    fn(TileWorld.prototype, "computeRegions", function computeRegions(layer) {
        if (layer === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.computeRegions: layer is required");
        const n = __bro_native.tile_world.TileWorld_computeRegions(this, layer);
        const out = new Array(n);
        for (let i = 0; i < n; i++) {
            out[i] = { regionId: __bro_native.tile_world.TileWorld_computeRegions_regionId(i), tiles: JSON.parse(__bro_native.tile_world.TileWorld_computeRegions_tiles(i)), area: __bro_native.tile_world.TileWorld_computeRegions_area(i) };
        }
        return out;
    });
    fn(TileWorld.prototype, "setOrigin", function setOrigin(x, y, z) {
        if (x === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.setOrigin: x is required");
        if (y === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.setOrigin: y is required");
        if (z === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.setOrigin: z is required");
        __bro_native.tile_world.TileWorld_setOrigin(this, x, y, z);
    });
    fn(TileWorld.prototype, "advance", function advance(dtMs) {
        if (dtMs === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.advance: dtMs is required");
        return __bro_native.tile_world.TileWorld_advance(this, dtMs);
    });
    fn(TileWorld.prototype, "addObjectKind", function addObjectKind(mesh, style) {
        return __bro_native.tile_world.TileWorld_addObjectKind(this, mesh, JSON.stringify(style || {}));
    });
    fn(TileWorld.prototype, "addObject", function addObject(kindId, x, y, opts) {
        opts = opts || {};
        return __bro_native.tile_world.TileWorld_addObjectPlacement(this, kindId, x, y,
            opts.yaw || 0, opts.scale == null ? 1 : opts.scale, opts.yOffset || 0, opts.offsetX || 0, opts.offsetZ || 0, opts.variant || 0,
            opts.color == null ? EMPTY_F64 : Float64Array.from(opts.color));
    });
    fn(TileWorld.prototype, "clearObjects", function clearObjects(kindId) {
        __bro_native.tile_world.TileWorld_clearObjects(this, kindId !== undefined, kindId === undefined ? 0 : kindId);
    });
    fn(TileWorld.prototype, "objectCount", function objectCount(kind) {
        if (kind === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.objectCount: kind is required");
        return __bro_native.tile_world.TileWorld_objectCount(this, kind);
    });
    fn(TileWorld.prototype, "rebuildObjects", function rebuildObjects() {
        __bro_native.tile_world.TileWorld_rebuildObjects(this);
    });
    fn(TileWorld.prototype, "rebuild", function rebuild() {
        __bro_native.tile_world.TileWorld_rebuild(this);
    });
    fn(TileWorld.prototype, "rebuildAll", function rebuildAll() {
        __bro_native.tile_world.TileWorld_rebuildAll(this);
    });
    fn(TileWorld.prototype, "configure", function configure(opts) {
        opts = opts || {};
        const json = JSON.stringify(opts, (k, v) => {
            if (ArrayBuffer.isView(v) && !(v instanceof DataView)) return Array.from(v);
            if (v instanceof ArrayBuffer) return Array.from(new Uint8Array(v));
            return v;
        });
        __bro_native.tile_world.TileWorld_configure(this, json);
        return this;
    });
    fn(TileWorld.prototype, "save", function save() {
        const buf = bufferOf(__bro_native.tile_world.TileWorld_save(this));
        return new Uint8Array(buf);
    });
    fn(TileWorld.prototype, "load", function load(data) {
        if (data === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.load: data is required");
        return __bro_native.tile_world.TileWorld_load(this, bytesOf(data));
    });
    fn(TileWorld.prototype, "setShade", function setShade(x, y, v) {
        __bro_native.tile_world.TileWorld_setShade(this, x, y, v);
    });
    fn(TileWorld.prototype, "fillShade", function fillShade(x0, y0, x1, y1, v) {
        __bro_native.tile_world.TileWorld_fillShade(this, x0, y0, x1, y1, v);
    });
    fn(TileWorld.prototype, "setShadeMap", function setShadeMap(values) {
        if (!values) return;
        if (values instanceof Float32Array) {
            __bro_native.tile_world.TileWorld_setShadeMapFloat(this, values);
        } else if (values instanceof Uint8Array) {
            __bro_native.tile_world.TileWorld_setShadeMapBytes(this, values);
        } else if (Array.isArray(values)) {
            __bro_native.tile_world.TileWorld_setShadeMapFloat(this, new Float32Array(values));
        }
    });
    fn(TileWorld.prototype, "getShade", function getShade(x, y) {
        return __bro_native.tile_world.TileWorld_getShade(this, x, y);
    });
    fn(TileWorld.prototype, "distanceField", function distanceField(sources, opts) {
        const srcJson = Array.isArray(sources) ? JSON.stringify(sources) : (sources ? JSON.stringify([sources]) : "[]");
        const optsJson = opts ? JSON.stringify(opts) : "";
        const s = __bro_native.tile_world.TileWorld_distanceField(this, srcJson, optsJson);
        const arr = s ? JSON.parse(s) : [];
        return (opts && opts.costs) ? Float32Array.from(arr) : Int32Array.from(arr);
    });
    fn(TileWorld.prototype, "floodFill", function floodFill(seedX, seedY, opts) {
        const optsJson = opts ? JSON.stringify(opts) : "";
        const s = __bro_native.tile_world.TileWorld_floodFill(this, seedX, seedY, optsJson);
        return s ? JSON.parse(s) : [];
    });
    fn(TileWorld.prototype, "components", function components(opts) {
        const optsJson = opts ? JSON.stringify(opts) : "";
        const s = __bro_native.tile_world.TileWorld_components(this, optsJson);
        return s ? JSON.parse(s) : [];
    });
    fn(TileWorld.prototype, "cellDistance", function cellDistance(ax, ay, bx, by, conn) {
        return __bro_native.tile_world.TileWorld_cellDistance(this, ax, ay, bx, by, conn ? String(conn) : "");
    });
    fn(TileWorld.prototype, "cellRing", function cellRing(cx, cy, radius, conn) {
        const s = __bro_native.tile_world.TileWorld_cellRing(this, cx, cy, radius, conn ? String(conn) : "");
        return s ? JSON.parse(s) : [];
    });
    fn(TileWorld.prototype, "cellsInRange", function cellsInRange(cx, cy, radius, conn) {
        const s = __bro_native.tile_world.TileWorld_cellsInRange(this, cx, cy, radius, conn ? String(conn) : "");
        return s ? JSON.parse(s) : [];
    });
    fn(TileWorld.prototype, "cellLine", function cellLine(ax, ay, bx, by) {
        const s = __bro_native.tile_world.TileWorld_cellLine(this, ax, ay, bx, by);
        return s ? JSON.parse(s) : [];
    });
    fn(TileWorld.prototype, "cellNeighbors", function cellNeighbors(cx, cy, conn) {
        const s = __bro_native.tile_world.TileWorld_cellNeighbors(this, cx, cy, conn ? String(conn) : "");
        return s ? JSON.parse(s) : [];
    });
    fn(TileWorld.prototype, "destroy", function destroy() {
        __bro_native.tile_world.TileWorld_destroy(this);
    });
})();

