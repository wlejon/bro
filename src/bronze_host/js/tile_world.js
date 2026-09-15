// tile_world.js — the public shape of bro.tile_world.TileWorld, assembled over the
// natives under __bro_native.tile_world.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_U8 = new Uint8Array(0);
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
    accessor(TileWorld.prototype, "paging",
        function () {
            return __bro_native.tile_world.TileWorld_paging_get(this);
        },
        function (v) {
            __bro_native.tile_world.TileWorld_paging_set(this, v);
        });
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
    fn(TileWorld.prototype, "update", function update(camX, camY, camZ) {
        return __bro_native.tile_world.TileWorld_update(this, camX === undefined ? 0 : camX, camY === undefined ? 0 : camY, camZ === undefined ? 0 : camZ);
    });
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
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.tile_world.TileWorld_findPath(this, startX, startY, endX, endY, d_opts.agentRadius !== undefined, d_opts.agentRadius === undefined ? 0 : d_opts.agentRadius, d_opts.allowDiagonal !== undefined, d_opts.allowDiagonal === undefined ? false : d_opts.allowDiagonal, d_opts.maxSlope !== undefined, d_opts.maxSlope === undefined ? 0 : d_opts.maxSlope);
        return { reachable: __bro_native.tile_world.TileWorld_findPath_reachable(), path: JSON.parse(__bro_native.tile_world.TileWorld_findPath_path()), totalCost: __bro_native.tile_world.TileWorld_findPath_totalCost() };
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
    fn(TileWorld.prototype, "applyAutotile", function applyAutotile(layer, rules) {
        if (layer === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.applyAutotile: layer is required");
        if (rules === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.applyAutotile: rules is required");
        __bro_native.tile_world.TileWorld_applyAutotile(this, layer, JSON.stringify(rules));
    });
    fn(TileWorld.prototype, "extractVoxelMesh", function extractVoxelMesh(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return __bro_native.tile_world.TileWorld_extractVoxelMesh(this, d_opts.minX !== undefined, d_opts.minX === undefined ? 0 : d_opts.minX, d_opts.minY !== undefined, d_opts.minY === undefined ? 0 : d_opts.minY, d_opts.maxX !== undefined, d_opts.maxX === undefined ? 0 : d_opts.maxX, d_opts.maxY !== undefined, d_opts.maxY === undefined ? 0 : d_opts.maxY, d_opts.heightScale !== undefined, d_opts.heightScale === undefined ? 0 : d_opts.heightScale);
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
            opts.yaw || 0, opts.scale || 1, opts.yOffset || 0, opts.offsetX || 0, opts.offsetZ || 0, opts.variant || 0);
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
            if (v instanceof Float32Array || v instanceof Float64Array || v instanceof Uint8Array || v instanceof Int32Array) {
                return Array.from(v);
            }
            return v;
        });
        __bro_native.tile_world.TileWorld_configure(this, json);
        return this;
    });
    fn(TileWorld.prototype, "save", function save() {
        return bufferOf(__bro_native.tile_world.TileWorld_save(this));
    });
    fn(TileWorld.prototype, "load", function load(data) {
        if (data === undefined) throw new TypeError("bro.tile_world.TileWorld.prototype.load: data is required");
        return __bro_native.tile_world.TileWorld_load(this, bytesOf(data));
    });
    fn(TileWorld.prototype, "destroy", function destroy() {
        __bro_native.tile_world.TileWorld_destroy(this);
    });
})();
