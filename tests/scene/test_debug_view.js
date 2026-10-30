// scene.setDebugView(name): the frame is what the custom chunks answering
// SCENE_DEBUG(name) put out with sceneDebugOut(rgb), unlit at an exposure of
// 1 and a linear tone map, over black (no sky):
//   - an answering surface shows its colour as is, whatever the light and
//     the auto exposure would make of it, and the same at another view's;
//   - a surface that does not answer is a dim grey (0.06 to 0.26, shown
//     through the linear tone map's display encoding);
//   - an answering volume covers what is behind it with its colour; one
//     that does not answer lets all through;
//   - debugViews() names the views the chunks named and the one shown;
//   - no view: the frame as it was.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) sc.setCamera({ fov: 50, near: 0.1, far: 100, position: [0, 2.2, 3.2], target: [0, 0, 0] });
    return sc;
}

const px = (img, x, y) => {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
};

const S = 64;
const scene = freshScene(S);
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setAtmosphere({});
    scene.setExposure({ mode: 'auto', meter: 'incident' });
    scene.createLight({ type: 'directional', direction: [-0.3, -0.8, -0.5], color: [1, 1, 1], intensity: 100000 });
    // A plane answering 'probe' with a quarter red, its left half only.
    const plane = scene.createMesh({
        positions: new Float32Array([-2, 0, -2, 2, 0, -2, -2, 0, 2, 2, 0, 2]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
        indices: new Uint32Array([0, 2, 1, 1, 2, 3]),
        color: [0.5, 0.5, 0.5],
    });
    plane.setShader({
        fragment: `void userFragment(inout vec3 baseColor, inout vec3 normal, inout float metallic,
                                     inout float roughness, inout vec3 emissive, inout float alpha) {
                if (SCENE_DEBUG(probe) && gl_FragCoord.x < 32.0) sceneDebugOut(vec3(0.25, 0.0, 0.0));
            }`,
    });
    const normal = scene.captureFrame();
    const sky = px(normal, S / 2, 2);
    assert(sky[2] > 40, `the sky shows without a view (${sky})`);

    scene.setDebugView('probe');
    let img = scene.captureFrame();
    for (let i = 0; i < 4; i++) img = scene.captureFrame();
    const red = px(img, 20, 44), grey = px(img, 44, 44);
    assert(red[0] > 60 && red[1] < 4 && red[2] < 4, `the answering half shows its colour (${red})`);
    assert(Math.abs(grey[0] - grey[1]) < 3 && Math.abs(grey[1] - grey[2]) < 3 && grey[0] > 30 && grey[0] < 150,
           `the half that does not answer is a dim grey (${grey})`);
    assert(px(img, S / 2, 2).every((v) => v < 3), `no sky behind a view (${px(img, S / 2, 2)})`);
    const views = scene.debugViews();
    assert(views.shown === 'probe' && views.views.includes('probe'), `debugViews names it (${JSON.stringify(views)})`);

    // The light's strength does not reach a debug view's colour.
    scene.createLight({ type: 'directional', direction: [0.3, -0.8, 0.5], color: [1, 1, 1], intensity: 400000 });
    img = scene.captureFrame();
    for (let i = 0; i < 4; i++) img = scene.captureFrame();
    assert(Math.abs(px(img, 20, 44)[0] - red[0]) <= 1, `a view's colour is the chunk's, not the light's (${px(img, 20, 44)}, ${red})`);

    // A volume answering 'probe' over the top rows, and lets all through
    // under another view.
    const vol = scene.createVolume({
        fragment: `void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
                                   inout float transmittance, inout float depth) {
                transmittance = 0.2;
                radiance = vec3(1.0);
                if (SCENE_DEBUG(probe) && gl_FragCoord.y < 16.0) sceneDebugOut(vec3(0.0, 0.0, 0.25));
            }`,
    });
    for (let i = 0; i < 12; i++) img = scene.captureFrame();
    const blue = px(img, 20, 8);
    assert(blue[2] > 60 && blue[0] < 4 && blue[1] < 4, `an answering volume covers with its colour (${blue})`);
    assert(Math.abs(px(img, 20, 44)[0] - red[0]) <= 2, `one that does not answer lets all through (${px(img, 20, 44)})`);

    scene.setDebugView('other');
    for (let i = 0; i < 12; i++) img = scene.captureFrame();
    const g2 = px(img, 20, 44);
    assert(Math.abs(g2[0] - g2[2]) < 3 && g2[0] < 150, `under a view nothing answers, all is grey (${g2})`);

    scene.destroyNode(vol);
    scene.setDebugView(null);
    for (let i = 0; i < 6; i++) img = scene.captureFrame();
    assert(scene.debugViews().shown === null, 'setDebugView(null) shows none');
    assert(px(img, S / 2, 2)[2] > 40, `the sky is back (${px(img, S / 2, 2)})`);
}
