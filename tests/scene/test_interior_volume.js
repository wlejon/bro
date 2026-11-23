// Verifies InteriorVolumeNode JS API, outdoor sky/sun attenuation, warm
// interior ambient bounce, priority stacking, and incident exposure coupling.

function centerPatch(img) {
    const cx = img.width >> 1;
    const cy = img.height >> 1;
    let r = 0, g = 0, b = 0, n = 0;
    for (let y = cy - 6; y <= cy + 6; y++) {
        for (let x = cx - 6; x <= cx + 6; x++) {
            const i = (y * img.width + x) * 4;
            r += img.data[i];
            g += img.data[i + 1];
            b += img.data[i + 2];
            n++;
        }
    }
    return { r: r / n, g: g / n, b: b / n, lum: (r + g + b) / (3 * n) };
}

const cv = document.createElement('canvas');
cv.setAttribute('width', '128');
cv.setAttribute('height', '128');
document.body.appendChild(cv);
flush();

const scn = cv.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    // 1. Property & accessor round-trips
    const v = scn.createInteriorVolume({
        size: [12, 6, 12],
        position: [0, 3, 0],
        skyVisibility: 0.04,
        sunVisibility: 0.25,
        ambientColor: [1.0, 0.85, 0.6],
        ambientIntensity: 0.5,
        blendDistance: 1.5,
        priority: 3,
        affectExposure: true,
    });
    assert(v !== null && v.type === 'interiorVolume', 'createInteriorVolume creates interiorVolume node');
    assert(Math.abs(v.skyVisibility - 0.04) < 1e-4, 'skyVisibility getter');
    assert(Math.abs(v.sunVisibility - 0.25) < 1e-4, 'sunVisibility getter');
    assert(Math.abs(v.ambientIntensity - 0.5) < 1e-4, 'ambientIntensity getter');
    assert(Math.abs(v.blendDistance - 1.5) < 1e-4, 'blendDistance getter');
    assert(v.priority === 3 && v.affectExposure === true, 'priority & affectExposure getters');
    v.ambientColor = '#ffcc88';
    assert(v.ambientColor[0] > 0.95 && v.ambientColor[2] < 0.6, 'ambientColor CSS setter');

    const probeMesh = scn.createMesh({ mesh: Mesh.box(2, 2, 2), position: [0, 1, 0], color: [1, 1, 1], roughness: 0.9, castsShadow: false });
    assert(probeMesh.skyVisibility === undefined, 'non-interiorVolume returns undefined for skyVisibility');

    // 2. Rendering: outdoor sky + sun attenuation & warm interior ambient bounce
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAmbient({ color: [0.55, 0.70, 1.0], intensity: 1.0 });
    scn.setCamera({ fov: 50, near: 0.1, far: 100, position: [0, 1.2, 4.2], target: [0, 1, 0] });
    const sun = scn.createLight({ type: 'directional', direction: [-0.2, -1, -0.3], color: [1, 0.98, 0.92], intensity: 2.0 });

    v.visible = false;
    scn.captureFrame();
    const outdoor = centerPatch(scn.captureFrame());

    v.visible = true;
    v.scale = [12, 8, 12];
    v.position = [0, 2, 0];
    v.skyVisibility = 0.0;
    v.sunVisibility = 0.0;
    v.ambientColor = [0, 0, 0];
    v.ambientIntensity = 0.0;
    v.blendDistance = 0.2;
    const enclosedDark = centerPatch(scn.captureFrame());
    assert(enclosedDark.lum < outdoor.lum * 0.25,
        `zero sky/sun interior volume darkens box (${enclosedDark.lum.toFixed(1)} vs outdoor ${outdoor.lum.toFixed(1)})`);

    v.ambientColor = [1.0, 0.45, 0.08];
    v.ambientIntensity = 0.9;
    const warmFill = centerPatch(scn.captureFrame());
    assert(warmFill.r > enclosedDark.r + 35 && warmFill.r > warmFill.b * 2.5,
        `interior ambient fill illuminates warmly (rgb=${warmFill.r.toFixed(0)},${warmFill.g.toFixed(0)},${warmFill.b.toFixed(0)})`);

    // Higher-priority nested volume overrides outer volume
    const inner = scn.createInteriorVolume({
        size: [6, 6, 6],
        position: [0, 1, 0],
        skyVisibility: 0.0,
        sunVisibility: 0.0,
        ambientColor: [0.1, 0.5, 1.0],
        ambientIntensity: 0.9,
        blendDistance: 0.1,
        priority: 10,
    });
    const innerBlue = centerPatch(scn.captureFrame());
    assert(innerBlue.b > innerBlue.r * 2.0,
        `higher-priority inner volume overrides outer fill (rgb=${innerBlue.r.toFixed(0)},${innerBlue.g.toFixed(0)},${innerBlue.b.toFixed(0)})`);
    inner.visible = false;

    // 3. Incident exposure coupling + blendDistance transition
    scn.setAtmosphere({});
    sun.intensity = 120000;
    scn.setExposure({ mode: 'auto', meter: 'incident' });

    // Place the wall well inside the volume (past blendDistance) so interior
    // surfaces see full enclosure attenuation rather than 97% outdoor sun.
    scn.createMesh({ mesh: Mesh.box(24, 12, 0.5), position: [0, 3, -5.0], color: [0.6, 0.6, 0.6], roughness: 0.9, castsShadow: true });

    v.position = [0, 3, 0];
    v.scale = [20, 10, 20];
    v.skyVisibility = 0.05;
    v.sunVisibility = 0.0;
    v.ambientColor = [1, 0.9, 0.75];
    v.ambientIntensity = 0.5;
    v.blendDistance = 2.0;
    v.affectExposure = true;

    const sampleEv = (zPos) => {
        scn.setCamera({ fov: 60, near: 0.1, far: 200, position: [0, 2, zPos], target: [0, 2, -4.5] });
        for (let i = 0; i < 6; i++) scn.captureFrame();
        return scn.cullStats().meteredEv;
    };

    const evOutdoor = sampleEv(20);
    const evEdge = sampleEv(9.2);   // 0.8m inside +Z face (< 2.0 blendDistance)
    const evIndoor = sampleEv(0);   // deep interior
    assert(evOutdoor > evEdge + 0.3 && evEdge > evIndoor + 0.5,
        `incident meteredEv drops smoothly from outdoors -> doorway -> indoor (${evOutdoor.toFixed(2)} -> ${evEdge.toFixed(2)} -> ${evIndoor.toFixed(2)})`);

    v.affectExposure = false;
    const evIgnored = sampleEv(0);
    assert(evIgnored > evIndoor + 1.5 && evIgnored <= evOutdoor + 0.05,
        `affectExposure=false stops interior enclosure from lowering incident meter (${evIgnored.toFixed(2)} vs indoor ${evIndoor.toFixed(2)}, outdoor ${evOutdoor.toFixed(2)})`);

    document.body.removeChild(cv);
}
