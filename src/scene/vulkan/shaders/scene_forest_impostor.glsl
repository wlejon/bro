// Forest impostors (scene/forest_impostor.h): one card a tree showing its
// specimen's baked picture, the leaves tier's own colour, normal, depth and
// coverage from N x N directions over the upper hemisphere
// (hemi-octahedral). Shared by mesh_forest_impostor.vert, mesh.frag's
// SCENE_FOREST_IMPOSTOR build and the shadow casters; needs nothing else.
//
// A fragment traces its eye ray through the card into specimen space and
// reads the three baked views round the direction it is seen from, each at
// the point where the ray crosses that view's image plane, blended by the
// view's weight and coverage. The depth each texel carries puts the surface
// back where the leaf was (the ray's point at that depth), so the lighting,
// the shadow lookup and the depth buffer see the crown's real surface
// rather than the card.

// A view's direction (toward the viewer, unit, y >= 0) from cell (i, j) of
// an n x n grid: forest_impostor.cpp's forestImpostorViewDir.
vec3 forestImpostorDir(ivec2 ij, int n) {
    vec2 e = (vec2(ij) + 0.5) / float(n) * 2.0 - 1.0;
    float px = 0.5 * (e.x + e.y), pz = 0.5 * (e.x - e.y);
    return normalize(vec3(px, max(1.0 - abs(px) - abs(pz), 0.0), pz));
}

// A view's image axes (forestImpostorBasis).
void forestImpostorBasis(vec3 f, out vec3 right, out vec3 up) {
    vec3 r = cross(vec3(0.0, 1.0, 0.0), f);
    right = dot(r, r) < 1e-10 ? vec3(1.0, 0.0, 0.0) : normalize(r);
    up = cross(f, right);
}

// The three views round direction `d` (specimen space, toward the viewer;
// below the horizon reads as on it), as cell indices i + j * n, and their
// weights (summing to 1): the corners of the grid triangle `d` falls in.
void forestImpostorViews(vec3 d, int n, out ivec3 cells, out vec3 w) {
    d.y = max(d.y, 0.0);
    d /= max(abs(d.x) + d.y + abs(d.z), 1e-6);
    vec2 g = (vec2(d.x + d.z, d.x - d.z) * 0.5 + 0.5) * float(n) - 0.5;
    vec2 b = floor(g);
    vec2 f = g - b;
    ivec2 c0, c1, c2;
    ivec2 ib = ivec2(b);
    if (f.x + f.y < 1.0) {
        c0 = ib; c1 = ib + ivec2(1, 0); c2 = ib + ivec2(0, 1);
        w = vec3(1.0 - f.x - f.y, f.x, f.y);
    } else {
        c0 = ib + ivec2(1, 1); c1 = ib + ivec2(0, 1); c2 = ib + ivec2(1, 0);
        w = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
    }
    c0 = clamp(c0, ivec2(0), ivec2(n - 1));
    c1 = clamp(c1, ivec2(0), ivec2(n - 1));
    c2 = clamp(c2, ivec2(0), ivec2(n - 1));
    cells = ivec3(c0.x + c0.y * n, c1.x + c1.y * n, c2.x + c2.y * n);
}

// What the trace found: the blended albedo (linear), coverage, normal
// (specimen space), leafiness, and the ray parameter of the surface.
struct ForestImpostorHit {
    vec3 albedo;
    float coverage;
    vec3 normal;
    float leafy;
    float t;
};

// The layout of the atlas: views a side, texels a view, and the first cell
// of the specimen's block (in cells).
struct ForestImpostorAtlas {
    int views;
    int texels;
    ivec2 block;
};

#ifndef FOREST_IMPOSTOR_VERTEX
// Trace the ray o + t d (specimen space; d need not be unit) through the
// card of a specimen whose picture is centred on `center` with half size
// `extent`, reading views `cells` at weights `w` from `colorTex` / `dataTex`.
// `shadow` reads only what a caster needs (coverage and depth). Fragment
// stages only (the vertex stages define FOREST_IMPOSTOR_VERTEX).
ForestImpostorHit forestImpostorTrace(sampler2D colorTex, sampler2D dataTex, ForestImpostorAtlas atlas,
                                      ivec3 cells, vec3 w, vec3 center, float extent, vec3 o, vec3 d, bool shadow) {
    ForestImpostorHit h;
    h.albedo = vec3(0.0);
    h.coverage = 0.0;
    h.normal = vec3(0.0);
    h.leafy = 0.0;
    h.t = 0.0;
    vec2 atlasSize = vec2(textureSize(colorTex, 0));
    float R = float(atlas.texels);
    float inset = 0.5 / R;
    float tSum = 0.0;
    for (int k = 0; k < 3; ++k) {
        if (w[k] <= 0.0) continue;   // a view left out (the casters read one)
        int cell = cells[k];
        ivec2 ij = ivec2(cell % atlas.views, cell / atlas.views);
        vec3 f = forestImpostorDir(ij, atlas.views);
        vec3 right, up;
        forestImpostorBasis(f, right, up);
        float dn = dot(d, f);
        float t = abs(dn) > 1e-8 ? dot(center - o, f) / dn : 0.0;
        vec3 q = o + d * t - center;
        vec2 uv = vec2(dot(q, right), -dot(q, up)) / extent * 0.5 + 0.5;
        bool inside = all(greaterThan(uv, vec2(inset))) && all(lessThan(uv, vec2(1.0 - inset)));
        vec2 st = (vec2(atlas.block + ij) * R + clamp(uv, vec2(inset), vec2(1.0 - inset)) * R) / atlasSize;
        // A caster reads a level coarser: a shadow's edge is softened by
        // its filter anyway, and the reads are its cost.
        vec4 c = shadow ? texture(colorTex, st, 1.0) : texture(colorTex, st);
        vec4 dat = shadow ? texture(dataTex, st, 1.0) : texture(dataTex, st);
        float a = inside ? c.a * w[k] : 0.0;
        // The surface: back along the ray to the texel's depth toward the view.
        float depth = (dat.z * 2.0 - 1.0) * extent;
        float ts = abs(dn) > 1e-8 ? t + depth / dn : t;
        h.coverage += a;
        tSum += a * ts;
        if (!shadow) {
            vec2 nxy = dat.xy * 2.0 - 1.0;
            vec3 n = right * nxy.x + up * nxy.y + f * sqrt(max(1.0 - dot(nxy, nxy), 0.0));
            h.albedo += a * c.rgb;
            h.normal += a * n;
            h.leafy += a * dat.w;
        }
    }
    if (h.coverage > 1e-5) {
        float inv = 1.0 / h.coverage;
        h.t = tSum * inv;
        h.albedo *= inv;
        h.leafy *= inv;
        h.normal = dot(h.normal, h.normal) > 1e-12 ? normalize(h.normal) : vec3(0.0, 1.0, 0.0);
    }
    return h;
}
#endif

// The pose's offset at a point `h` of the way up a tree (the impostor's
// share of vegPose): the trunk's bend there (with its top dropping a little
// as it leans), and the crown's mean limb bend, in full from the crown's
// middle up. `xt` the trunk tip's deflection, `xl` the limb's at the crown,
// `height` the tree's (world units).
vec3 forestImpostorOffset(vec3 xt, vec3 xl, float height, float h) {
    h = clamp(h, 0.0, 1.3);
    float s = h <= 1.0 ? h * h * (3.0 - h) * 0.5 : 1.0 + 1.5 * (h - 1.0);
    vec3 o = xt * s - vec3(0.0, 0.6 * dot(xt, xt) * s / max(height, 0.05), 0.0);
    return o + xl * smoothstep(0.25, 0.6, h);
}
