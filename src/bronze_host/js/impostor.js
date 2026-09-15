// impostor.js — Octahedral impostor billboards on bro.impostor
(function () {
    'use strict';

    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});

    const CORNERS = [[-1, -1], [1, -1], [1, 1], [-1, 1]];

    const VERTEX_CHUNK = `
uniform vec2  u_grid;   // atlas grid (cols, rows)
uniform float u_half;   // billboard half-extent (world units at scale 1)
uniform vec2  u_cull;   // (fadeStart, cullEnd) metres from the camera

flat out vec2 v_uvMin;
flat out vec2 v_uvMax;
out float v_fade;       // 1 = solid, ->0 across the cull band (dither in fragment)

void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
    float scl = normal.x;                 // per-billboard scale carried in normal.x
    vec2 corner = uv;                     // corner sign in [-1,1]^2

    vec3 centerCR = (uModel * vec4(pos, 1.0)).xyz;
    float camDist = length(centerCR);
    v_fade = 1.0 - smoothstep(u_cull.x, u_cull.y, camDist);

    vec3 f = camDist > 1e-4 ? centerCR / camDist : vec3(0.0, 0.0, 1.0);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), f);
    float rl = length(right);
    right = rl > 1e-4 ? right / rl : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(f, right);

    vec3 offset = right * (corner.x * u_half * scl) + up * (corner.y * u_half * scl);
    pos = pos + offset;
    normal = -f;

    vec3 dir = -f;
    vec3 ad = abs(dir);
    vec3 d = dir / (ad.x + ad.y + ad.z);
    float coordX = d.x + d.z;
    float coordY = d.x - d.z;
    float col = floor(clamp(coordX * 0.5 + 0.5, 0.0, 0.999999) * u_grid.x);
    float row = floor(clamp(coordY * 0.5 + 0.5, 0.0, 0.999999) * u_grid.y);
    vec2 cellSz = vec2(1.0 / u_grid.x, 1.0 / u_grid.y);
    v_uvMin = vec2(col, row) * cellSz;
    v_uvMax = v_uvMin + cellSz;

    uv = corner * 0.5 + 0.5;

    if (v_fade <= 0.001) pos = pos - offset;
}
`;

    const FRAGMENT_CHUNK = `
flat in vec2 v_uvMin;
flat in vec2 v_uvMax;
in float v_fade;

void userFragment(inout vec3 baseColor, inout vec3 normal,
                  inout float metallic, inout float roughness,
                  inout vec3 emissive, inout float alpha) {
    vec2 cellUV = vec2(vUV.x, 1.0 - vUV.y);
    vec2 uv = v_uvMin + cellUV * (v_uvMax - v_uvMin);
    vec4 tex = texture(uBaseColorTex, uv);
    if (tex.a < 0.5) discard;

    if (v_fade < 0.999) {
        float hash = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        if (hash > v_fade) discard;
    }
    baseColor = vec3(0.0);
    emissive = tex.rgb;
    alpha = 1.0;
}
`;

    function createLayer(scene, impostor, transforms, opts) {
        opts = opts || {};
        const margin = opts.margin != null ? opts.margin : 1.03;
        const cullNear = opts.cullNear != null ? opts.cullNear : 450;
        const cullFar = opts.cullFar != null ? opts.cullFar : 950;

        const src = (transforms instanceof Float32Array) ? transforms : new Float32Array(transforms);
        const count = Math.floor(src.length / 9);

        const bnd = impostor.bounds || { center: [0, 0, 0], radius: 1 };
        const c = bnd.center;
        const half = Math.max(bnd.radius, 1e-3) * margin;

        const positions = new Float32Array(count * 4 * 3);
        const normals = new Float32Array(count * 4 * 3);
        const uvs = new Float32Array(count * 4 * 2);
        const indices = new Uint32Array(count * 6);
        for (let i = 0; i < count; i++) {
            const o = i * 9;
            const scl = src[o + 7];
            const cxw = src[o] + c[0] * scl, cyw = src[o + 1] + c[1] * scl, czw = src[o + 2] + c[2] * scl;
            for (let k = 0; k < 4; k++) {
                const v = i * 4 + k;
                positions[v * 3] = cxw; positions[v * 3 + 1] = cyw; positions[v * 3 + 2] = czw;
                normals[v * 3] = scl; normals[v * 3 + 1] = 0; normals[v * 3 + 2] = 0;
                uvs[v * 2] = CORNERS[k][0]; uvs[v * 2 + 1] = CORNERS[k][1];
            }
            const b = i * 4, t = i * 6;
            indices[t] = b; indices[t + 1] = b + 1; indices[t + 2] = b + 2;
            indices[t + 3] = b; indices[t + 4] = b + 2; indices[t + 5] = b + 3;
        }

        const rgba = impostor.atlasRGBA;
        const data = (rgba instanceof Uint8Array)
            ? rgba : new Uint8Array(rgba.buffer, rgba.byteOffset, rgba.length);

        const MeshCtor = globalThis.Mesh || (bro.mesh && bro.mesh.Mesh);
        const node = scene.createMesh({
            mesh: new MeshCtor({ positions, normals, uvs, indices }),
            x: 0, y: 0, z: 0,
            color: [1, 1, 1], metallic: 0.0, roughness: 1.0,
            twoSided: true,
            castsShadow: false, receivesShadow: false,
            texture: { width: impostor.width, height: impostor.height, data },
        });

        node.setShader({
            vertex: VERTEX_CHUNK,
            fragment: FRAGMENT_CHUNK,
            uniforms: {
                u_grid: [impostor.cols, impostor.rows],
                u_half: half,
                u_cull: [cullNear, cullFar],
            },
        });
        if (node.setCullMargin) node.setCullMargin(half * 2.0);

        return {
            node,
            quadCount: count,
            setCull(near, far) { node.setShaderUniform('u_cull', [near, far]); },
        };
    }

    const ns_impostor = mount(bro, "impostor");
    fn(ns_impostor, "createLayer", createLayer);
})();
