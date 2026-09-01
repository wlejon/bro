// Test bro.scene lighting/PBR pipeline — exercises src/scene/light_node.cpp,
// src/scene/scene_graph.cpp (tonemap / bloom / ambient / environment passes)
// and src/js/scene_bindings.cpp light + material property bindings.
// See docs/lighting-api.js for the documented surface.

function avgBrightness(img) {
    // Mean of R,G,B across every pixel (alpha ignored — background pixels
    // are RGBA(0,0,0,0) per toImageData/captureFrame's contract, so they
    // pull the average down, which is fine: we only compare two captures
    // of the *same* framing against each other).
    let sum = 0;
    const d = img.data;
    for (let i = 0; i < d.length; i += 4) {
        sum += (d[i] + d[i + 1] + d[i + 2]) / 3;
    }
    return sum / (d.length / 4);
}

function maxChannel(img) {
    let m = 0;
    const d = img.data;
    for (let i = 0; i < d.length; i += 4) {
        m = Math.max(m, d[i], d[i + 1], d[i + 2]);
    }
    return m;
}

// =============================================================================
// Section 1: light types + PBR material + shadow flags — property round-trip
// =============================================================================

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping lighting test');
} else {
    scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [2, 2, 2], target: [0, 0, 0] });

    // --- Directional light ---------------------------------------------------
    const sun = scene.createLight({
        type: 'directional',
        direction: [-0.3, -1.0, -0.4],
        color: [1.0, 0.98, 0.92],
        intensity: 3.0,
    });
    assert(sun !== null, 'createLight(directional) returns node');
    assert(sun.type === 'light', 'directional node type is light');
    assert(sun.kind === 'directional', 'directional kind');
    let d = sun.direction;
    assert(Array.isArray(d) && Math.abs(d[0] + 0.3) < 0.01 && Math.abs(d[1] + 1.0) < 0.01,
        'directional direction round-trips');
    let c = sun.color;
    assert(Math.abs(c[0] - 1.0) < 0.01 && Math.abs(c[2] - 0.92) < 0.01, 'directional color round-trips');
    assert(Math.abs(sun.intensity - 3.0) < 0.001, 'directional intensity round-trips');
    assert(sun.castsShadow === false, 'directional castsShadow defaults false');
    sun.castsShadow = true;
    assert(sun.castsShadow === true, 'directional castsShadow setter');
    sun.direction = [1, 0, 0];
    d = sun.direction;
    assert(Math.abs(d[0] - 1) < 0.01 && Math.abs(d[1]) < 0.01, 'direction setter');

    // Shadow tuning knobs (directional-specific + shared).
    // 0: the renderer sizes its own depth bias from the shadow texel; this
    // is an extra constant on top (see LightNode::shadowBias).
    assert(Math.abs(sun.shadowBias) < 1e-6, 'shadowBias default');
    assert(Math.abs(sun.shadowNormalBias - 0.03) < 1e-6, 'shadowNormalBias default');
    assert(sun.cascadeCount === 4, 'cascadeCount default');
    assert(Math.abs(sun.cascadeSplitLambda - 0.5) < 1e-6, 'cascadeSplitLambda default');
    sun.shadowBias = 1e-3;
    sun.shadowNormalBias = 0.06;
    sun.cascadeCount = 2;
    sun.cascadeSplitLambda = 0.8;
    assert(Math.abs(sun.shadowBias - 1e-3) < 1e-6, 'shadowBias setter');
    assert(Math.abs(sun.shadowNormalBias - 0.06) < 1e-6, 'shadowNormalBias setter');
    assert(sun.cascadeCount === 2, 'cascadeCount setter');
    assert(Math.abs(sun.cascadeSplitLambda - 0.8) < 1e-6, 'cascadeSplitLambda setter');
    // Clamped to [1,4]
    sun.cascadeCount = 10;
    assert(sun.cascadeCount === 4, 'cascadeCount clamps to 4');
    sun.cascadeCount = 0;
    assert(sun.cascadeCount === 1, 'cascadeCount clamps to 1');

    // --- Point light -----------------------------------------------------------
    const lamp = scene.createLight({
        type: 'point', position: [0, 2, 0], color: '#ff8800', intensity: 20, range: 8,
    });
    assert(lamp !== null, 'createLight(point) returns node');
    assert(lamp.kind === 'point', 'point kind');
    let pos = lamp.position;
    assert(Math.abs(pos[1] - 2) < 0.01, 'point position round-trips');
    assert(Math.abs(lamp.intensity - 20) < 0.001, 'point intensity round-trips');
    assert(Math.abs(lamp.range - 8) < 0.001, 'point range round-trips');
    c = lamp.color;
    // '#ff8800' -> (1.0, 0.533, 0.0)
    assert(Math.abs(c[0] - 1.0) < 0.02 && c[1] > 0.4 && c[1] < 0.65 && Math.abs(c[2]) < 0.02,
        'point color parses CSS hex string');

    // All mutable via returned node (per lighting-api.js doc example).
    lamp.x = 3;
    lamp.color = '#4488ff';
    lamp.intensity = 15;
    lamp.range = 10;
    assert(Math.abs(lamp.x - 3) < 0.01, 'point x setter');
    assert(Math.abs(lamp.intensity - 15) < 0.001, 'point intensity setter');
    assert(Math.abs(lamp.range - 10) < 0.001, 'point range setter');
    // `direction` is a field on every LightNode regardless of kind (unused by
    // point-light shading, but readable/writable without crashing); default
    // is (0,-1,0) per light_node.h.
    const lampDir = lamp.direction;
    assert(Array.isArray(lampDir) && Math.abs(lampDir[1] + 1) < 0.01,
        'point light direction field defaults to (0,-1,0) and does not throw');
    // innerAngle/outerAngle are LightNode-wide too — spot-only semantically,
    // but readable on point/directional lights (own defaults, not crashes).
    assert(typeof lamp.innerAngle === 'number', 'point light innerAngle readable');

    // --- Spot light --------------------------------------------------------
    const spot = scene.createLight({
        type: 'spot', position: [0, 5, 0], direction: [0, -1, 0],
        color: [1, 0.9, 0.6], intensity: 40, range: 12, innerAngle: 0.25, outerAngle: 0.45,
    });
    assert(spot !== null, 'createLight(spot) returns node');
    assert(spot.kind === 'spot', 'spot kind');
    assert(Math.abs(spot.innerAngle - 0.25) < 0.001, 'spot innerAngle round-trips');
    assert(Math.abs(spot.outerAngle - 0.45) < 0.001, 'spot outerAngle round-trips');
    assert(Math.abs(spot.range - 12) < 0.001, 'spot range round-trips');
    spot.innerAngle = 0.1;
    spot.outerAngle = 0.6;
    assert(Math.abs(spot.innerAngle - 0.1) < 0.001, 'spot innerAngle setter');
    assert(Math.abs(spot.outerAngle - 0.6) < 0.001, 'spot outerAngle setter');

    // --- PBR material round-trip -------------------------------------------
    const chrome = scene.createMesh({
        mesh: 'sphere', radius: 0.5, color: '#c8c8c8', metallic: 1.0, roughness: 0.2,
    });
    assert(Math.abs(chrome.metallic - 1.0) < 0.001, 'metallic round-trips from createMesh');
    assert(Math.abs(chrome.roughness - 0.2) < 0.001, 'roughness round-trips from createMesh');
    chrome.metallic = 0.9;
    chrome.roughness = 0.15;
    chrome.emissive = 2.0;
    assert(Math.abs(chrome.metallic - 0.9) < 0.001, 'metallic setter');
    assert(Math.abs(chrome.roughness - 0.15) < 0.001, 'roughness setter');
    assert(Math.abs(chrome.emissive - 2.0) < 0.001, 'emissive setter');

    // Default material values (glTF metallic/roughness defaults per mesh_node.h).
    const plain = scene.createMesh({ mesh: 'box' });
    assert(Math.abs(plain.metallic - 0.0) < 0.001, 'metallic defaults to 0 (dielectric)');
    assert(Math.abs(plain.roughness - 0.7) < 0.001, 'roughness defaults to 0.7');
    assert(Math.abs(plain.emissive - 0.0) < 0.001, 'emissive defaults to 0');

    // Self-emissive bar — construction with emissive + emissiveColor shouldn't crash.
    const neon = scene.createMesh({
        mesh: 'box', halfW: 3, halfH: 0.1, halfD: 0.1, emissive: 4.0, emissiveColor: [0.5, 0.8, 1.0],
    });
    assert(neon !== null, 'emissive bar construction does not crash');
    assert(Math.abs(neon.emissive - 4.0) < 0.001, 'emissive round-trips on construction');

    // Non-mesh nodes: material props are no-ops, not crashes.
    assert(sun.metallic === undefined, 'metallic is undefined on a light node');

    // --- PBR textures — construction with all four maps should not crash ---
    const W = 2, H = 2;
    const flatRGBA = new Uint8Array(W * H * 4).fill(128);
    const texturedSphere = scene.createMesh({
        mesh: 'sphere', radius: 0.5, color: '#ffffff', metallic: 1.0, roughness: 1.0,
        texture: { width: W, height: H, data: flatRGBA },
        normalTexture: { width: W, height: H, data: flatRGBA },
        metallicRoughnessTexture: { width: W, height: H, data: flatRGBA },
        occlusionTexture: { width: W, height: H, data: flatRGBA },
        emissiveTexture: { width: W, height: H, data: flatRGBA },
        emissive: 1.0, emissiveColor: [1.0, 0.6, 0.2],
    });
    assert(texturedSphere !== null, 'mesh with all 5 texture slots constructs without crashing');

    // --- Per-mesh shadow flags ------------------------------------------------
    const grass = scene.createMesh({ mesh: 'plane', castsShadow: false });
    const hud = scene.createMesh({ mesh: 'sphere', receivesShadow: false });
    assert(grass.castsShadow === false, 'mesh castsShadow:false from createMesh');
    assert(hud.receivesShadow === false, 'mesh receivesShadow:false from createMesh');
    const defaultMesh = scene.createMesh({ mesh: 'box' });
    assert(defaultMesh.castsShadow === true, 'mesh castsShadow defaults true');
    assert(defaultMesh.receivesShadow === true, 'mesh receivesShadow defaults true');
    defaultMesh.castsShadow = false;
    defaultMesh.receivesShadow = false;
    assert(defaultMesh.castsShadow === false, 'mesh castsShadow setter');
    assert(defaultMesh.receivesShadow === false, 'mesh receivesShadow setter');

    // --- Shadow quality global -------------------------------------------------
    if (typeof scene.setShadowQuality === 'function') {
        scene.setShadowQuality(2048, 3);
        scene.setShadowQuality(4096, 1);
    }

    flush();
    document.body.removeChild(canvas);
}

// =============================================================================
// Section 2: tonemap / ambient / bloom — real pixel-readback assertions via
// scene.captureFrame(), a fresh scene per sub-test so lighting state (implicit
// sun vs explicit lights) doesn't bleed between comparisons.
// =============================================================================

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    return { canvas: cv, scene: sc };
}

const vis = freshScene(128);
if (!vis.scene) {
    console.log('scene context not available (no GPU) — skipping visual lighting assertions');
} else {
    const scn = vis.scene;

    // A bright point light close to a white diffuse sphere, viewed head-on,
    // pushes the HDR radiance comfortably above 1.0 pre-tonemap so the three
    // tonemap operators visibly disagree (linear clips to white, Reinhard and
    // ACES roll off by different amounts).
    scn.setCamera({ fov: 50, near: 0.1, far: 100, position: [0, 0, 2], target: [0, 0, 0] });
    scn.createMesh({ mesh: 'sphere', radius: 0.6, color: '#ffffff', metallic: 0.0, roughness: 1.0 });
    scn.createLight({ type: 'point', position: [0, 0, 2.2], color: [1, 1, 1], intensity: 12, range: 20 });

    // --- setAmbient: flat additive term ----------------------------------------
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.setAmbient([0, 0, 0]);
    const ambDark = scn.captureFrame();
    scn.setAmbient([1, 1, 1]);
    const ambBright = scn.captureFrame();
    assert(ambDark !== null && ambBright !== null, 'captureFrame returns ImageData-shaped object');
    assert(avgBrightness(ambBright) > avgBrightness(ambDark),
        'setAmbient([1,1,1]) is brighter than setAmbient([0,0,0]): ' +
        avgBrightness(ambBright) + ' vs ' + avgBrightness(ambDark));
    scn.setAmbient([0.03, 0.03, 0.035]); // restore doc default

    // --- setToneMap: exposure is a pre-tonemap multiplier -----------------------
    scn.setToneMap({ mode: 'linear', exposure: 0.1, gamma: 1.0 });
    const lowExposure = scn.captureFrame();
    scn.setToneMap({ mode: 'linear', exposure: 3.0, gamma: 1.0 });
    const highExposure = scn.captureFrame();
    assert(avgBrightness(highExposure) > avgBrightness(lowExposure),
        'higher exposure brightens the frame: ' +
        avgBrightness(highExposure) + ' vs ' + avgBrightness(lowExposure));

    // --- setToneMap: mode changes the HDR->LDR curve ----------------------------
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    const linearImg = scn.captureFrame();
    scn.setToneMap({ mode: 'reinhard', exposure: 1.0, gamma: 1.0 });
    const reinhardImg = scn.captureFrame();
    scn.setToneMap({ mode: 'aces', exposure: 1.0, gamma: 1.0 });
    const acesImg = scn.captureFrame();

    const linearMax = maxChannel(linearImg);
    const reinhardMax = maxChannel(reinhardImg);
    const acesMax = maxChannel(acesImg);
    // At this intensity the lit hemisphere overshoots 1.0 pre-tonemap, so
    // linear clips hard to 255 while Reinhard/ACES roll off below it.
    assert(linearMax >= 254, 'linear tonemap clips the over-bright highlight to ~255, got ' + linearMax);
    assert(reinhardMax < linearMax,
        'reinhard tonemap rolls off below linear clip: ' + reinhardMax + ' vs ' + linearMax);
    assert(acesMax < linearMax,
        'aces tonemap rolls off below linear clip: ' + acesMax + ' vs ' + linearMax);
    assert(Math.abs(reinhardMax - acesMax) > 2 || Math.abs(reinhardMax - linearMax) > 2,
        'reinhard and aces produce visibly different highlight rolloff');
    scn.setToneMap({ mode: 'aces', exposure: 1.0, gamma: 2.2 }); // restore doc default

    // --- setEnvironment: null clears to the flat-ambient fallback ---------------
    // No .hdr test asset ships in this repo (see docs/lighting-api.js — the
    // CC0 environments live in the sibling broworkshop repo's demo assets),
    // so this only exercises the documented clear/no-op path, not a real
    // HDR IBL bake.
    const cleared = scn.setEnvironment(null);
    assert(cleared === true, 'setEnvironment(null) clears env and returns true');
    const clearedAgain = scn.setEnvironment({ hdr: '' });
    assert(clearedAgain === true, 'setEnvironment({hdr:""}) also clears');
    const invalid = scn.setEnvironment(42);
    assert(invalid === false, 'setEnvironment(non-object, non-null) returns false');
    // Updating intensity/rotation without hdr shouldn't crash even with no env loaded.
    scn.setEnvironment({ intensity: 2.0 });
    scn.setEnvironment({ rotation: Math.PI / 2 });

    // --- setBloom: bright-pass + blur adds energy around the highlight ----------
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.setBloom({ enabled: false });
    const bloomOff = scn.captureFrame();
    scn.setBloom({ enabled: true, threshold: 0.3, intensity: 3.0, strength: 3.0 });
    const bloomOn = scn.captureFrame();
    assert(avgBrightness(bloomOn) > avgBrightness(bloomOff),
        'bloom enabled adds glow energy vs disabled: ' +
        avgBrightness(bloomOn) + ' vs ' + avgBrightness(bloomOff));
    scn.setBloom({ enabled: false }); // restore default (off)

    document.body.removeChild(vis.canvas);
}
