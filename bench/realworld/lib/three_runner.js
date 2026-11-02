// three.js r160 scene runner. A scene module calls
//   threeScene({ name, defaults, create(ctx) -> { scene, camera, step(f, t) } })
// In state mode there is no WebGLRenderer: emulateRender() runs the CPU half
// of WebGLRenderer.render (r160 lines 29540-29790): matrixWorld updates,
// frustum culling and the opaque/transparent render-list sort, and the
// once-per-frame skeleton.update() of WebGLObjects.update. In render modes
// the state is read from the live renderer's render list instead.

import * as THREE from '../vendor/three.module.js';
import { run, makeRng, Hasher } from './harness.js';

const _proj = new THREE.Matrix4();
const _frustum = new THREE.Frustum();
const _v = new THREE.Vector3();

function painterSortStable(a, b) {
    if (a.groupOrder !== b.groupOrder) return a.groupOrder - b.groupOrder;
    else if (a.renderOrder !== b.renderOrder) return a.renderOrder - b.renderOrder;
    else if (a.material.id !== b.material.id) return a.material.id - b.material.id;
    else if (a.z !== b.z) return a.z - b.z;
    return a.id - b.id;
}
function reversePainterSortStable(a, b) {
    if (a.groupOrder !== b.groupOrder) return a.groupOrder - b.groupOrder;
    else if (a.renderOrder !== b.renderOrder) return a.renderOrder - b.renderOrder;
    else if (a.z !== b.z) return b.z - a.z;
    return a.id - b.id;
}

function makeList() { return { opaque: [], transparent: [], lights: [], skeletons: new Set(), frame: 0 }; }

function pushItem(list, object, geometry, material, groupOrder, z) {
    const item = { id: object.id, object, geometry, material, groupOrder, renderOrder: object.renderOrder, z };
    if (material.transparent === true) list.transparent.push(item);
    else list.opaque.push(item);
}

function projectObject(object, camera, groupOrder, list) {
    if (object.visible === false) return;
    if (object.layers.test(camera.layers)) {
        if (object.isGroup) {
            groupOrder = object.renderOrder;
        } else if (object.isLight) {
            list.lights.push(object);
        } else if (object.isMesh || object.isLine || object.isPoints) {
            if (!object.frustumCulled || _frustum.intersectsObject(object)) {
                const geometry = object.geometry;
                if (object.isSkinnedMesh && !list.skeletons.has(object.skeleton)) {
                    object.skeleton.update();
                    list.skeletons.add(object.skeleton);
                }
                const material = object.material;
                if (object.boundingSphere !== undefined) {
                    if (object.boundingSphere === null) object.computeBoundingSphere();
                    _v.copy(object.boundingSphere.center);
                } else {
                    if (geometry.boundingSphere === null) geometry.computeBoundingSphere();
                    _v.copy(geometry.boundingSphere.center);
                }
                _v.applyMatrix4(object.matrixWorld).applyMatrix4(_proj);
                if (Array.isArray(material)) {
                    for (const group of geometry.groups) {
                        const gm = material[group.materialIndex];
                        if (gm && gm.visible) pushItem(list, object, geometry, gm, groupOrder, _v.z);
                    }
                } else if (material.visible) {
                    pushItem(list, object, geometry, material, groupOrder, _v.z);
                }
            }
        }
    }
    const children = object.children;
    for (let i = 0, l = children.length; i < l; i++) projectObject(children[i], camera, groupOrder, list);
}

export function emulateRender(scene, camera, list) {
    if (scene.matrixWorldAutoUpdate === true) scene.updateMatrixWorld();
    if (camera.parent === null && camera.matrixWorldAutoUpdate === true) camera.updateMatrixWorld();
    _proj.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
    _frustum.setFromProjectionMatrix(_proj);
    list.opaque.length = 0;
    list.transparent.length = 0;
    list.lights.length = 0;
    list.skeletons.clear();
    list.frame++;
    projectObject(scene, camera, 0, list);
    if (list.opaque.length > 1) list.opaque.sort(painterSortStable);
    if (list.transparent.length > 1) list.transparent.sort(reversePainterSortStable);
}

// State of one checkpoint. `items` is { opaque, transparent } from either the
// emulation or the renderer's own WebGLRenderList (same item shape).
function threeState(scene, items, detail) {
    const mw = new Hasher();
    const bones = new Hasher();
    const inst = new Hasher();
    const geo = new Hasher();
    const skeletons = new Set();
    let objects = 0;
    let last = null;
    const chunks = detail ? [] : null;
    let chunk = detail ? new Hasher() : null;
    scene.traverse((o) => {
        objects++;
        mw.arr(o.matrixWorld.elements);
        if (chunk) { chunk.arr(o.matrixWorld.elements); if (objects % 64 === 0) { chunks.push(chunk.f32); chunk = new Hasher(); } }
        if (o.isSkinnedMesh) skeletons.add(o.skeleton);
        if (o.isInstancedMesh) {
            inst.arr(o.instanceMatrix.array);
            if (o.instanceColor) inst.arr(o.instanceColor.array);
        }
        if (o.userData.dynamicGeometry) geo.arr(o.geometry.attributes.position.array);
        last = o;
    });
    for (const s of skeletons) bones.arr(s.boneMatrices);
    const vis = new Hasher();
    const z = new Hasher();
    for (const it of items.opaque) { vis.num(it.object.id); vis.num(it.material.id); z.num(it.z); }
    vis.num(-1);
    for (const it of items.transparent) { vis.num(it.object.id); vis.num(it.material.id); z.num(it.z); }
    const out = {
        objects, mw: mw.both,
        opaque: items.opaque.length, transparent: items.transparent.length,
        vis: vis.exact, z: z.both,
        sample: last ? Array.from(last.matrixWorld.elements.slice(12, 15)) : null,
    };
    if (skeletons.size) out.bones = bones.both;
    if (inst.count) out.inst = inst.both;
    if (geo.count) out.geo = geo.both;
    if (chunks) { if (chunk.count) chunks.push(chunk.f32); out.chunks = chunks; }
    if (detail >= 2) {
        // full dump: every object's type, name, local TRS and matrixWorld
        out.dump = [];
        scene.traverse((o) => out.dump.push([o.type, o.name, Array.from(o.position.toArray()), Array.from(o.quaternion.toArray()), Array.from(o.scale.toArray()), Array.from(o.matrixWorld.elements)]));
    }
    return out;
}

export function threeScene(def) {
    run({
        name: def.name,
        lib: 'three',
        defaults: def.defaults,
        start(cfg, h) {
            const rng = makeRng(cfg.seed);
            let renderer = null;
            if (h) {
                renderer = new THREE.WebGLRenderer({ canvas: h.canvas, antialias: !!cfg.aa, powerPreference: 'high-performance', preserveDrawingBuffer: false });
                renderer.setPixelRatio(1);
                renderer.setSize(cfg.w, cfg.h, false);
                if (cfg.shadows) { renderer.shadowMap.enabled = true; renderer.shadowMap.type = THREE.PCFShadowMap; }
            }
            const s = def.create({ THREE, n: cfg.n, rng, cfg, renderer });
            const list = makeList();
            return {
                info() {
                    const gl = renderer.getContext();
                    return { lib: 'three', rev: THREE.REVISION, webgl2: renderer.capabilities.isWebGL2, maxTextures: renderer.capabilities.maxTextures, precision: renderer.capabilities.precision, logDepth: renderer.capabilities.logarithmicDepthBuffer, ctxAttrs: gl.getContextAttributes() };
                },
                frame(f) {
                    s.step(f, f / 60);
                    if (renderer) renderer.render(s.scene, s.camera);
                    else emulateRender(s.scene, s.camera, list);
                },
                state(detail) {
                    const items = renderer ? renderer.renderLists.get(s.scene, 0) : list;
                    const out = threeState(s.scene, items, detail);
                    if (renderer) out.calls = renderer.info.render.calls, out.tris = renderer.info.render.triangles;
                    return out;
                },
                gl() { return renderer.getContext(); },
            };
        },
    });
}

export { THREE };
