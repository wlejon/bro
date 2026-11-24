// scene_impostor.js — SceneGraph.bakeImpostor: what a scene draws, baked from
// a grid of directions into the texture arrays an impostor is drawn from
// (the hook side is the engine's `impostor` module, shaders/common/impostor.bk).
//
// The bake captures the scene's own surfaces as their hooks leave them (the
// debug views `albedo`, `normals`, `view_depth` and the caller's), so an
// impostor is the subject's material, not a second description of it.
//
// Entered after js/scene_graph.js (installSceneModule, host_js_modules.cpp),
// whose setCamera / setDebugView / captureFrame it drives.
(function () {
    'use strict';

    const SceneGraph = globalThis.SceneGraph;
    if (typeof SceneGraph !== 'function') return;
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    // The display's encoding undone: a debug view's bytes are its values encoded.
    const LIN = new Float32Array(256);
    for (let i = 0; i < 256; i++) { const x = i / 255; LIN[i] = x <= 0.04045 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4); }

    // View `layer` of a g x g grid: its direction (the centre toward its
    // camera), right and up. impostor.bk's view_dir / view_right.
    function viewFrame(layer, g) {
        const ex = (layer % g) / (g - 1) * 2 - 1, ey = Math.floor(layer / g) / (g - 1) * 2 - 1;
        const tx = (ex + ey) * 0.5, tz = (ex - ey) * 0.5;
        let d = [tx, 1 - Math.abs(tx) - Math.abs(tz), tz];
        const l = Math.hypot(d[0], d[1], d[2]);
        d = [d[0] / l, d[1] / l, d[2] / l];
        let r = [1, 0, 0];
        if (Math.abs(d[1]) <= 0.999) { const rl = Math.hypot(d[2], d[0]); r = [d[2] / rl, 0, -d[0] / rl]; }
        const u = [d[1] * r[2] - d[2] * r[1], d[2] * r[0] - d[0] * r[2], d[0] * r[1] - d[1] * r[0]];
        return { d, r, u };
    }

    fn(SceneGraph.prototype, "bakeImpostor", function bakeImpostor(opts) {
        const o = opts || {};
        const c = o.center || [0, 0, 0], R = +o.radius;
        if (!(R > 0)) throw new RangeError("bakeImpostor: radius must be positive");
        const G = o.grid === undefined ? 8 : o.grid | 0, W = o.size === undefined ? 256 : o.size | 0;
        const SS = o.supersample === undefined ? 2 : o.supersample | 0;
        if (G < 2 || W < 1 || SS < 1) throw new RangeError("bakeImpostor: grid is at least 2, size and supersample at least 1");
        const aux = o.aux === undefined ? 'sky_visibility' : o.aux;
        const masks = (o.masks || []).slice(0, 4);
        const V = G * G, B = W * SS, n = SS * SS, D = 32 * R;
        const albedo = new Float32Array(V * W * W * 4), surface = new Float32Array(V * W * W * 4);
        const mask = masks.length ? new Float32Array(V * W * W * 4) : null;
        const near = new Int32Array(W * W);
        const extent = [0, Infinity, -Infinity];
        const mean = [0, 0, 0];
        let meanCov = 0;
        const shot = (view) => {
            this.setDebugView(view);
            const f = this.captureFrame(B, B);
            if (!f || f.width !== B || f.height !== B) throw new Error("bakeImpostor: the capture came back " + (f && f.width) + " x " + (f && f.height) + " for " + B);
            return f.data;
        };
        for (let vi = 0; vi < V; vi++) {
            const { d, r, u } = viewFrame(vi, G);
            this.setCamera({ mode: 'orthographic', size: 2 * R, aspect: 1, near: D - R, far: D + R,
                eye: [c[0] + d[0] * D, c[1] + d[1] * D, c[2] + d[2] * D], target: c, up: u });
            const A = shot('albedo'), N = shot('normals'), Z = shot('view_depth');
            const X = aux ? shot(aux) : null;
            const M = masks.map(shot);
            const base = vi * W * W * 4;
            const vm = [0, 0, 0, 0, 0, 0, 0, 0, 0];   // albedo, normal (image x, y, toward the camera), depth, aux, coverage
            for (let ty = 0; ty < W; ty++) for (let tx = 0; tx < W; tx++) {
                let k = 0, a0 = 0, a1 = 0, a2 = 0, nx = 0, ny = 0, nz = 0, z = 0, x = 0;
                const m = [0, 0, 0, 0];
                for (let sy = 0; sy < SS; sy++) for (let sx = 0; sx < SS; sx++) {
                    const p = ((ty * SS + sy) * B + tx * SS + sx) * 4;
                    if (A[p + 3] < 128) continue;
                    k++;
                    a0 += LIN[A[p]]; a1 += LIN[A[p + 1]]; a2 += LIN[A[p + 2]];
                    const wx = LIN[N[p]] * 2 - 1, wy = LIN[N[p + 1]] * 2 - 1, wz = LIN[N[p + 2]] * 2 - 1;
                    nx += wx * r[0] + wy * r[1] + wz * r[2];
                    ny += wx * u[0] + wy * u[1] + wz * u[2];
                    nz += wx * d[0] + wy * d[1] + wz * d[2];
                    z += LIN[Z[p]];
                    if (X) x += LIN[X[p]];
                    for (let j = 0; j < M.length; j++) m[j] += LIN[M[j][p]];
                }
                const t = base + (ty * W + tx) * 4;
                albedo[t + 3] = k / n;
                if (!k) continue;
                // (A normal turned from the camera is kept at the image's rim.)
                const nl = Math.hypot(nx, ny, Math.max(nz, 0)) || 1;
                albedo[t] = Math.sqrt(a0 / k); albedo[t + 1] = Math.sqrt(a1 / k); albedo[t + 2] = Math.sqrt(a2 / k);
                surface[t] = nx / nl * 0.5 + 0.5; surface[t + 1] = ny / nl * 0.5 + 0.5;
                surface[t + 2] = z / k; surface[t + 3] = X ? x / k : 1;
                if (mask) for (let j = 0; j < M.length; j++) mask[t + j] = m[j] / k;
                vm[0] += a0; vm[1] += a1; vm[2] += a2; vm[3] += nx; vm[4] += ny; vm[5] += Math.max(nz, 0); vm[6] += z; vm[7] += x; vm[8] += k;
                const px = (tx + 0.5) * 2 / W - 1, py = 1 - (ty + 0.5) * 2 / W;
                extent[0] = Math.max(extent[0], Math.abs(px) + 1 / W);
                extent[1] = Math.min(extent[1], py - 1 / W);
                extent[2] = Math.max(extent[2], py + 1 / W);
            }
            // A texel the subject does not reach holds what the covered
            // texel nearest it does (two sweeps, each texel taking its
            // nearest from the neighbours already passed), so filtering
            // across an outline mixes in nothing that is not there: a trunk
            // a texel wide stays bark, not the leaves' mean. Its depth too,
            // near the subject: a ray that passes beside a thin part is
            // still stepped to that part's depth, where the next step finds
            // it (impostor.bk refine).
            const k = vm[8] || 1, nl = Math.hypot(vm[3], vm[4], vm[5]) || 1;
            near.fill(-1);
            for (let i = 0; i < W * W; i++) if (albedo[base + i * 4 + 3] > 0) near[i] = i;
            const take = (i, x, y, j) => {
                const q = near[j];
                if (q < 0) return;
                const p = near[i];
                const dq = (q % W - x) ** 2 + (Math.floor(q / W) - y) ** 2;
                if (p < 0 || dq < (p % W - x) ** 2 + (Math.floor(p / W) - y) ** 2) near[i] = q;
            };
            for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) {
                const i = y * W + x;
                if (x > 0) take(i, x, y, i - 1);
                if (y > 0) { take(i, x, y, i - W); if (x > 0) take(i, x, y, i - W - 1); if (x < W - 1) take(i, x, y, i - W + 1); }
            }
            for (let y = W - 1; y >= 0; y--) for (let x = W - 1; x >= 0; x--) {
                const i = y * W + x;
                if (x < W - 1) take(i, x, y, i + 1);
                if (y < W - 1) { take(i, x, y, i + W); if (x < W - 1) take(i, x, y, i + W + 1); if (x > 0) take(i, x, y, i + W - 1); }
            }
            for (let i = 0; i < W * W; i++) {
                const t = base + i * 4;
                if (albedo[t + 3] > 0) continue;
                const q = near[i];
                if (q < 0) {
                    albedo[t] = Math.sqrt(vm[0] / k); albedo[t + 1] = Math.sqrt(vm[1] / k); albedo[t + 2] = Math.sqrt(vm[2] / k);
                    surface[t] = vm[3] / nl * 0.5 + 0.5; surface[t + 1] = vm[4] / nl * 0.5 + 0.5;
                    surface[t + 2] = 0.5; surface[t + 3] = X ? vm[7] / k : 1;
                    continue;
                }
                const s = base + q * 4;
                albedo[t] = albedo[s]; albedo[t + 1] = albedo[s + 1]; albedo[t + 2] = albedo[s + 2];
                surface[t] = surface[s]; surface[t + 1] = surface[s + 1]; surface[t + 3] = surface[s + 3];
                if (mask) for (let j = 0; j < M.length; j++) mask[t + j] = mask[s + j];
                // (The depth only near the subject: far from it, the centre's.)
                const far = Math.min(1, Math.max(0, (Math.hypot(q % W - i % W, Math.floor(q / W) - Math.floor(i / W)) * 256 / W - 6) / 18));
                surface[t + 2] = surface[s + 2] * (1 - far) + 0.5 * far;
            }
            mean[0] += vm[0]; mean[1] += vm[1]; mean[2] += vm[2]; meanCov += vm[8];
        }
        this.setDebugView(null);
        if (!(extent[1] <= extent[2])) { extent[0] = 0; extent[1] = 0; extent[2] = 0; }
        const tex = (data) => ({ data, width: W, height: W, layers: V, channels: 4, mipmap: true, format: 'unorm8' });
        return {
            grid: G, size: W, layers: V, center: [c[0], c[1], c[2]], radius: R,
            extent,
            meanAlbedo: mean.map((v) => v / (meanCov || 1)),
            coverage: meanCov / (n * V * W * W),
            albedo, surface, masks: mask,
            textures: { albedo: tex(albedo), surface: tex(surface), masks: mask ? tex(mask) : null },
        };
    });
})();
