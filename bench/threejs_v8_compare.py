#!/usr/bin/env python3
"""
End-to-end Three.js & Pixi.js benchmark suite comparing:
  1. bro-headless (Compiled AOT via bronze + brass SMT guard-elim & superopt)
  2. bro-headless (In-process Tiered JIT)
  3. Chromium Headless (V8 + WebGL2 on Vulkan/ANGLE)
  4. Pure-compute Three.js / kernel benchmarks (bronze AOT modes vs Node.js V8)

Usage:
  python3 bench/threejs_v8_compare.py [--reps 3] [--webgl-only] [--pure-only]
                                      [--json-out /tmp/threejs_v8_report.json]
"""

import argparse
import http.server
import json
import os
import shutil
import socketserver
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

BRO_ROOT = Path(__file__).resolve().parent.parent
BRONZE_ROOT = BRO_ROOT.parent / "bronze"
ORACLE_THREE = BRONZE_ROOT / "tests" / "oracle" / "threejs"
ORACLE_PIXI = BRONZE_ROOT / "tests" / "oracle" / "pixi"


def find_bro_binaries():
    for sub in ["build-release", "build"]:
        d = BRO_ROOT / sub
        bh = d / "bro-headless"
        bz = d / "bronze"
        rt = d / "shared" / "libbronze_runtime_shared.so"
        if bh.is_file() and os.access(bh, os.X_OK):
            return bh, (bz if bz.is_file() else None), (rt if rt.is_file() else None)
    raise FileNotFoundError("Could not find built bro-headless in build-release/ or build/")


COMMON_JS_HEADER = """
const WIDTH = 640;
const HEIGHT = 480;

const nowMs = (typeof __host !== 'undefined' && typeof __host.wallMs === 'function')
    ? () => __host.wallMs()
    : () => performance.now();

const isBroHeadless = (typeof __host !== 'undefined' && typeof __host.wallMs === 'function');
"""

SCENE_INSTANCED_JS = COMMON_JS_HEADER + """
import {
    Scene, PerspectiveCamera, WebGLRenderer, BoxGeometry,
    InstancedMesh, MeshStandardMaterial, DirectionalLight,
    AmbientLight, Object3D, Color
} from './three.module.js';

const INSTANCE_COUNT = 2500;
const WARMUP_FRAMES = 20;
const MEASURED_FRAMES = 120;
const ENGINE_RAF_FRAMES = 60;

const canvas = document.createElement('canvas');
canvas.width = WIDTH;
canvas.height = HEIGHT;
document.body.appendChild(canvas);

const renderer = new WebGLRenderer({ canvas: canvas, antialias: false, powerPreference: 'high-performance' });
renderer.setSize(WIDTH, HEIGHT, false);
renderer.setClearColor(0x101018, 1);

const scene = new Scene();
const camera = new PerspectiveCamera(60, WIDTH / HEIGHT, 0.1, 1000);
camera.position.set(0, 0, 18);
camera.lookAt(0, 0, 0);

scene.add(new AmbientLight(0x404050, 1.2));
const dirLight = new DirectionalLight(0xffffff, 1.8);
dirLight.position.set(5, 10, 7);
scene.add(dirLight);

const geometry = new BoxGeometry(0.3, 0.3, 0.3);
const material = new MeshStandardMaterial({ roughness: 0.45, metalness: 0.15 });
const instancedMesh = new InstancedMesh(geometry, material, INSTANCE_COUNT);
scene.add(instancedMesh);

const dummy = new Object3D();
const tempColor = new Color();

function stepScene(frame) {
    const t = frame * 0.05;
    const gridDim = 50;
    for (let i = 0; i < INSTANCE_COUNT; i++) {
        const gx = (i % gridDim) - 25;
        const gy = ((i / gridDim) | 0) - 25;
        const x = gx * 0.4 + Math.sin(t + i * 0.05) * 0.15;
        const y = gy * 0.4 + Math.cos(t + i * 0.05) * 0.15;
        const z = Math.sin(t + (gx + gy) * 0.1) * 0.5;
        dummy.position.set(x, y, z);
        dummy.rotation.set(t + i * 0.02, t * 0.5 + i * 0.03, 0);
        const s = 0.8 + 0.2 * Math.sin(t + i * 0.1);
        dummy.scale.set(s, s, s);
        dummy.updateMatrix();
        instancedMesh.setMatrixAt(i, dummy.matrix);

        const r = 0.5 + 0.5 * Math.sin(t + gx * 0.2);
        const g = 0.5 + 0.5 * Math.cos(t + gy * 0.2);
        const b = 0.5 + 0.5 * Math.sin(t + (gx + gy) * 0.2);
        tempColor.setRGB(r, g, b);
        instancedMesh.setColorAt(i, tempColor);
    }
    instancedMesh.instanceMatrix.needsUpdate = true;
    if (instancedMesh.instanceColor !== null) {
        instancedMesh.instanceColor.needsUpdate = true;
    }
    renderer.render(scene, camera);
}

const gl = renderer.getContext();
const syncPx = new Uint8Array(4);
const fullPx = new Uint8Array(WIDTH * HEIGHT * 4);

let phase = 0;
let rafCount = 0;
let tRafStart = 0;
let batchResult = null;

function tick() {
    if (phase === 0) {
        for (let f = 0; f < WARMUP_FRAMES; f++) {
            stepScene(f);
        }
        gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);

        const t0 = nowMs();
        for (let f = 0; f < MEASURED_FRAMES; f++) {
            stepScene(WARMUP_FRAMES + f);
        }
        const t1 = nowMs();
        gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
        const t2 = nowMs();

        gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, fullPx);
        const cIdx = ((HEIGHT >> 1) * WIDTH + (WIDTH >> 1)) * 4;
        const pixelOk = (fullPx[cIdx] > 15 || fullPx[cIdx + 1] > 15 || fullPx[cIdx + 2] > 15) &&
                        renderer.info.render.triangles === INSTANCE_COUNT * 12;

        batchResult = {
            scene: 'instanced_2500_webgl2',
            frames: MEASURED_FRAMES,
            cpu_gl_ms: +(t1 - t0).toFixed(3),
            total_ms: +(t2 - t0).toFixed(3),
            ms_per_frame: +((t2 - t0) / MEASURED_FRAMES).toFixed(3),
            fps: +(1000.0 / ((t2 - t0) / MEASURED_FRAMES)).toFixed(1),
            draw_calls: renderer.info.render.calls,
            triangles: renderer.info.render.triangles,
            pixel_ok: pixelOk
        };

        if (!isBroHeadless) {
            console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
            return;
        }
        phase = 1;
        rafCount = 0;
        tRafStart = nowMs();
        requestAnimationFrame(tick);
        return;
    }

    if (phase === 1) {
        stepScene(WARMUP_FRAMES + MEASURED_FRAMES + rafCount);
        rafCount++;
        if (rafCount < ENGINE_RAF_FRAMES) {
            requestAnimationFrame(tick);
        } else {
            gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
            const tRafEnd = nowMs();
            const engineTotalMs = tRafEnd - tRafStart;
            batchResult.engine_frames = ENGINE_RAF_FRAMES;
            batchResult.engine_ms_per_frame = +(engineTotalMs / ENGINE_RAF_FRAMES).toFixed(3);
            batchResult.engine_fps = +(1000.0 / (engineTotalMs / ENGINE_RAF_FRAMES)).toFixed(1);
            console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
            phase = 2;
        }
    }
}

requestAnimationFrame(tick);
"""

SCENE_2K_MESHES_JS = COMMON_JS_HEADER + """
import {
    Scene, PerspectiveCamera, WebGLRenderer, BoxGeometry,
    Mesh, MeshLambertMaterial, DirectionalLight, AmbientLight, Group
} from './three.module.js';

const CLUSTERS = 20;
const MESHES_PER_CLUSTER = 100;
const WARMUP_FRAMES = 15;
const MEASURED_FRAMES = 90;
const ENGINE_RAF_FRAMES = 45;

const canvas = document.createElement('canvas');
canvas.width = WIDTH;
canvas.height = HEIGHT;
document.body.appendChild(canvas);

const renderer = new WebGLRenderer({ canvas: canvas, antialias: false, powerPreference: 'high-performance' });
renderer.setSize(WIDTH, HEIGHT, false);
renderer.setClearColor(0x0c0e14, 1);

const scene = new Scene();
const camera = new PerspectiveCamera(60, WIDTH / HEIGHT, 0.1, 500);
camera.position.set(0, 8, 32);
camera.lookAt(0, 0, 0);

scene.add(new AmbientLight(0x505060, 1.2));
const dirLight = new DirectionalLight(0xffffff, 1.6);
dirLight.position.set(10, 20, 15);
scene.add(dirLight);

const clusters = [];
const geometries = [];
const allMeshes = [];

for (let c = 0; c < CLUSTERS; c++) {
    const group = new Group();
    const angle = (c / CLUSTERS) * Math.PI * 2;
    group.position.set(Math.cos(angle) * 10, Math.sin(angle) * 6, (c % 5) * -2.0);
    scene.add(group);
    clusters.push(group);

    const geom = new BoxGeometry(0.45, 0.45, 0.45);
    geometries.push(geom);
    const mat = new MeshLambertMaterial({ color: 0x3388ff + (c * 0x050200) });

    for (let m = 0; m < MESHES_PER_CLUSTER; m++) {
        const mesh = new Mesh(geom, mat);
        const ma = (m / MESHES_PER_CLUSTER) * Math.PI * 2;
        const r = 1.5 + (m % 10) * 0.35;
        mesh.position.set(Math.cos(ma) * r, Math.sin(ma) * r, ((m % 5) - 2) * 0.5);
        group.add(mesh);
        allMeshes.push(mesh);
    }
}

function stepScene(frame) {
    const t = frame * 0.04;
    for (let c = 0; c < CLUSTERS; c++) {
        const g = clusters[c];
        g.rotation.y = t + c * 0.15;
        g.rotation.x = Math.sin(t + c) * 0.3;

        const geom = geometries[c];
        const pos = geom.attributes.position;
        const count = pos.count;
        for (let v = 0; v < count; v++) {
            const ox = pos.getX(v);
            const oy = pos.getY(v);
            const oz = pos.getZ(v);
            pos.setZ(v, oz + Math.sin(t + ox * 2.0 + oy * 2.0) * 0.005);
        }
        pos.needsUpdate = true;
    }

    for (let i = 0; i < allMeshes.length; i += 4) {
        allMeshes[i].rotation.z = t * 1.5;
    }

    camera.position.x = Math.sin(t * 0.5) * 6.0;
    camera.position.y = 6.0 + Math.cos(t * 0.3) * 3.0;
    camera.lookAt(0, 0, 0);

    renderer.render(scene, camera);
}

const gl = renderer.getContext();
const syncPx = new Uint8Array(4);
const fullPx = new Uint8Array(WIDTH * HEIGHT * 4);

let phase = 0;
let rafCount = 0;
let tRafStart = 0;
let batchResult = null;

function tick() {
    if (phase === 0) {
        for (let f = 0; f < WARMUP_FRAMES; f++) {
            stepScene(f);
        }
        gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);

        const t0 = nowMs();
        for (let f = 0; f < MEASURED_FRAMES; f++) {
            stepScene(WARMUP_FRAMES + f);
        }
        const t1 = nowMs();
        gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
        const t2 = nowMs();

        gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, fullPx);
        let nonClear = 0;
        for (let i = 0; i < fullPx.length; i += 64) {
            if (fullPx[i] > 25 || fullPx[i + 1] > 25 || fullPx[i + 2] > 35) nonClear++;
        }
        const pixelOk = nonClear > 10 && renderer.info.render.calls >= 100;

        batchResult = {
            scene: 'scene_2k_webgl2',
            frames: MEASURED_FRAMES,
            cpu_gl_ms: +(t1 - t0).toFixed(3),
            total_ms: +(t2 - t0).toFixed(3),
            ms_per_frame: +((t2 - t0) / MEASURED_FRAMES).toFixed(3),
            fps: +(1000.0 / ((t2 - t0) / MEASURED_FRAMES)).toFixed(1),
            draw_calls: renderer.info.render.calls,
            triangles: renderer.info.render.triangles,
            pixel_ok: pixelOk
        };

        if (!isBroHeadless) {
            console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
            return;
        }
        phase = 1;
        rafCount = 0;
        tRafStart = nowMs();
        requestAnimationFrame(tick);
        return;
    }

    if (phase === 1) {
        stepScene(WARMUP_FRAMES + MEASURED_FRAMES + rafCount);
        rafCount++;
        if (rafCount < ENGINE_RAF_FRAMES) {
            requestAnimationFrame(tick);
        } else {
            gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
            const tRafEnd = nowMs();
            const engineTotalMs = tRafEnd - tRafStart;
            batchResult.engine_frames = ENGINE_RAF_FRAMES;
            batchResult.engine_ms_per_frame = +(engineTotalMs / ENGINE_RAF_FRAMES).toFixed(3);
            batchResult.engine_fps = +(1000.0 / (engineTotalMs / ENGINE_RAF_FRAMES)).toFixed(1);
            console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
            phase = 2;
        }
    }
}

requestAnimationFrame(tick);
"""

SCENE_PBR_ORBIT_JS = COMMON_JS_HEADER + """
import {
    Scene, PerspectiveCamera, WebGLRenderer, BoxGeometry, SphereGeometry,
    TorusKnotGeometry, MeshStandardMaterial, Mesh, DirectionalLight,
    PointLight, AmbientLight
} from './three.module.js';
import { OrbitControls } from './OrbitControls.js';

const WARMUP_FRAMES = 20;
const MEASURED_FRAMES = 120;
const ENGINE_RAF_FRAMES = 60;

const canvas = document.createElement('canvas');
canvas.width = WIDTH;
canvas.height = HEIGHT;
document.body.appendChild(canvas);

const renderer = new WebGLRenderer({ canvas: canvas, antialias: false, powerPreference: 'high-performance' });
renderer.setSize(WIDTH, HEIGHT, false);
renderer.setClearColor(0x101218, 1);

const scene = new Scene();
const camera = new PerspectiveCamera(60, WIDTH / HEIGHT, 0.1, 1000);
camera.position.set(0, 4, 14);
camera.lookAt(0, 0, 0);

const controls = new OrbitControls(camera, canvas);
controls.enableDamping = true;
controls.dampingFactor = 0.08;
controls.autoRotate = true;
controls.autoRotateSpeed = 4.0;

scene.add(new AmbientLight(0x404050, 1.0));
const dirLight = new DirectionalLight(0xffffff, 1.8);
dirLight.position.set(6, 10, 8);
scene.add(dirLight);

const pointLights = [];
const lightColors = [0xff4444, 0x44ff88, 0x4488ff];
for (let i = 0; i < 3; i++) {
    const pl = new PointLight(lightColors[i], 25, 30);
    scene.add(pl);
    pointLights.push(pl);
}

const meshes = [];
const knotGeom = new TorusKnotGeometry(1.2, 0.35, 96, 16);
const sphereGeom = new SphereGeometry(0.65, 24, 16);
const boxGeom = new BoxGeometry(0.9, 0.9, 0.9);

for (let i = 0; i < 64; i++) {
    const gx = (i % 8) - 3.5;
    const gz = ((i / 8) | 0) - 3.5;
    const geom = (i % 3 === 0) ? knotGeom : ((i % 3 === 1) ? sphereGeom : boxGeom);
    const mat = new MeshStandardMaterial({
        color: 0x6688aa + (i * 0x020301),
        roughness: 0.15 + (i % 7) * 0.12,
        metalness: 0.1 + (i % 5) * 0.18
    });
    const m = new Mesh(geom, mat);
    m.position.set(gx * 2.2, Math.sin(i) * 0.8, gz * 2.2);
    scene.add(m);
    meshes.push(m);
}

function stepScene(frame) {
    const t = frame * 0.05;
    for (let i = 0; i < 3; i++) {
        const a = t * 1.2 + (i * Math.PI * 2 / 3);
        pointLights[i].position.set(Math.cos(a) * 8, 3 + Math.sin(t * 2 + i) * 2, Math.sin(a) * 8);
    }
    for (let i = 0; i < meshes.length; i++) {
        const m = meshes[i];
        m.rotation.x = t * 0.7 + i * 0.1;
        m.rotation.y = t * 0.9 + i * 0.15;
    }
    controls.update();
    renderer.render(scene, camera);
}

const gl = renderer.getContext();
const syncPx = new Uint8Array(4);
const fullPx = new Uint8Array(WIDTH * HEIGHT * 4);

let phase = 0;
let rafCount = 0;
let tRafStart = 0;
let batchResult = null;

function tick() {
    if (phase === 0) {
        for (let f = 0; f < WARMUP_FRAMES; f++) {
            stepScene(f);
        }
        gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);

        const t0 = nowMs();
        for (let f = 0; f < MEASURED_FRAMES; f++) {
            stepScene(WARMUP_FRAMES + f);
        }
        const t1 = nowMs();
        gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
        const t2 = nowMs();

        gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, fullPx);
        let nonClear = 0;
        for (let i = 0; i < fullPx.length; i += 64) {
            if (fullPx[i] > 25 || fullPx[i + 1] > 25 || fullPx[i + 2] > 35) nonClear++;
        }
        const pixelOk = nonClear > 10 && renderer.info.render.triangles > 1000;

        batchResult = {
            scene: 'pbr_orbit_webgl2',
            frames: MEASURED_FRAMES,
            cpu_gl_ms: +(t1 - t0).toFixed(3),
            total_ms: +(t2 - t0).toFixed(3),
            ms_per_frame: +((t2 - t0) / MEASURED_FRAMES).toFixed(3),
            fps: +(1000.0 / ((t2 - t0) / MEASURED_FRAMES)).toFixed(1),
            draw_calls: renderer.info.render.calls,
            triangles: renderer.info.render.triangles,
            pixel_ok: pixelOk
        };

        if (!isBroHeadless) {
            console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
            return;
        }
        phase = 1;
        rafCount = 0;
        tRafStart = nowMs();
        requestAnimationFrame(tick);
        return;
    }

    if (phase === 1) {
        stepScene(WARMUP_FRAMES + MEASURED_FRAMES + rafCount);
        rafCount++;
        if (rafCount < ENGINE_RAF_FRAMES) {
            requestAnimationFrame(tick);
        } else {
            gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
            const tRafEnd = nowMs();
            const engineTotalMs = tRafEnd - tRafStart;
            batchResult.engine_frames = ENGINE_RAF_FRAMES;
            batchResult.engine_ms_per_frame = +(engineTotalMs / ENGINE_RAF_FRAMES).toFixed(3);
            batchResult.engine_fps = +(1000.0 / (engineTotalMs / ENGINE_RAF_FRAMES)).toFixed(1);
            console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
            phase = 2;
        }
    }
}

requestAnimationFrame(tick);
"""


def build_pixi_scene_js() -> str:
    probe_path = BRO_ROOT / "tests" / "bronze_host" / "apps" / "pixi_sprites_probe.js"
    probe_text = probe_path.read_text(encoding="utf-8")
    cut_marker = "const WIDTH = 320;"
    idx = probe_text.find(cut_marker)
    if idx == -1:
        raise RuntimeError("Could not find polyfill boundary in pixi_sprites_probe.js")
    polyfill_header = probe_text[:idx].replace("from 'pixi.js';", "from './pixi.mjs';")

    pixi_body = COMMON_JS_HEADER + """
const SPRITE_COUNT = 1000;
const WARMUP_FRAMES = 20;
const MEASURED_FRAMES = 120;
const ENGINE_RAF_FRAMES = 60;

const canvas = document.createElement('canvas');
canvas.width = WIDTH;
canvas.height = HEIGHT;
document.body.appendChild(canvas);

const app = new Application();
app.init({
    canvas: canvas,
    width: WIDTH,
    height: HEIGHT,
    preference: 'webgl',
    backgroundColor: 0x101018,
    autoStart: false
}).then(function () {
    const container = new Container();
    app.stage.addChild(container);

    const sprites = [];
    for (let i = 0; i < SPRITE_COUNT; i++) {
        const s = new Sprite(Texture.WHITE);
        s.width = 16;
        s.height = 16;
        s.anchor.set(0.5, 0.5);
        s.x = (i % 40) * 16 + 8;
        s.y = ((i / 40) | 0) * 18 + 16;
        s.tint = ((i * 73) & 0xff) << 16 | (((i * 151) & 0xff) << 8) | ((i * 211) & 0xff);
        container.addChild(s);
        sprites.push(s);
    }

    function stepScene(frame) {
        const t = frame * 0.06;
        for (let i = 0; i < SPRITE_COUNT; i++) {
            const s = sprites[i];
            const gx = (i % 40) * 16 + 8;
            const gy = ((i / 40) | 0) * 18 + 16;
            s.x = gx + Math.sin(t + i * 0.1) * 12;
            s.y = gy + Math.cos(t + i * 0.1) * 12;
            s.rotation = t + i * 0.02;
            const sc = 0.8 + 0.3 * Math.sin(t + i * 0.05);
            s.scale.set(sc, sc);
        }
        app.render();
    }

    const gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
    const syncPx = new Uint8Array(4);
    const fullPx = new Uint8Array(WIDTH * HEIGHT * 4);

    let phase = 0;
    let rafCount = 0;
    let tRafStart = 0;
    let batchResult = null;

    function tick() {
        if (phase === 0) {
            for (let f = 0; f < WARMUP_FRAMES; f++) {
                stepScene(f);
            }
            gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);

            const t0 = nowMs();
            for (let f = 0; f < MEASURED_FRAMES; f++) {
                stepScene(WARMUP_FRAMES + f);
            }
            const t1 = nowMs();
            gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
            const t2 = nowMs();

            gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, fullPx);
            let nonClear = 0;
            for (let i = 0; i < fullPx.length; i += 64) {
                if (fullPx[i] > 30 || fullPx[i + 1] > 30 || fullPx[i + 2] > 40) nonClear++;
            }
            const pixelOk = nonClear > 10;

            batchResult = {
                scene: 'pixi_1k_sprites_webgl2',
                frames: MEASURED_FRAMES,
                cpu_gl_ms: +(t1 - t0).toFixed(3),
                total_ms: +(t2 - t0).toFixed(3),
                ms_per_frame: +((t2 - t0) / MEASURED_FRAMES).toFixed(3),
                fps: +(1000.0 / ((t2 - t0) / MEASURED_FRAMES)).toFixed(1),
                draw_calls: 1,
                triangles: SPRITE_COUNT * 2,
                pixel_ok: pixelOk
            };

            if (!isBroHeadless) {
                console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
                return;
            }
            phase = 1;
            rafCount = 0;
            tRafStart = nowMs();
            requestAnimationFrame(tick);
            return;
        }

        if (phase === 1) {
            stepScene(WARMUP_FRAMES + MEASURED_FRAMES + rafCount);
            rafCount++;
            if (rafCount < ENGINE_RAF_FRAMES) {
                requestAnimationFrame(tick);
            } else {
                gl.readPixels(WIDTH >> 1, HEIGHT >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, syncPx);
                const tRafEnd = nowMs();
                const engineTotalMs = tRafEnd - tRafStart;
                batchResult.engine_frames = ENGINE_RAF_FRAMES;
                batchResult.engine_ms_per_frame = +(engineTotalMs / ENGINE_RAF_FRAMES).toFixed(3);
                batchResult.engine_fps = +(1000.0 / (engineTotalMs / ENGINE_RAF_FRAMES)).toFixed(1);
                console.log('BENCH_RESULT ' + JSON.stringify(batchResult));
                phase = 2;
            }
        }
    }

    requestAnimationFrame(tick);
});
"""
    return polyfill_header + "\n" + pixi_body


class QuietHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


def setup_scene_dirs(base_dir: Path, scene_name: str, scene_js: str):
    aot_dir = base_dir / f"{scene_name}_aot"
    jit_dir = base_dir / f"{scene_name}_jit"
    aot_dir.mkdir(parents=True, exist_ok=True)
    jit_dir.mkdir(parents=True, exist_ok=True)

    orbit_src = (ORACLE_THREE / "OrbitControls.js").read_text(encoding="utf-8")
    orbit_patched = orbit_src.replace("from 'three';", "from './three.module.js';")

    for d in (aot_dir, jit_dir):
        for fname in ("three.module.js",):
            dst = d / fname
            if not dst.exists():
                os.symlink(ORACLE_THREE / fname, dst)
        pixi_dst = d / "pixi.mjs"
        if not pixi_dst.exists() and (ORACLE_PIXI / "pixi.mjs").exists():
            os.symlink(ORACLE_PIXI / "pixi.mjs", pixi_dst)
        (d / "OrbitControls.js").write_text(orbit_patched, encoding="utf-8")
        (d / "scene.js").write_text(scene_js, encoding="utf-8")

    (aot_dir / "bro.json").write_text(json.dumps({
        "title": scene_name,
        "width": 640,
        "height": 480,
        "splash": False,
        "compiled": True
    }, indent=2), encoding="utf-8")
    (aot_dir / "index.html").write_text(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<style>html,body{margin:0;padding:0;}canvas{width:640px;height:480px;}</style>"
        "</head><body></body></html>\n",
        encoding="utf-8"
    )

    (jit_dir / "bro.json").write_text(json.dumps({
        "title": scene_name,
        "width": 640,
        "height": 480,
        "splash": False,
        "compiled": False
    }, indent=2), encoding="utf-8")
    (jit_dir / "index.html").write_text(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<style>html,body{margin:0;padding:0;}canvas{width:640px;height:480px;}</style>"
        "</head><body><script type='module' src='./scene.js'></script></body></html>\n",
        encoding="utf-8"
    )
    return aot_dir, jit_dir


def compile_aot_module(bro_headless: Path, bronze_bin: Path, rt_lib: Path, aot_dir: Path) -> float:
    manifest = aot_dir / "app.host-globals"
    natives = aot_dir / "app.natives.json"
    module_so = aot_dir / "app.so"

    res = subprocess.run(
        [str(bro_headless), str(aot_dir), "--print-host-globals", "--print-native-manifest", str(natives)],
        capture_output=True, text=True, check=True
    )
    manifest.write_text(res.stdout, encoding="utf-8")

    env = os.environ.copy()
    env["BRONZE_SHARED_RT_LIB"] = str(rt_lib)
    env["BRO_PROJECT_ROOT"] = str(BRO_ROOT)

    t0 = time.perf_counter()
    subprocess.run(
        [
            str(bronze_bin), "build", str(aot_dir / "scene.js"),
            "-o", str(module_so),
            "--emit-shared",
            "--host-globals", str(manifest),
            "--native-manifest", str(natives),
        ],
        env=env, capture_output=True, text=True, check=True
    )
    return (time.perf_counter() - t0) * 1000.0


def run_bro_headless(bro_headless: Path, app_dir: Path, code_cache_dir: Path) -> dict:
    env = os.environ.copy()
    env["BRO_CODE_CACHE_DIR"] = str(code_cache_dir)
    env["BRO_TEST_VK_VALIDATION"] = "0"
    res = subprocess.run(
        [str(bro_headless), str(app_dir), "-e", "advanceTime(80 * 16);"],
        env=env, capture_output=True, text=True, timeout=120
    )
    for line in (res.stdout + "\n" + res.stderr).splitlines():
        if "BENCH_RESULT " in line:
            payload = line.split("BENCH_RESULT ", 1)[1].strip()
            return json.loads(payload)
    raise RuntimeError(f"No BENCH_RESULT in bro-headless output for {app_dir}:\n{res.stdout[-600:]}\n{res.stderr[-600:]}")


def run_chromium_headless(url: str) -> dict:
    proc = subprocess.Popen(
        [
            "chromium",
            "--headless",
            "--no-sandbox",
            "--disable-extensions",
            "--disable-gpu-vsync",
            "--disable-frame-rate-limit",
            "--use-gl=angle",
            "--use-angle=vulkan",
            "--ignore-gpu-blocklist",
            "--window-size=640,480",
            "--enable-logging=stderr",
            url,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    result = None
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            if "BENCH_RESULT " in line:
                payload = line.split("BENCH_RESULT ", 1)[1]
                end_idx = payload.rfind("}")
                if end_idx != -1:
                    payload = payload[: end_idx + 1]
                result = json.loads(payload)
                proc.terminate()
                break
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
        raise
    if result is None:
        raise RuntimeError(f"Chromium failed to produce BENCH_RESULT for {url}")
    return result


def summarize_runs(runs: list[dict]) -> dict:
    first = runs[0].copy()
    for k in ("cpu_gl_ms", "total_ms", "ms_per_frame", "fps", "engine_ms_per_frame", "engine_fps"):
        if k in first:
            vals = [r[k] for r in runs if k in r]
            first[k] = round(statistics.median(vals), 3 if "ms" in k else 1)
    first["all_ms_per_frame"] = [r["ms_per_frame"] for r in runs]
    return first


def run_webgl_suite(reps: int) -> list[dict]:
    bro_headless, bronze_bin, rt_lib = find_bro_binaries()
    assert bronze_bin is not None and rt_lib is not None, "bronze CLI and shared runtime required"

    scenes = [
        ("instanced_2500_webgl2", SCENE_INSTANCED_JS),
        ("scene_2k_webgl2", SCENE_2K_MESHES_JS),
        ("pbr_orbit_webgl2", SCENE_PBR_ORBIT_JS),
        ("pixi_1k_sprites_webgl2", build_pixi_scene_js()),
    ]

    results = []
    with tempfile.TemporaryDirectory(prefix="bro_webgl_bench_") as tmp:
        base_dir = Path(tmp)
        code_cache = base_dir / "code_cache"
        code_cache.mkdir(parents=True, exist_ok=True)

        handler = lambda *a, **kw: QuietHTTPRequestHandler(*a, directory=str(base_dir), **kw)
        with socketserver.TCPServer(("127.0.0.1", 0), handler) as httpd:
            port = httpd.server_address[1]
            server_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
            server_thread.start()

            for scene_name, scene_js in scenes:
                print(f"\n=== Scene: {scene_name} ===", flush=True)
                aot_dir, jit_dir = setup_scene_dirs(base_dir, scene_name, scene_js)

                print("  Compiling AOT app.so via bronze...", end="", flush=True)
                compile_ms = compile_aot_module(bro_headless, bronze_bin, rt_lib, aot_dir)
                print(f" done in {compile_ms:.0f} ms", flush=True)

                # Prime JIT code cache once
                run_bro_headless(bro_headless, jit_dir, code_cache)

                aot_runs, jit_runs, chr_runs = [], [], []
                url = f"http://127.0.0.1:{port}/{scene_name}_jit/index.html"

                for r in range(reps):
                    aot_res = run_bro_headless(bro_headless, aot_dir, code_cache)
                    jit_res = run_bro_headless(bro_headless, jit_dir, code_cache)
                    chr_res = run_chromium_headless(url)
                    aot_runs.append(aot_res)
                    jit_runs.append(jit_res)
                    chr_runs.append(chr_res)
                    print(
                        f"  [rep {r + 1}/{reps}] "
                        f"bro-AOT: {aot_res['ms_per_frame']:.2f} ms/f ({aot_res['fps']:.1f} FPS, "
                        f"full-engine: {aot_res.get('engine_ms_per_frame', 0):.2f} ms/f) | "
                        f"bro-JIT: {jit_res['ms_per_frame']:.2f} ms/f ({jit_res['fps']:.1f} FPS) | "
                        f"Chromium-V8: {chr_res['ms_per_frame']:.2f} ms/f ({chr_res['fps']:.1f} FPS)",
                        flush=True,
                    )

                results.append({
                    "scene": scene_name,
                    "aot_compile_ms": round(compile_ms, 1),
                    "bro_aot": summarize_runs(aot_runs),
                    "bro_jit": summarize_runs(jit_runs),
                    "chromium_v8": summarize_runs(chr_runs),
                })

    return results


def print_webgl_summary(webgl_results: list[dict]):
    print("\n" + "=" * 112)
    print("REAL WEBGL2 APPLICATION BENCHMARKS (640x480, Vulkan GPU, Warmed Steady-State)")
    print("=" * 112)
    header = (
        f"{'Scene':<24} | {'bro AOT (ms/f / FPS)':<24} | {'bro Full-Frame':<16} | "
        f"{'bro JIT (ms/f / FPS)':<24} | {'Chromium V8 (ms/f / FPS)':<24} | {'Pixels'}"
    )
    print(header)
    print("-" * 112)
    for item in webgl_results:
        s = item["scene"]
        aot = item["bro_aot"]
        jit = item["bro_jit"]
        ch = item["chromium_v8"]
        ok = "PASS" if (aot["pixel_ok"] and jit["pixel_ok"] and ch["pixel_ok"]) else "FAIL"
        aot_str = f"{aot['ms_per_frame']:6.2f} ms ({aot['fps']:6.1f} FPS)"
        eng_str = f"{aot.get('engine_ms_per_frame', 0):5.2f} ms ({aot.get('engine_fps', 0):5.1f})"
        jit_str = f"{jit['ms_per_frame']:6.2f} ms ({jit['fps']:6.1f} FPS)"
        ch_str = f"{ch['ms_per_frame']:6.2f} ms ({ch['fps']:6.1f} FPS)"
        print(f"{s:<24} | {aot_str:<24} | {eng_str:<16} | {jit_str:<24} | {ch_str:<24} | {ok}")
    print("=" * 112)


def main():
    parser = argparse.ArgumentParser(description="Three.js & Pixi.js vs V8/Chromium Benchmark Suite")
    parser.add_argument("--reps", type=int, default=3, help="Repetitions per WebGL scene (default: 3)")
    parser.add_argument("--json-out", type=str, default="/tmp/threejs_v8_webgl_report.json")
    args = parser.parse_args()

    webgl_results = run_webgl_suite(args.reps)
    print_webgl_summary(webgl_results)

    if args.json_out:
        Path(args.json_out).write_text(json.dumps({"webgl": webgl_results}, indent=2), encoding="utf-8")
        print(f"\nSaved JSON report to {args.json_out}")


if __name__ == "__main__":
    main()
