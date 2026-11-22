// Tests bro.eye.structure multi-floor slab support (floors / autoDetectFloors / minFloorSeparation)
// and triangle-accurate interiorWalk checks (doorways, corridors, stairs, headroom).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    // 1. Multi-story building slabs: y = 0, y = 3.5, y = 7.0
    const f0 = scene.createMesh({ mesh: Mesh.box(12, 0.1, 8), color: '#444c56' });
    f0.name = 'Floor_Ground';
    f0.position = [0, -0.1, 0];

    const f1 = scene.createMesh({ mesh: Mesh.box(2.5, 0.1, 4.0), color: '#55606e' });
    f1.name = 'Floor1_Slab';
    f1.position = [0, 3.4, 0]; // top at 3.5, x in [-2.5, 2.5]

    const f2 = scene.createMesh({ mesh: Mesh.box(5.0, 0.1, 5.0), color: '#55606e' });
    f2.name = 'Floor2_Slab';
    f2.position = [0, 6.9, 0]; // top at 7.0

    // Hollow doorway wall on ground floor at z = 0: left jamb, right jamb, lintel
    const doorLeft = Mesh.box(0.9, 1.5, 0.15).translate(-1.5, 1.5, 0);
    const doorRight = Mesh.box(0.9, 1.5, 0.15).translate(1.5, 1.5, 0);
    const doorLintel = Mesh.box(0.6, 0.4, 0.15).translate(0, 2.6, 0);
    const doorWall = scene.createMesh({ mesh: Mesh.merge([doorLeft, doorRight, doorLintel]), color: '#9aa5b1' });
    doorWall.name = 'HollowDoorWall';

    // Valid chair on Floor 1
    const chair = scene.createMesh({ mesh: Mesh.box(0.3, 0.4, 0.3), color: '#c49a6c' });
    chair.name = 'Chair_Floor1';
    chair.position = [0, 3.9, -1.5];

    // Valid stair flight at x = 4.0, z from 0.15 to 2.85, y from 0.15 to 1.50
    const stairs = scene.createInstancedMesh({ mesh: Mesh.box(0.6, 0.08, 0.18), color: '#8a94a0' });
    stairs.name = 'LobbyStairs';
    const stairData = [];
    for (let k = 0; k < 10; k++) {
        const ty = 0.15 * (k + 1) - 0.08;
        const tz = 0.30 * k + 0.15;
        stairData.push(1, 0, 0, 4.0,  0, 1, 0, ty,  0, 0, 1, tz,  1, 1, 1, 1);
    }
    stairs.setInstances(new Float32Array(stairData));

    // Valid residential stair (rise=0.20, run=0.24, stepUp=0.30) at x = 6.5
    const resStairs = scene.createInstancedMesh({ mesh: Mesh.box(0.5, 0.11, 0.14), color: '#8a94a0' });
    resStairs.name = 'ResidentialStairs';
    const rsData = [];
    for (let k = 0; k < 6; k++) {
        const ty = 0.20 * (k + 1) - 0.11;
        const tz = 0.24 * k + 0.12;
        rsData.push(1, 0, 0, 6.5,  0, 1, 0, ty,  0, 0, 1, tz,  1, 1, 1, 1);
    }
    resStairs.setInstances(new Float32Array(rsData));
    flush();

    const cleanRes = bro.eye.structure(scene, {
        autoDetectFloors: true,
        minFloorSeparation: 2.0,
        interiorWalk: {
            enabled: true,
            agentRadius: 0.35,
            agentHeight: 1.85,
            stepUp: 0.30,
            minHeadroom: 1.95,
            doorways: [{ name: 'FrontDoor', position: [0, 0, 0], normal: [0, 0, 1], width: 1.2, height: 2.2, depth: 0.8 }],
            corridors: [{ name: 'LobbyHall', start: [-2.0, 0, -2.0], end: [2.0, 0, -2.0], width: 1.4, height: 2.4 }],
            stairs: [
                { name: 'LobbyStair', start: [4.0, 0.15, 0.15], end: [4.0, 1.50, 2.85], width: 1.2, headroom: 2.2 },
                { name: 'ResidentialStair', start: [6.5, 0.20, 0.12], end: [6.5, 1.20, 1.32], width: 1.0, headroom: 2.2 },
            ],
        },
    });
    assert(cleanRes.ok === true, 'clean multifloor + interiorWalk passes: ' + JSON.stringify(cleanRes.issues));
    assert(cleanRes.floors.length === 3, 'autoDetectFloors found 3 floors, got ' + cleanRes.floors.length);
    assert(cleanRes.floorDefectCount === 0, 'valid slab separation');
    assert(cleanRes.interiorWalk.checked === true, 'interiorWalk ran');
    assert(cleanRes.interiorWalk.doorwaysBlocked === 0, 'hollow doorway cutout not blocked');
    assert(cleanRes.interiorWalk.corridorsBlocked === 0, 'corridor clear');
    assert(cleanRes.interiorWalk.stairsBroken === 0, 'residential & lobby stairs walkable');

    // Add defects: floating sofa, wide overhanging table (2.0 x 0.3 x 1.6 m), blocking crate, steep/low-headroom/gap stairs
    const floatSofa = scene.createMesh({ mesh: Mesh.box(0.4, 0.3, 0.4), color: '#e06c75' });
    floatSofa.name = 'FloatingSofa_F1';
    floatSofa.position = [-1.2, 4.15, 1.0];

    const wideOverhangTable = scene.createMesh({ mesh: Mesh.box(1.0, 0.15, 0.8), color: '#d19a66' });
    wideOverhangTable.name = 'WideCutoutTable_F1';
    wideOverhangTable.position = [2.4, 3.65, 0.0]; // 2.0x0.3x1.6m resting at y=3.5, 45% off Floor1_Slab edge at x=2.5

    const doorCrate = scene.createMesh({ mesh: Mesh.box(0.3, 0.6, 0.3), color: '#be5046' });
    doorCrate.name = 'DoorCrate';
    doorCrate.position = [0, 0.6, 0];

    const steepStairs = scene.createInstancedMesh({ mesh: Mesh.box(0.5, 0.05, 0.45), color: '#998877' });
    steepStairs.name = 'SteepWideStairs';
    const swData = [];
    for (let k = 0; k < 3; k++) {
        swData.push(1, 0, 0, -5.0,  0, 1, 0, 0.55 * (k + 1) - 0.05,  0, 0, 1, 0.90 * k + 0.45,  1, 1, 1, 1);
    }
    steepStairs.setInstances(new Float32Array(swData));

    const headStairs = scene.createInstancedMesh({ mesh: Mesh.box(0.6, 0.08, 0.18), color: '#8899aa' });
    headStairs.name = 'HeadroomStairs';
    const hsData = [];
    for (let k = 0; k < 10; k++) {
        hsData.push(1, 0, 0, -7.5,  0, 1, 0, 0.15 * (k + 1) - 0.08,  0, 0, 1, 0.30 * k + 0.15,  1, 1, 1, 1);
    }
    headStairs.setInstances(new Float32Array(hsData));
    const stairBeam = scene.createMesh({ mesh: Mesh.box(0.6, 0.2, 0.3), color: '#aa5555' });
    stairBeam.name = 'StairBeam';
    stairBeam.position = [-7.5, 2.4, 1.5];

    const gapStairs = scene.createInstancedMesh({ mesh: Mesh.box(0.6, 0.08, 0.18), color: '#8899aa' });
    gapStairs.name = 'GapStairsLow';
    const gsData = [];
    for (let k = 0; k < 10; k++) {
        if (k === 2) continue; // missing low tread at y=0.45m over Floor_Ground y=0
        gsData.push(1, 0, 0, -10.0,  0, 1, 0, 0.15 * (k + 1) - 0.08,  0, 0, 1, 0.30 * k + 0.15,  1, 1, 1, 1);
    }
    gapStairs.setInstances(new Float32Array(gsData));
    flush();

    const walkOpts = {
        enabled: true,
        stepUp: 0.40,
        doorways: [{ name: 'FrontDoor', position: [0, 0, 0], normal: [0, 0, 1], width: 1.2, height: 2.2, depth: 0.8 }],
        stairs: [
            { name: 'SteepWideStair', start: [-5.0, 0.55, 0.45], end: [-5.0, 1.65, 2.25], width: 1.0, headroom: 2.2 },
            { name: 'HeadroomStair', start: [-7.5, 0.15, 0.15], end: [-7.5, 1.50, 2.85], width: 1.2, headroom: 2.2 },
            { name: 'GapStairLow', start: [-10.0, 0.15, 0.15], end: [-10.0, 1.50, 2.85], width: 1.2, headroom: 2.2 },
        ],
    };

    const autoDefectRes = bro.eye.structure(scene, {
        autoDetectFloors: true,
        minFloorSeparation: 2.0,
        interiorWalk: walkOpts,
    });
    assert(autoDefectRes.floors.length === 3 && autoDefectRes.floorDefectCount === 0,
        'autoDetectFloors does not misclassify WideCutoutTable as a floor slab');
    assert(autoDefectRes.overhangCount >= 1, 'autoDetectFloors catches WideCutoutTable overhang');

    const defectRes = bro.eye.structure(scene, {
        floors: [0, 3.5, 7.0],
        minFloorSeparation: 2.0,
        interiorWalk: walkOpts,
    });
    assert(defectRes.ok === false, 'defect multifloor + interiorWalk catches issues');
    assert(defectRes.floatingCount >= 1, 'caught FloatingSofa_F1');
    assert(defectRes.overhangCount >= 1, 'caught WideCutoutTable_F1 under explicit floors');
    assert(defectRes.interiorWalk.doorwaysBlocked === 1, 'caught blocked FrontDoor');
    assert(defectRes.interiorWalk.stairsBroken === 3, 'caught all 3 stair defects');
    assert(defectRes.issues.some(i => i.kind === 'stair-step-too-high' && i.nodeA === 'SteepWideStair'),
        'steep wide stair emitted stair-step-too-high');
    assert(defectRes.issues.some(i => i.kind === 'stair-low-headroom' && i.nodeA === 'HeadroomStair'),
        'low-headroom stair emitted stair-low-headroom');
    assert(defectRes.issues.some(i => i.kind === 'stair-gap' && i.nodeA === 'GapStairLow'),
        'missing low tread emitted stair-gap');

    console.log('OK test_eye_structure_multifloor: floors=' + cleanRes.floors.length + ' issues=' + defectRes.issues.length);
}
