// Inspection recorded and compared afterwards (scene.recordInspect,
// keepInspect, stopInspectRecording, inspectRecording;
// scene/vulkan/frame/scene_inspect_record.h), and inspectFrame, which
// compares as it goes.
// Verifies:
//   - each kept render is a frame of the recording, the first compared with
//     none, the rest with the one kept before
//   - a still view is the same surfaces with nothing moved
//   - a box that appears in front of the ground rose; the camera's travel
//     between two frames is the eye's
//   - the frames' numbers match inspectFrame's for the same renders (both
//     cut to the same size they would differ: here the recording is kept
//     at full size, scale 1)
//   - the change map of a frame comes back at the recording's size
//   - a file that is not a recording throws

const fs = require('fs');
const os = require('os');
const path = require('path');

const S = 96;
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(S));
canvas.setAttribute('height', String(S));
document.body.appendChild(canvas);
flush();
const scn = canvas.getContext('scene');
scn.setCamera({ fov: 60, near: 0.1, far: 200, position: [0, 6, 12], target: [0, 0, 0] });
scn.createMesh({ mesh: 'plane', halfW: 50, halfD: 50, color: 'white' });
scn.createLight({ type: 'directional', direction: [0.3, -1, -0.2], intensity: 2 });
for (let i = 0; i < 3; i++) scn.captureFrame();

const file = path.join(os.tmpdir(), 'bro_test_inspect_' + Date.now() + '.inspect');
scn.setInspect(true);
scn.recordInspect(file, { scale: 1 });
const live = [];
const keep = () => {
    scn.captureFrame();
    assert(scn.keepInspect(), 'a render is kept');
    live.push(scn.inspectFrame({}));
};
keep();                 // 0: nothing before it
keep();                 // 1: the same view
const box = scn.createMesh({ mesh: 'box', halfW: 1.5, halfH: 1.5, halfD: 1.5, color: 'red', y: 1.5 });
keep();                 // 2: a box in front of the ground
scn.setCamera({ fov: 60, near: 0.1, far: 200, position: [0, 6, 13], target: [0, 0, 0] });
keep();                 // 3: the eye a metre back
assert(!scn.keepInspect(), 'nothing new to keep without a render');
assert(scn.stopInspectRecording() === 4, 'four frames kept');

const rec = scn.inspectRecording(file);
assert(rec.frames.length === 4, `the recording's frames (${rec.frames.length})`);
const [f0, f1, f2, f3] = rec.frames;
assert(!f0.compared && f1.compared && f2.compared && f3.compared, 'the first compared with none, the rest with the one before');
assert(f0.width === S && f0.height === S, `kept at full size (${f0.width} x ${f0.height})`);
assert(f1.same > 0.3 && f1.rose + f1.sank + f1.appeared + f1.vanished < 0.001,
       `a still view: the same surfaces, nothing moved (same ${f1.same.toFixed(3)})`);
assert(f2.rose > 0.01, `the box rose in front of the ground (${f2.rose.toFixed(3)})`);
assert(Math.abs(f3.travel - 1) < 0.01, `the eye travelled a metre (${f3.travel.toFixed(3)})`);
for (let k = 1; k < 4; k++)
    for (const key of ['same', 'rose', 'sank', 'appeared', 'vanished', 'lumaSpread', 'travel'])
        assert(Math.abs(rec.frames[k][key] - live[k][key]) < 1e-9,
               `frame ${k}'s ${key} as inspectFrame has it (${rec.frames[k][key]} / ${live[k][key]})`);

const pics = scn.inspectRecording(file, { image: 'change', frames: [2] });
assert(pics.images.length === 1 && pics.images[0] && pics.images[0].length === S * S * 4, 'the change map at the recording\'s size');

let threw = false;
const bad = path.join(os.tmpdir(), 'bro_test_inspect_bad_' + Date.now());
fs.writeFileSync(bad, 'not a recording');
try { scn.inspectRecording(bad); } catch (e) { threw = true; }
assert(threw, 'a file that is not a recording throws');

scn.setInspect(false);
try { fs.rmSync(file, { force: true }); fs.rmSync(bad, { force: true }); } catch (e) {}
console.log('test_inspect_record: OK');
