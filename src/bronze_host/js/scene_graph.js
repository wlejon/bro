// scene_graph.js — the members of bro.scene.SceneGraph: the node factories,
// the camera, environment and post-processing setters, raycast and
// projection, and frame capture.
//
// Entered right after js/scene.js (installSceneModule, host_js_modules.cpp),
// which is what put SceneGraph on globalThis and published the node wrapper
// cache as __bro_native.scene.canonNode, and before js/scene_extras.js, which
// wraps createMesh / createInstancedMesh / createShape / createSprite.
(function () {
    'use strict';

    const SceneGraph = globalThis.SceneGraph;
    if (typeof SceneGraph !== 'function') return;

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_F32 = new Float32Array(0);
    const EMPTY_F64 = new Float64Array(0);
    const EMPTY_I32 = new Int32Array(0);
    const toF32 = (v) => v instanceof Float32Array ? v : (typeof v === 'number' ? new Float32Array([v]) : (Array.isArray(v) ? Float32Array.from(v) : EMPTY_F32));
    const toF64 = (v) => v instanceof Float64Array ? v : (typeof v === 'number' ? new Float64Array([v]) : (Array.isArray(v) ? Float64Array.from(v) : EMPTY_F64));
    const toI32 = (v) => v instanceof Int32Array ? v : (typeof v === 'number' ? new Int32Array([v]) : (Array.isArray(v) ? Int32Array.from(v) : EMPTY_I32));
    const canon = __bro_native.scene.canonNode;
    const applyNodeOpts = (node, opts) => {
        if (!node || !opts) return;
        const keys = ['name', 'x', 'y', 'z', 'position', 'rotation', 'scale', 'color',
                      'metallic', 'roughness', 'emissive', 'cullMargin', 'castsShadow', 'receivesShadow',
                      'visible', 'updateMode', 'resolution', 'boxProjection', 'intensity',
                      'interior', 'priority', 'worldAnchor', 'rx', 'ry', 'rz',
                      'rotationX', 'rotationY', 'rotationZ'];
        for (let i = 0; i < keys.length; i++) {
            const k = keys[i];
            if (opts[k] !== undefined) {
                if (k === 'color' && typeof opts.color === 'object' && !Array.isArray(opts.color)) continue;
                node[k] = opts[k];
            }
        }
        if (opts.fill !== undefined && opts.color === undefined) node.color = opts.fill;
    };

    accessor(SceneGraph.prototype, "root", function () { const r = __bro_native.scene.SceneGraph_root_get(this); return r ? canon(r) : undefined; }, undefined);
    accessor(SceneGraph.prototype, "cameraX", function () { return __bro_native.scene.SceneGraph_cameraX_get(this); }, function (v) { __bro_native.scene.SceneGraph_cameraX_set(this, v); });
    accessor(SceneGraph.prototype, "cameraY", function () { return __bro_native.scene.SceneGraph_cameraY_get(this); }, function (v) { __bro_native.scene.SceneGraph_cameraY_set(this, v); });
    accessor(SceneGraph.prototype, "cameraZoom", function () { return __bro_native.scene.SceneGraph_cameraZoom_get(this); }, function (v) { __bro_native.scene.SceneGraph_cameraZoom_set(this, v); });
    accessor(SceneGraph.prototype, "showLightIcons", function () { return __bro_native.scene.SceneGraph_showLightIcons_get(this); }, function (v) { __bro_native.scene.SceneGraph_showLightIcons_set(this, v); });
    accessor(SceneGraph.prototype, "frustumCulling", function () { return __bro_native.scene.SceneGraph_frustumCulling_get(this); }, function (v) { __bro_native.scene.SceneGraph_frustumCulling_set(this, v); });
    accessor(SceneGraph.prototype, "shadowCache", function () { return __bro_native.scene.SceneGraph_shadowCache_get(this); }, function (v) { __bro_native.scene.SceneGraph_shadowCache_set(this, v); });
    accessor(SceneGraph.prototype, "renderScale", function () { return __bro_native.scene.SceneGraph_renderScale_get(this); }, function (v) { __bro_native.scene.SceneGraph_renderScale_set(this, v); });
    accessor(SceneGraph.prototype, "msaa", function () { return __bro_native.scene.SceneGraph_msaa_get(this); }, function (v) { __bro_native.scene.SceneGraph_msaa_set(this, v); });
    accessor(SceneGraph.prototype, "activeCamera", function () { return canon(__bro_native.scene.SceneGraph_activeCamera_get(this)); }, function (v) { if (v === null || v === undefined) __bro_native.scene.SceneGraph_clearActiveCamera(this); else __bro_native.scene.SceneGraph_activeCamera_set(this, v); });

    accessor(SceneGraph.prototype, "viewMatrix",
        function () {
            if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
            return Array.from(__bro_native.scene.SceneGraph_viewMatrix_get(this));
        },
        undefined);
    accessor(SceneGraph.prototype, "projectionMatrix",
        function () {
            if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
            return Array.from(__bro_native.scene.SceneGraph_projectionMatrix_get(this));
        },
        undefined);
    accessor(SceneGraph.prototype, "cameraEye",
        function () {
            if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
            return Array.from(__bro_native.scene.SceneGraph_cameraEye_get(this));
        },
        undefined);
    fn(SceneGraph.prototype, "createNode", function createNode(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        if (typeof opts === 'string') opts = { name: opts };
        const d_opts = opts === undefined ? {} : opts;
        return canon(__bro_native.scene.SceneGraph_createNode(this, d_opts.name !== undefined, d_opts.name === undefined ? '' : d_opts.name, d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position), d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation), d_opts.scale === undefined ? EMPTY_F64 : toF64(d_opts.scale), d_opts.visible !== undefined, d_opts.visible === undefined ? false : d_opts.visible));
    });
    fn(SceneGraph.prototype, "createHtmlNode", function createHtmlNode(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return canon(__bro_native.scene.SceneGraph_createHtmlNode(this, d_opts.html !== undefined, d_opts.html === undefined ? '' : d_opts.html, d_opts.width !== undefined, d_opts.width === undefined ? 0 : d_opts.width, d_opts.height !== undefined, d_opts.height === undefined ? 0 : d_opts.height, d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position), d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation), d_opts.scale === undefined ? EMPTY_F64 : toF64(d_opts.scale), d_opts.visible !== undefined, d_opts.visible === undefined ? false : d_opts.visible));
    });
    fn(SceneGraph.prototype, "createLight", function createLight(opts) {
        const d_opts = opts === undefined ? {} : opts;
        let color = d_opts.color;
        const innerCone = d_opts.innerCone !== undefined ? d_opts.innerCone : d_opts.innerAngle;
        const outerCone = d_opts.outerCone !== undefined ? d_opts.outerCone : d_opts.outerAngle;
        const castShadow = d_opts.castShadow !== undefined ? d_opts.castShadow : d_opts.castsShadow;
        const node = canon(__bro_native.scene.SceneGraph_createLight(this,
            d_opts.type !== undefined, d_opts.type === undefined ? '' : d_opts.type,
            color === undefined ? EMPTY_F64 : toF64(color),
            d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity,
            d_opts.range !== undefined, d_opts.range === undefined ? 0 : d_opts.range,
            innerCone !== undefined, innerCone === undefined ? 0 : innerCone,
            outerCone !== undefined, outerCone === undefined ? 0 : outerCone,
            castShadow !== undefined, castShadow === undefined ? false : castShadow,
            d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position),
            d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation)));
        if (node) {
            applyNodeOpts(node, d_opts);
            if (d_opts.direction !== undefined) node.direction = d_opts.direction;
            if (d_opts.innerAngle !== undefined) node.innerAngle = d_opts.innerAngle;
            if (d_opts.outerAngle !== undefined) node.outerAngle = d_opts.outerAngle;
            if (d_opts.shadowBias !== undefined) node.shadowBias = d_opts.shadowBias;
            if (d_opts.shadowNormalBias !== undefined) node.shadowNormalBias = d_opts.shadowNormalBias;
            if (d_opts.cascadeCount !== undefined) node.cascadeCount = d_opts.cascadeCount;
            if (d_opts.cascadeSplitLambda !== undefined) node.cascadeSplitLambda = d_opts.cascadeSplitLambda;
        }
        return node;
    });

    fn(SceneGraph.prototype, "createParticles", function createParticles(opts) {
        return canon(__bro_native.scene.SceneGraph_createParticles(this, opts ? JSON.stringify(opts, (k, v) =>
            ArrayBuffer.isView(v) && !(v instanceof DataView) ? Array.from(v) : v) : ""));
    });
    fn(SceneGraph.prototype, "createDecal", function createDecal(opts) {
        const d_opts = opts === undefined ? {} : opts;
        let sz = d_opts.size;
        if (typeof sz === 'number') sz = [sz, sz, sz];
        const texStr = typeof d_opts.texture === 'string' ? d_opts.texture : '';
        const node = canon(__bro_native.scene.SceneGraph_createDecal(this, texStr !== '', texStr, sz === undefined ? EMPTY_F64 : toF64(sz), d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position), d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation)));
        if (node && opts) {
            applyNodeOpts(node, d_opts);
            if (d_opts.size !== undefined) {
                if (typeof d_opts.size === 'number') node.scale = [d_opts.size, d_opts.size, d_opts.size];
                else node.scale = d_opts.size;
            }
            if (opts.modulate !== undefined) node.modulate = opts.modulate;
            if (opts.upperFade !== undefined) node.upperFade = opts.upperFade;
            if (opts.lowerFade !== undefined) node.lowerFade = opts.lowerFade;
            if (opts.normalFade !== undefined) node.normalFade = opts.normalFade;
            if (opts.renderPriority !== undefined) node.renderPriority = opts.renderPriority;
            if (opts.emissionStrength !== undefined) node.emissionStrength = opts.emissionStrength;
            if (opts.texture && typeof opts.texture === 'object') node.setBaseColorTexture(opts.texture);
            if (opts.emissionTexture && typeof opts.emissionTexture === 'object') node.setEmissionTexture(opts.emissionTexture);
            if (opts.name !== undefined) node.name = opts.name;
        }
        return node;
    });
    fn(SceneGraph.prototype, "createReflectionProbe", function createReflectionProbe(opts) {
        const d_opts = opts === undefined ? {} : opts;
        let sz = d_opts.size;
        if (typeof sz === 'number') sz = [sz, sz, sz];
        const node = canon(__bro_native.scene.SceneGraph_createReflectionProbe(this, sz === undefined ? EMPTY_F64 : toF64(sz), d_opts.resolution !== undefined, d_opts.resolution === undefined ? 0 : d_opts.resolution, d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position)));
        if (node) {
            applyNodeOpts(node, d_opts);
            if (d_opts.size !== undefined) {
                if (typeof d_opts.size === 'number') node.scale = [d_opts.size, d_opts.size, d_opts.size];
                else node.scale = d_opts.size;
            }
        }
        return node;
    });

    fn(SceneGraph.prototype, "createTween", function createTween() {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        return __bro_native.scene.SceneGraph_createTween(this);
    });
    fn(SceneGraph.prototype, "createAnimationPlayer", function createAnimationPlayer() {
        return __bro_native.scene.SceneGraph_createAnimationPlayer(this);
    });
    fn(SceneGraph.prototype, "createTerrain", function createTerrain(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const d_opts_noise = d_opts.noise === undefined ? {} : d_opts.noise;
        return __bro_native.scene.SceneGraph_createTerrain(this, d_opts.chunkSize === undefined ? EMPTY_I32 : toI32(d_opts.chunkSize), d_opts.cellSize !== undefined, d_opts.cellSize === undefined ? 0 : d_opts.cellSize, d_opts.loadRadius !== undefined, d_opts.loadRadius === undefined ? 0 : d_opts.loadRadius, d_opts.unloadRadius !== undefined, d_opts.unloadRadius === undefined ? 0 : d_opts.unloadRadius, d_opts.maxLoadsPerUpdate !== undefined, d_opts.maxLoadsPerUpdate === undefined ? 0 : d_opts.maxLoadsPerUpdate, d_opts.seed !== undefined, d_opts.seed === undefined ? 0 : d_opts.seed, d_opts_noise.frequency !== undefined, d_opts_noise.frequency === undefined ? 0 : d_opts_noise.frequency, d_opts_noise.octaves !== undefined, d_opts_noise.octaves === undefined ? 0 : d_opts_noise.octaves, d_opts_noise.gain !== undefined, d_opts_noise.gain === undefined ? 0 : d_opts_noise.gain, d_opts_noise.lacunarity !== undefined, d_opts_noise.lacunarity === undefined ? 0 : d_opts_noise.lacunarity, d_opts.baseHeight !== undefined, d_opts.baseHeight === undefined ? 0 : d_opts.baseHeight, d_opts.heightAmplitude !== undefined, d_opts.heightAmplitude === undefined ? 0 : d_opts.heightAmplitude, d_opts.seaLevel !== undefined, d_opts.seaLevel === undefined ? 0 : d_opts.seaLevel, d_opts.meshMode !== undefined, d_opts.meshMode === undefined ? 0 : d_opts.meshMode, d_opts.terraceStep !== undefined, d_opts.terraceStep === undefined ? 0 : d_opts.terraceStep, d_opts.continentFrequency !== undefined, d_opts.continentFrequency === undefined ? 0 : d_opts.continentFrequency, d_opts.continentMin !== undefined, d_opts.continentMin === undefined ? 0 : d_opts.continentMin, d_opts.continentMax !== undefined, d_opts.continentMax === undefined ? 0 : d_opts.continentMax, d_opts.mountainFrequency !== undefined, d_opts.mountainFrequency === undefined ? 0 : d_opts.mountainFrequency, d_opts.mountainAmplitude !== undefined, d_opts.mountainAmplitude === undefined ? 0 : d_opts.mountainAmplitude, d_opts.mountainOctaves !== undefined, d_opts.mountainOctaves === undefined ? 0 : d_opts.mountainOctaves, d_opts.lodLevels !== undefined, d_opts.lodLevels === undefined ? 0 : d_opts.lodLevels, d_opts.lodScaleFactor !== undefined, d_opts.lodScaleFactor === undefined ? 0 : d_opts.lodScaleFactor, d_opts.planetRadius !== undefined, d_opts.planetRadius === undefined ? 0 : d_opts.planetRadius, d_opts.origin === undefined ? EMPTY_F64 : toF64(d_opts.origin), d_opts.palette === undefined ? EMPTY_F32 : toF32(d_opts.palette));
    });
    fn(SceneGraph.prototype, "createClipmapTerrain", function createClipmapTerrain(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return __bro_native.clipmap.createClipmapTerrain(this, d_opts.levels !== undefined, d_opts.levels === undefined ? 0 : d_opts.levels, d_opts.resolution !== undefined, d_opts.resolution === undefined ? 0 : d_opts.resolution, d_opts.cellSize !== undefined, d_opts.cellSize === undefined ? 0 : d_opts.cellSize, d_opts.heightScale !== undefined, d_opts.heightScale === undefined ? 0 : d_opts.heightScale, d_opts.seaLevel !== undefined, d_opts.seaLevel === undefined ? 0 : d_opts.seaLevel, d_opts.snowLine !== undefined, d_opts.snowLine === undefined ? 0 : d_opts.snowLine, d_opts.maxCellScale !== undefined, d_opts.maxCellScale === undefined ? 0 : d_opts.maxCellScale, d_opts.planetRadius !== undefined, d_opts.planetRadius === undefined ? 0 : d_opts.planetRadius, d_opts.layerFade !== undefined, d_opts.layerFade === undefined ? false : d_opts.layerFade, d_opts.coverageFloor !== undefined, d_opts.coverageFloor === undefined ? false : d_opts.coverageFloor, d_opts.cubicSurface !== undefined, d_opts.cubicSurface === undefined ? false : d_opts.cubicSurface, d_opts.cubicHeight !== undefined, d_opts.cubicHeight === undefined ? false : d_opts.cubicHeight, d_opts.detailWavelength !== undefined, d_opts.detailWavelength === undefined ? 0 : d_opts.detailWavelength, d_opts.detailRelief !== undefined, d_opts.detailRelief === undefined ? 0 : d_opts.detailRelief, d_opts.detailGain !== undefined, d_opts.detailGain === undefined ? 0 : d_opts.detailGain, d_opts.detailOctaves !== undefined, d_opts.detailOctaves === undefined ? 0 : d_opts.detailOctaves);
    });
    fn(SceneGraph.prototype, "createTileWorld", function createTileWorld(opts) {
        opts = opts || {};
        // Any typed-array view (a canvas's Uint8ClampedArray included) or a
        // bare ArrayBuffer travels as a plain array; JSON would otherwise
        // turn it into an index-keyed object the native cannot read.
        const json = JSON.stringify(opts, (k, v) => {
            if (ArrayBuffer.isView(v) && !(v instanceof DataView)) return Array.from(v);
            if (v instanceof ArrayBuffer) return Array.from(new Uint8Array(v));
            return v;
        });
        const res = __bro_native.tile_world.createTileWorldJson(this, json);
        if (!res) throw new Error("createTileWorld: invalid configuration");
        return res;
    });
    fn(SceneGraph.prototype, "findById", function findById(id) {
        if (id === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.findById: id is required");
        return canon(__bro_native.scene.SceneGraph_findById(this, id));
    });
    fn(SceneGraph.prototype, "findByName", function findByName(name) {
        if (name === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.findByName: name is required");
        return canon(__bro_native.scene.SceneGraph_findByName(this, name));
    });
    fn(SceneGraph.prototype, "destroyNode", function destroyNode(node) {
        if (node === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.destroyNode: node is required");
        __bro_native.scene.SceneGraph_destroyNode(this, node);
    });
    fn(SceneGraph.prototype, "setCamera", function setCamera(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const eye = d_opts.eye !== undefined ? d_opts.eye : d_opts.position;
        const target = d_opts.target !== undefined ? d_opts.target : d_opts.lookAt;
        __bro_native.scene.SceneGraph_setCamera(this, d_opts.fov !== undefined, d_opts.fov === undefined ? 0 : d_opts.fov, d_opts.near !== undefined, d_opts.near === undefined ? 0 : d_opts.near, d_opts.far !== undefined, d_opts.far === undefined ? 0 : d_opts.far, eye === undefined ? EMPTY_F64 : toF64(eye), target === undefined ? EMPTY_F64 : toF64(target), d_opts.up === undefined ? EMPTY_F64 : toF64(d_opts.up), d_opts.aspect !== undefined, d_opts.aspect === undefined ? 0 : d_opts.aspect, d_opts.quaternion === undefined ? EMPTY_F64 : toF64(d_opts.quaternion), d_opts.mode !== undefined, d_opts.mode === undefined ? '' : d_opts.mode, d_opts.size !== undefined, d_opts.size === undefined ? 0 : d_opts.size);
    });
    fn(SceneGraph.prototype, "createCamera", function createCamera(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const eye = d_opts.eye !== undefined ? d_opts.eye : d_opts.position;
        const target = d_opts.target !== undefined ? d_opts.target : d_opts.lookAt;
        // `projection` / `orthoHeight` are the node-attribute spellings of
        // `mode` / `size`; either form reaches the native as mode/size.
        const mode = d_opts.mode !== undefined ? d_opts.mode : d_opts.projection;
        const size = d_opts.size !== undefined ? d_opts.size : d_opts.orthoHeight;
        const cam = canon(__bro_native.scene.SceneGraph_createCamera(this, d_opts.fov !== undefined, d_opts.fov === undefined ? 0 : d_opts.fov, d_opts.near !== undefined, d_opts.near === undefined ? 0 : d_opts.near, d_opts.far !== undefined, d_opts.far === undefined ? 0 : d_opts.far, eye === undefined ? EMPTY_F64 : toF64(eye), target === undefined ? EMPTY_F64 : toF64(target), d_opts.up === undefined ? EMPTY_F64 : toF64(d_opts.up), d_opts.aspect !== undefined, d_opts.aspect === undefined ? 0 : d_opts.aspect, d_opts.quaternion === undefined ? EMPTY_F64 : toF64(d_opts.quaternion), mode !== undefined, mode === undefined ? '' : mode, size !== undefined, size === undefined ? 0 : size));
        if (d_opts.name !== undefined) cam.name = d_opts.name;
        if (d_opts.active) this.setActiveCamera(cam);
        return cam;
    });
    fn(SceneGraph.prototype, "setActiveCamera", function setActiveCamera(camera) {
        if (camera === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setActiveCamera: camera is required");
        if (camera === null) {
            __bro_native.scene.SceneGraph_clearActiveCamera(this);
            return;
        }
        __bro_native.scene.SceneGraph_setActiveCamera(this, camera);
    });
    fn(SceneGraph.prototype, "setToneMap", function setToneMap(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const wp = d_opts.whitePoint !== undefined ? d_opts.whitePoint : d_opts.gamma;
        __bro_native.scene.SceneGraph_setToneMap(this, d_opts.mode !== undefined, d_opts.mode === undefined ? '' : d_opts.mode, d_opts.exposure !== undefined, d_opts.exposure === undefined ? 0 : d_opts.exposure, wp !== undefined, wp === undefined ? 0 : wp);
    });
    fn(SceneGraph.prototype, "setAmbient", function setAmbient(opts, g, b) {
        let col = [0.03, 0.03, 0.03];
        if (typeof opts === 'number' && typeof g === 'number' && typeof b === 'number') {
            col = [opts, g, b];
        } else if (Array.isArray(opts) || ArrayBuffer.isView(opts)) {
            col = opts;
        } else if (opts && opts.color) {
            col = opts.color;
        }
        __bro_native.scene.SceneGraph_setAmbient(this, toF64(col), false, 0);
    });
    fn(SceneGraph.prototype, "setWind", function setWind(opts, speed) {
        // setWind({ direction, strength, frequency }); the (dir, strength)
        // positional spelling the generated docs carried for a while maps
        // onto the same natives.
        let d_opts = opts === undefined || opts === null ? {} : opts;
        if (Array.isArray(opts) || ArrayBuffer.isView(opts)) d_opts = { direction: opts, strength: speed };
        __bro_native.scene.SceneGraph_setWind(this, d_opts.direction === undefined ? EMPTY_F64 : toF64(d_opts.direction), d_opts.strength !== undefined, d_opts.strength === undefined ? 0 : d_opts.strength, d_opts.frequency !== undefined, d_opts.frequency === undefined ? 0 : d_opts.frequency);
    });
    fn(SceneGraph.prototype, "setShadowQuality", function setShadowQuality(opts, pcfTaps) {
        // setShadowQuality({ atlasSize, pcfTaps }) or the positional
        // setShadowQuality(atlasSize, pcfTaps) every workshop app calls.
        let d_opts = opts === undefined || opts === null ? {} : opts;
        if (typeof opts === "number") d_opts = { atlasSize: opts, pcfTaps: pcfTaps };
        __bro_native.scene.SceneGraph_setShadowQuality(this, d_opts.atlasSize !== undefined, d_opts.atlasSize === undefined ? 0 : d_opts.atlasSize, d_opts.pcfTaps !== undefined, d_opts.pcfTaps === undefined ? 0 : d_opts.pcfTaps);
    });
    fn(SceneGraph.prototype, "setShadowCache", function setShadowCache(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setShadowCache(this, d_opts.enabled !== undefined, d_opts.enabled === undefined ? false : d_opts.enabled, d_opts.staticResolution !== undefined, d_opts.staticResolution === undefined ? 0 : d_opts.staticResolution);
    });
    fn(SceneGraph.prototype, "setFog", function setFog(opts) {
        const d_opts = opts == null ? {} : opts;   // null, like {}, turns fog off
        const f_start = d_opts.startDistance !== undefined ? d_opts.startDistance : d_opts.start;
        __bro_native.scene.SceneGraph_setFog(this, d_opts.mode !== undefined, d_opts.mode === undefined ? '' : d_opts.mode, d_opts.color === undefined ? EMPTY_F64 : toF64(d_opts.color), d_opts.density !== undefined, d_opts.density === undefined ? 0 : d_opts.density, f_start !== undefined, f_start === undefined ? 0 : f_start, d_opts.end !== undefined, d_opts.end === undefined ? 0 : d_opts.end, d_opts.heightFalloff !== undefined, d_opts.heightFalloff === undefined ? 0 : d_opts.heightFalloff, d_opts.height !== undefined, d_opts.height === undefined ? 0 : d_opts.height);
    });
    fn(SceneGraph.prototype, "setAtmosphere", function setAtmosphere(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setAtmosphere(this, d_opts.rayleigh === undefined ? EMPTY_F64 : toF64(d_opts.rayleigh), d_opts.mie === undefined ? EMPTY_F64 : toF64(d_opts.mie), d_opts.turbidity !== undefined, d_opts.turbidity === undefined ? 0 : d_opts.turbidity, d_opts.sunPosition === undefined ? EMPTY_F64 : toF64(d_opts.sunPosition), d_opts.sunIntensity !== undefined, d_opts.sunIntensity === undefined ? 0 : d_opts.sunIntensity);
    });
    fn(SceneGraph.prototype, "setStarfield", function setStarfield(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setStarfield(this, d_opts.starCount !== undefined, d_opts.starCount === undefined ? 0 : d_opts.starCount, d_opts.starSize !== undefined, d_opts.starSize === undefined ? 0 : d_opts.starSize, d_opts.twinkleSpeed !== undefined, d_opts.twinkleSpeed === undefined ? 0 : d_opts.twinkleSpeed, d_opts.tint === undefined ? EMPTY_F64 : toF64(d_opts.tint));
    });
    fn(SceneGraph.prototype, "setTiltShift", function setTiltShift(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setTiltShift(this, d_opts.blur !== undefined, d_opts.blur === undefined ? 0 : d_opts.blur, d_opts.focus !== undefined, d_opts.focus === undefined ? 0 : d_opts.focus, d_opts.range !== undefined, d_opts.range === undefined ? 0 : d_opts.range);
    });
    fn(SceneGraph.prototype, "setBloom", function setBloom(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setBloom(this, d_opts.threshold !== undefined, d_opts.threshold === undefined ? 0 : d_opts.threshold, d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity, d_opts.radius !== undefined, d_opts.radius === undefined ? 0 : d_opts.radius, d_opts.iterations !== undefined, d_opts.iterations === undefined ? 0 : d_opts.iterations);
    });
    fn(SceneGraph.prototype, "setSSAO", function setSSAO(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setSSAO(this, d_opts.radius !== undefined, d_opts.radius === undefined ? 0 : d_opts.radius, d_opts.bias !== undefined, d_opts.bias === undefined ? 0 : d_opts.bias, d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity, d_opts.sampleCount !== undefined, d_opts.sampleCount === undefined ? 0 : d_opts.sampleCount);
    });
    fn(SceneGraph.prototype, "setSSR", function setSSR(opts) {
        if (!opts || opts.enabled === false) {
            __bro_native.scene.SceneGraph_setSSR(this, false, 30, 48, 0.3, 1, 0.1);
            return;
        }
        const steps = opts.steps !== undefined ? opts.steps : (opts.stepCount !== undefined ? opts.stepCount : 48);
        __bro_native.scene.SceneGraph_setSSR(this, true,
            opts.maxDistance !== undefined ? opts.maxDistance : 30, steps,
            opts.thickness !== undefined ? opts.thickness : 0.3,
            opts.intensity !== undefined ? opts.intensity : 1,
            opts.edgeFade !== undefined ? opts.edgeFade : 0.1);
    });
    fn(SceneGraph.prototype, "setDepthOfField", function setDepthOfField(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setDepthOfField(this, d_opts.focusDistance !== undefined, d_opts.focusDistance === undefined ? 0 : d_opts.focusDistance, d_opts.focalLength !== undefined, d_opts.focalLength === undefined ? 0 : d_opts.focalLength, d_opts.fStop !== undefined, d_opts.fStop === undefined ? 0 : d_opts.fStop, d_opts.maxBlur !== undefined, d_opts.maxBlur === undefined ? 0 : d_opts.maxBlur);
    });
    fn(SceneGraph.prototype, "setColorLUT", function setColorLUT(opts) {
        if (!opts) return __bro_native.scene.SceneGraph_setColorLUT(this, '', 0, 0);
        const p = opts.path || opts.texture || '';
        const s = opts.size !== undefined ? opts.size : 0;
        const a = opts.amount !== undefined ? opts.amount : (opts.intensity !== undefined ? opts.intensity : 1.0);
        return __bro_native.scene.SceneGraph_setColorLUT(this, p, s, a);
    });
    fn(SceneGraph.prototype, "setFXAA", function setFXAA(enabled) {
        if (enabled === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setFXAA: enabled is required");
        __bro_native.scene.SceneGraph_setFXAA(this, enabled);
    });
    fn(SceneGraph.prototype, "setRenderScale", function setRenderScale(scale) {
        if (scale === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setRenderScale: scale is required");
        __bro_native.scene.SceneGraph_setRenderScale(this, scale);
    });
    fn(SceneGraph.prototype, "setMSAA", function setMSAA(samples) {
        if (samples === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setMSAA: samples is required");
        __bro_native.scene.SceneGraph_setMSAA(this, samples);
    });
    fn(SceneGraph.prototype, "setEnvironment", function setEnvironment(opts) {
        if (opts === null || opts === undefined) {
            __bro_native.scene.SceneGraph_setEnvironment(this, true, '', false, '', EMPTY_F64, false, 0, false, 0, false, false);
            return true;
        }
        if (typeof opts !== 'object') return false;
        const d_opts = opts;
        const pano = d_opts.panorama !== undefined ? d_opts.panorama : d_opts.hdr;
        const rot = d_opts.rotation;
        return __bro_native.scene.SceneGraph_setEnvironment(this,
            pano !== undefined, pano === undefined ? '' : pano,
            d_opts.cubeMap !== undefined, d_opts.cubeMap === undefined ? '' : d_opts.cubeMap,
            d_opts.color === undefined ? EMPTY_F64 : toF64(d_opts.color),
            d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity,
            rot !== undefined, rot === undefined ? 0 : rot,
            d_opts.background !== undefined, d_opts.background === undefined ? false : d_opts.background);
    });
    fn(SceneGraph.prototype, "setFrustumCulling", function setFrustumCulling(e) { if (e === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setFrustumCulling: enabled is required"); __bro_native.scene.SceneGraph_setFrustumCulling(this, e); });
    fn(SceneGraph.prototype, "cullStats", function cullStats() {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
        const s = __bro_native.scene.SceneGraph_cullStatsJson(this);
        return s ? JSON.parse(s) : { totalNodes: 0, renderedNodes: 0, culledNodes: 0 };
    });
    fn(SceneGraph.prototype, "clear", function clear() { __bro_native.scene.SceneGraph_clear(this); });
    fn(SceneGraph.prototype, "syncPhysics", function syncPhysics() { __bro_native.scene.SceneGraph_syncPhysics(this); });
    fn(SceneGraph.prototype, "raycast", function raycast(origin, direction, maxDist) {
        if (origin === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.raycast: origin is required");
        if (direction === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.raycast: direction is required");
        if (!__bro_native.scene.SceneGraph_raycast(this, toF64(origin), toF64(direction))) return null;
        const dist = __bro_native.scene.SceneGraph_raycast_distance();
        if (maxDist !== undefined && dist > maxDist) return null;
        const inst = __bro_native.scene.SceneGraph_raycast_instance();
        const res = {
            hit: true,
            node: canon(__bro_native.scene.SceneGraph_raycast_node()),
            point: Array.from(__bro_native.scene.SceneGraph_raycast_point()),
            normal: Array.from(__bro_native.scene.SceneGraph_raycast_normal()),
            distance: dist
        };
        if (inst >= 0) res.instance = inst;
        return res;
    });

    fn(SceneGraph.prototype, "unprojectLocal", function unprojectLocal(x, y) {
        if (x === undefined || y === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.unprojectLocal: x and y are required");
        const r = __bro_native.scene.SceneGraph_unprojectLocal(this, +x, +y);
        if (r.length < 6) return null;
        return { origin: [r[0], r[1], r[2]], dir: [r[3], r[4], r[5]] };
    });
    fn(SceneGraph.prototype, "projectLocal", function projectLocal(x, y, z) {
        if (Array.isArray(x) || ArrayBuffer.isView(x)) { z = x[2]; y = x[1]; x = x[0]; }
        if (x === undefined || y === undefined || z === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.projectLocal: x, y and z are required");
        const r = __bro_native.scene.SceneGraph_projectLocal(this, +x, +y, +z);
        if (r.length < 3) return null;
        return { x: r[0], y: r[1], depth: r[2], behind: !(r[2] > 1e-6) };
    });
    fn(SceneGraph.prototype, "bindAudioListenerToCamera", function bindAudioListenerToCamera(bind) {
        if (bind === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.bindAudioListenerToCamera: bind is required");
        __bro_native.scene.SceneGraph_bindAudioListenerToCamera(this, bind);
    });
    // attachAIWorld is installed from C++ on the native SceneGraph prototype
    // (installSceneGraphAgent, host_scene_agent.cpp).
    fn(SceneGraph.prototype, "detachAIWorld", function detachAIWorld() {
        __bro_native.scene.SceneGraph_detachAIWorld(this);
    });

    fn(SceneGraph.prototype, "createMesh", function createMesh(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        if (typeof opts === 'string') opts = { mesh: opts };
        const meshObj = (opts && opts.mesh && typeof opts.mesh !== 'string') ? opts.mesh : null;
        const node = canon(__bro_native.scene.SceneGraph_createMesh(this, opts || {}, meshObj));
        if (!node) return undefined;
        applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createShape", function createShape(opts) {
        const node = canon(__bro_native.scene.SceneGraph_createShape(this, opts ? JSON.stringify(opts) : ""));
        if (node) applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createSprite", function createSprite(opts) {
        const node = canon(__bro_native.scene.SceneGraph_createSprite(this, opts ? JSON.stringify(opts) : ""));
        if (node) applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createPhysicsNode", function createPhysicsNode(opts) {
        const node = canon(__bro_native.scene.SceneGraph_createPhysicsNode(this, opts ? JSON.stringify(opts) : ""));
        if (node) applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createParticles3D", function createParticles3D(opts) {
        const node = canon(__bro_native.scene.SceneGraph_createParticles3D(this, opts ? JSON.stringify(opts) : ""));
        if (node) {
            applyNodeOpts(node, opts);
            if (opts && typeof opts.onFinished === 'function') node.onFinished = opts.onFinished;
        }
        return node;
    });
    fn(SceneGraph.prototype, "createGaussianSplat", function createGaussianSplat(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        const node = canon(__bro_native.scene.SceneGraph_createGaussianSplat(this, ""));
        if (!node) return undefined;
        if (opts) {
            if (typeof opts === 'string') {
                __bro_native.scene.SceneNode_loadSplatPly(node, opts);
            } else {
                if (opts.path) __bro_native.scene.SceneNode_loadSplatPly(node, opts.path);
                if (opts.cloud) node.setCloud(opts.cloud);
                applyNodeOpts(node, opts);
            }
        }
        return node;
    });
    fn(SceneGraph.prototype, "createSkinnedMesh", function createSkinnedMesh(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        const meshObj = (opts && opts.mesh && typeof opts.mesh !== 'string') ? opts.mesh : null;
        const node = canon(__bro_native.scene.SceneGraph_createSkinnedMesh(this, opts || {}, meshObj));
        if (!node) return undefined;
        applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createInstancedMesh", function createInstancedMesh(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        const meshObj = (opts && opts.mesh && typeof opts.mesh !== 'string') ? opts.mesh : null;
        const node = canon(__bro_native.scene.SceneGraph_createInstancedMesh(this, opts || {}, meshObj));
        if (!node) return undefined;
        applyNodeOpts(node, opts);
        if (opts) {
            if (opts.instances) node.setInstances(opts.instances);
            if (opts.instancesFromTransforms) node.setInstancesFromTransforms(opts.instancesFromTransforms);
        }
        return node;
    });
    fn(SceneGraph.prototype, "asTexture", function asTexture() {
        const scn = this;
        return { _scene: scn, get valid() { return __bro_native.scene.SceneGraph_isValid(scn); } };
    });

    fn(SceneGraph.prototype, "toImageData", function toImageData() {
        const buf = __bro_native.scene.SceneGraph_readTonemapPixels(this);
        if (!buf || buf.length === 0) return null;
        const w = __bro_native.scene.SceneGraph_readTonemapWidth();
        const h = __bro_native.scene.SceneGraph_readTonemapHeight();
        const data = new Uint8ClampedArray(buf.buffer, buf.byteOffset, buf.byteLength);
        if (typeof globalThis.ImageData !== 'undefined') {
            try { return new globalThis.ImageData(data, w, h); } catch (e) {}
        }
        return { width: w, height: h, data };
    });
    fn(SceneGraph.prototype, "captureFrame", function captureFrame(width, height) {
        if (width !== undefined && height !== undefined) {
            __bro_native.scene.SceneGraph_setCanvasSize(this, width, height);
        }
        __bro_native.scene.SceneGraph_render(this);
        const buf = __bro_native.scene.SceneGraph_readTonemapPixels(this);
        if (!buf || buf.length === 0) return null;
        const w = __bro_native.scene.SceneGraph_readTonemapWidth();
        const h = __bro_native.scene.SceneGraph_readTonemapHeight();
        const data = new Uint8ClampedArray(buf.buffer, buf.byteOffset, buf.byteLength);
        if (typeof globalThis.ImageData !== 'undefined') {
            try { return new globalThis.ImageData(data, w, h); } catch (e) {}
        }
        return { width: w, height: h, data };
    });
    // No setClearColor: the scene clears to transparent and the canvas's CSS
    // background shows through (docs/scene-api.js). The method that was here
    // dropped its colour and only reset the environment intensity to 1.
})();
