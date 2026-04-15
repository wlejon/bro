// Baking: AO, curvature, thickness — both vertex-space and texture-space.

const sphere = Mesh.sphere(1, 24, 18);

// Vertex baking: AO writes per-vertex colors
sphere.bakeAmbientOcclusion(16, 0);
assert(sphere.hasColors, 'bakeAO populates vertex colors');
assert(sphere.colors.length === sphere.vertexCount * 4, 'colors stride=4');

// Curvature
const s2 = Mesh.sphere(1, 16, 12);
s2.bakeCurvature(1.0);
assert(s2.hasColors, 'bakeCurvature populates colors');

// Thickness (a thin plate)
const s3 = Mesh.box(1, 0.1, 1);
s3.bakeThickness(8, 0);
assert(s3.hasColors, 'bakeThickness populates colors');

// Texture baking requires UVs. Use a plane (has UVs by default).
const plane = Mesh.plane(1, 1, 4, 4);
assert(plane.hasUVs, 'plane has UVs');

function checkTex(t, w, h, ch, name) {
    assert(t.width === w,    name + ' width matches');
    assert(t.height === h,   name + ' height matches');
    assert(t.channels === ch, name + ' channels=' + ch);
    assert(t.pixels instanceof Float32Array, name + ' pixels is Float32Array');
    assert(t.pixels.length === w * h * ch, name + ' pixel count matches w*h*c');
}

checkTex(plane.bakeAOToTexture(32, 32, 8, 0),        32, 32, 1, 'AO texture');
checkTex(plane.bakeCurvatureToTexture(32, 32, 1),    32, 32, 1, 'curvature texture');
checkTex(plane.bakeThicknessToTexture(32, 32, 8, 0), 32, 32, 1, 'thickness texture');
checkTex(plane.bakeNormalsToTexture(32, 32),         32, 32, 4, 'normal texture');
checkTex(plane.bakePositionToTexture(32, 32),        32, 32, 4, 'position texture');

// Transfer bakes: low-poly plane sampling high-poly plane as reference
const lo = Mesh.plane(1, 1, 2, 2);
const hi = Mesh.plane(1, 1, 8, 8);
const normTex = lo.bakeNormalsFromReference(hi, 32, 32, 0);
checkTex(normTex, 32, 32, 4, 'transfer normals');

const aoTex = lo.bakeAOFromReference(hi, 32, 32, 8, 0);
checkTex(aoTex, 32, 32, 1, 'transfer AO');
