// bro.impostor — camera-facing octahedral impostor billboards rendered as ONE
// merged, non-instanced draw. Promotes the app-side impostor layer into the
// engine using the fast path: instead of N instances (which pay a fixed
// ~7-9us/instance GPU cost on some drivers — see scene/instanced_mesh_node.h),
// bake every billboard's centre into one merged mesh (4 verts each, corner sign
// in UV, per-billboard scale in normal.x) and camera-face each quad in the
// vertex shader. One ordinary draw of N*2 triangles runs at merged-mesh speed
// (~2000 quads in ~0.02ms vs ~10ms instanced).
//
// The atlas bake (octahedral capture into an RGBA grid) stays app-side; this
// module only RENDERS a bake result. The octahedral inverse + within-cell
// sampling here mirror the atlas convention documented in the app's impostor
// baker exactly, so a bake feeds this renderer unchanged.
//
// Billboarding with no per-instance data: the mesh shader runs in
// CAMERA-RELATIVE space (uModel has the eye pre-subtracted, so the eye is the
// origin), so uModel * centre gives the eye-relative centre and the view basis
// follows from it — no uCameraEye needed.
(function () {
    'use strict';
    const broNs = globalThis.bro;
    if (!broNs) return;

    const CORNERS = [[-1, -1], [1, -1], [1, 1], [-1, 1]];

    // pos.xyz = billboard world centre; uv = corner sign in [-1,1]^2;
    // normal.x = per-billboard scale. u_half is the half-extent at scale 1.
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

    // Camera-relative centre: uModel has the eye pre-subtracted (the mesh path
    // is camera-relative), so the eye sits at the origin here.
    vec3 centerCR = (uModel * vec4(pos, 1.0)).xyz;
    float camDist = length(centerCR);
    v_fade = 1.0 - smoothstep(u_cull.x, u_cull.y, camDist);

    // SPHERICAL (fully view-facing) billboard basis: f = eye->centre. right is
    // horizontal (perp to world-up and f); up completes the frame and tilts with
    // pitch, so steep top-down views present the top-down atlas cell as a proper
    // crown instead of foreshortening to a streak.
    vec3 f = camDist > 1e-4 ? centerCR / camDist : vec3(0.0, 0.0, 1.0);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), f);
    float rl = length(right);
    right = rl > 1e-4 ? right / rl : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(f, right);

    // Node is identity, so the world-oriented offset is also the object offset.
    vec3 offset = right * (corner.x * u_half * scl) + up * (corner.y * u_half * scl);
    pos = pos + offset;
    normal = -f;                          // shading normal faces the camera

    // Octahedral INVERSE with dir = centre->eye (= -f). Picks one atlas cell for
    // the whole quad (flat varyings). Mirrors the baker's octahedral mapping.
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

    uv = corner * 0.5 + 0.5;              // within-cell uv in [0,1]

    // Beyond the cull radius, collapse the quad to its pivot: a degenerate
    // triangle rasterises to zero fragments, so culled billboards cost nothing.
    if (v_fade <= 0.001) pos = pos - offset;
}
`;

    // Sample the resolved atlas cell; cut out the transparent bake background.
    // The atlas is baked UNLIT, so emit its albedo (baseColor 0 -> emissive) and
    // let the scene's single ACES tonemap map it once. Within-cell v is flipped
    // so the billboard top maps to the tree top (atlas data is top-down).
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
    if (tex.a < 0.5) discard;   // alpha cutout against the transparent bake

    if (v_fade < 0.999) {       // distance dissolve into the terrain forest tint
        float hash = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        if (hash > v_fade) discard;
    }
    baseColor = vec3(0.0);
    emissive = tex.rgb;
    alpha = 1.0;
}
`;

    /**
     * Render `transforms` as ONE merged batch of octahedral impostor billboards
     * sampling `impostor` (an atlas bake result). Drop-in replacement for an
     * app-side createImpostorLayer, but one draw instead of N instances.
     *
     * @param {SceneGraph} scene  a canvas 'scene' context
     * @param {Object} impostor   { atlasRGBA, width, height, cols, rows,
     *                              bounds:{center,radius} }
     * @param {Float32Array|number[]} transforms  9 floats/instance
     *   (px,py,pz, qx,qy,qz,qw, scale, variantIndex). Rotation is ignored —
     *   billboards face the camera.
     * @param {Object} [opts]  { margin=1.03, cullNear=450, cullFar=950 }
     * @returns {{ node, quadCount, setCull:(near,far)=>void }}
     */
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

        // Merged geometry: 4 verts per billboard, all at the (scaled) world
        // centre so the vertex shader can rebuild the camera-facing quad.
        const positions = new Float32Array(count * 4 * 3);
        const normals = new Float32Array(count * 4 * 3);
        const uvs = new Float32Array(count * 4 * 2);
        const indices = new Uint32Array(count * 6);
        for (let i = 0; i < count; i++) {
            const o = i * 9;
            const scl = src[o + 7];
            // World centre = position + scale*boundsCentre (a taller billboard's
            // crown centre rises with it), matching the instanced path's
            // R*u_center + translation placement.
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

        const node = scene.createMesh({
            mesh: new Mesh({ positions, normals, uvs, indices }),
            x: 0, y: 0, z: 0,
            color: [1, 1, 1], metallic: 0.0, roughness: 1.0,
            twoSided: true,             // billboard winding varies with view
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
        // A billboard's drawn extent reaches u_half*scale beyond its baked
        // centre, but the mesh bounds are just the centre points — pad culling
        // so a batch near the frustum edge isn't wrongly culled.
        if (node.setCullMargin) node.setCullMargin(half * 2.0);

        return {
            node,
            quadCount: count,
            setCull(near, far) { node.setShaderUniform('u_cull', [near, far]); },
        };
    }

    broNs.impostor = { createLayer };
})();
