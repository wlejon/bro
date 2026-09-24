// clipmap.js — the public shape of bro.clipmap.ClipmapTerrain, assembled over the
// natives under __bro_native.clipmap.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_F32 = new Float32Array(0);
    const EMPTY_F64 = new Float64Array(0);
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});
    const toF64 = (v) => v instanceof Float64Array ? v : Float64Array.from(v);

    // ---- bro.clipmap.ClipmapTerrain ------------------------------------------
    function ClipmapTerrain() {
        throw new TypeError("bro.clipmap.ClipmapTerrain is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.clipmap.ClipmapTerrainProto;
        if (proto === undefined) throw new Error("bro.clipmap.ClipmapTerrain: native class prototype not published (registerNatives_clipmap did not run)");
        Object.setPrototypeOf(proto, ClipmapTerrain.prototype);
    }
    fn(mount(bro, "clipmap"), "ClipmapTerrain", ClipmapTerrain);
    globalThis.ClipmapTerrain = ClipmapTerrain;

    accessor(ClipmapTerrain.prototype, "node",
        function () {
            return __bro_native.scene.canonNode(__bro_native.clipmap.ClipmapTerrain_node_get(this));
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "levels",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_levels_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "resolution",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_resolution_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "cellSize",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_cellSize_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "layerCount",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_layerCount_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "triangleCount",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_triangleCount_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "vertexCount",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_vertexCount_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "farDistance",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_farDistance_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "cellScale",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_cellScale_get(this);
        },
        undefined);
    accessor(ClipmapTerrain.prototype, "planetRadius",
        function () {
            return __bro_native.clipmap.ClipmapTerrain_planetRadius_get(this);
        },
        undefined);

    fn(ClipmapTerrain.prototype, "setHeightLayer", function setHeightLayer(index, desc) {
        if (index === undefined) throw new TypeError("setHeightLayer(index, desc) needs an index");
        if (index < 0 || index >= 6) throw new RangeError("setHeightLayer: index out of range");
        if (!desc) {
            __bro_native.clipmap.ClipmapTerrain_setHeightLayer(this, index, EMPTY_F32, 0, 0, 0, 0, 1, false, false);
            return this;
        }
        const data = desc.data ? (desc.data instanceof Float32Array ? desc.data : Float32Array.from(desc.data)) : EMPTY_F32;
        __bro_native.clipmap.ClipmapTerrain_setHeightLayer(this, index, data, desc.width || 0, desc.height || 0, desc.originX || 0, desc.originZ || 0, desc.metresPerCell || 1, !!desc.wrapX, !!desc.bandLimited);
        return this;
    });
    fn(ClipmapTerrain.prototype, "setChartCenter", function setChartCenter(x, z) {
        if (x === null || x === undefined) {
            __bro_native.clipmap.ClipmapTerrain_setChartCenter(this, false, 0, 0);
        } else {
            __bro_native.clipmap.ClipmapTerrain_setChartCenter(this, true, +x, +z);
        }
        return this;
    });
    fn(ClipmapTerrain.prototype, "setSurfaceLayer", function setSurfaceLayer(indexOrDesc, desc) {
        let index = 0;
        let d = indexOrDesc;
        if (typeof indexOrDesc === 'number') {
            index = indexOrDesc;
            d = desc;
        }
        if (index < 0 || index >= 6) throw new RangeError("setSurfaceLayer: index out of range");
        if (!d) {
            __bro_native.clipmap.ClipmapTerrain_setSurfaceLayer(this, index, EMPTY_F32, 0, 0, 0, 0, 1, 3);
            return this;
        }
        const comps = d.components !== undefined ? d.components : 3;
        if (comps !== 3 && comps !== 4) throw new Error("setSurfaceLayer: components must be 3 or 4");
        const data = d.data ? (d.data instanceof Float32Array ? d.data : Float32Array.from(d.data)) : EMPTY_F32;
        const expected = (d.width || 0) * (d.height || 0) * comps;
        if (data.length < expected) throw new Error("setSurfaceLayer: buffer too short for its components");
        __bro_native.clipmap.ClipmapTerrain_setSurfaceLayer(this, index, data, d.width || 0, d.height || 0, d.originX || 0, d.originZ || 0, d.metresPerCell || 1, comps);
        return this;
    });

    fn(ClipmapTerrain.prototype, "setSnowLine", function setSnowLine(m) {
        if (m === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.setSnowLine: m is required");
        __bro_native.clipmap.ClipmapTerrain_setSnowLine(this, m);
        return this;
    });
    fn(ClipmapTerrain.prototype, "setDetail", function setDetail(desc) {
        if (desc === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.setDetail: desc is required");
        const d_desc = desc;
        __bro_native.clipmap.ClipmapTerrain_setDetail(this, d_desc.wavelength !== undefined, d_desc.wavelength === undefined ? 0 : d_desc.wavelength, d_desc.relief !== undefined, d_desc.relief === undefined ? 0 : d_desc.relief, d_desc.gain !== undefined, d_desc.gain === undefined ? 0 : d_desc.gain, d_desc.octaves !== undefined, d_desc.octaves === undefined ? 0 : d_desc.octaves);
        return this;
    });
    fn(ClipmapTerrain.prototype, "setMaterials", function setMaterials(desc) {
        if (desc === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.setMaterials: desc is required");
        const d_desc = desc;
        const d_desc_rock = d_desc.rock === undefined ? {} : d_desc.rock;
        const d_desc_snow = d_desc.snow === undefined ? {} : d_desc.snow;
        const d_desc_sand = d_desc.sand === undefined ? {} : d_desc.sand;
        const d_desc_grass = d_desc.grass === undefined ? {} : d_desc.grass;
        __bro_native.clipmap.ClipmapTerrain_setMaterials(this, d_desc_rock.albedo === undefined ? EMPTY_F64 : toF64(d_desc_rock.albedo), d_desc_rock.roughness !== undefined, d_desc_rock.roughness === undefined ? 0 : d_desc_rock.roughness, d_desc_snow.albedo === undefined ? EMPTY_F64 : toF64(d_desc_snow.albedo), d_desc_snow.roughness !== undefined, d_desc_snow.roughness === undefined ? 0 : d_desc_snow.roughness, d_desc_sand.albedo === undefined ? EMPTY_F64 : toF64(d_desc_sand.albedo), d_desc_sand.roughness !== undefined, d_desc_sand.roughness === undefined ? 0 : d_desc_sand.roughness, d_desc_grass.albedo === undefined ? EMPTY_F64 : toF64(d_desc_grass.albedo), d_desc_grass.roughness !== undefined, d_desc_grass.roughness === undefined ? 0 : d_desc_grass.roughness);
        return this;
    });
    fn(ClipmapTerrain.prototype, "setForest", function setForest(desc) {
        if (desc === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.setForest: desc is required");
        const d_desc = desc;
        __bro_native.clipmap.ClipmapTerrain_setForest(this, d_desc.albedo === undefined ? EMPTY_F64 : toF64(d_desc.albedo), d_desc.strength !== undefined, d_desc.strength === undefined ? 0 : d_desc.strength);
        return this;
    });
    fn(ClipmapTerrain.prototype, "update", function update(camX, camY, camZ) {
        if (camX === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.update: camX is required");
        if (camY === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.update: camY is required");
        if (camZ === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.update: camZ is required");
        __bro_native.clipmap.ClipmapTerrain_update(this, camX, camY, camZ);
        return this;
    });
    fn(ClipmapTerrain.prototype, "shaderSource", function shaderSource(stage) {
        if (stage === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.shaderSource: stage is required");
        return __bro_native.clipmap.ClipmapTerrain_shaderSource(this, stage);
    });
    fn(ClipmapTerrain.prototype, "elevationAt", function elevationAt(x, z) {
        if (x === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.elevationAt: x is required");
        if (z === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.elevationAt: z is required");
        return __bro_native.clipmap.ClipmapTerrain_elevationAt(this, x, z);
    });
    fn(ClipmapTerrain.prototype, "renderedElevationAt", function renderedElevationAt(x, z) {
        if (x === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.renderedElevationAt: x is required");
        if (z === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.renderedElevationAt: z is required");
        return __bro_native.clipmap.ClipmapTerrain_renderedElevationAt(this, x, z);
    });
    fn(ClipmapTerrain.prototype, "coverageDistance", function coverageDistance(eyeAboveSeaLevel) {
        if (eyeAboveSeaLevel === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.coverageDistance: eyeAboveSeaLevel is required");
        return __bro_native.clipmap.ClipmapTerrain_coverageDistance(this, eyeAboveSeaLevel);
    });
    fn(ClipmapTerrain.prototype, "horizonDistance", function horizonDistance(eyeAboveSeaLevel) {
        if (eyeAboveSeaLevel === undefined) throw new TypeError("bro.clipmap.ClipmapTerrain.prototype.horizonDistance: eyeAboveSeaLevel is required");
        return __bro_native.clipmap.ClipmapTerrain_horizonDistance(this, eyeAboveSeaLevel);
    });
    fn(ClipmapTerrain.prototype, "destroy", function destroy() {
        __bro_native.clipmap.ClipmapTerrain_destroy(this);
    });
})();
