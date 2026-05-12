// Sequencer: BPM, time signature, looping, automation lanes.

const ctx = new AudioContext();
const alloc = ctx.createVoiceAllocator(8);
alloc.setVoiceSetup((id, note, vel) => {
    ctx.setVoiceWaveform(id, 'sine');
    ctx.setVoiceFrequency(id, 440 * Math.pow(2, (note - 69)/12));
    ctx.setVoiceGain(id, 0.3 * vel);
    ctx.setVoiceAttack(id, 0.005);
    ctx.setVoiceSustain(id, 1.0);
    ctx.setVoiceRelease(id, 0.02);
});

const seq = ctx.createSequence(alloc);
assert(seq, 'createSequence returns sequence');

seq.setBPM(120);
console.log('bpm:', seq.bpm);
assert(seq.bpm === 120, 'BPM set/get: expected 120 got ' + seq.bpm);

let threw = false;
try { seq.setTimeSignature(4, 4); } catch (e) { threw = true; console.log('setTimeSignature threw:', e.message); }
assert(!threw, 'setTimeSignature does not throw');

seq.clearNotes();
seq.addNote(0, 60, 1.0, 0.5);
seq.addNote(1, 64, 1.0, 0.5);
seq.addNote(2, 67, 1.0, 0.5);
console.log('noteCount after 3 adds:', seq.noteCount);
assert(seq.noteCount === 3, 'noteCount after 3 adds = 3, got ' + seq.noteCount);

seq.removeNote(0);
console.log('noteCount after remove:', seq.noteCount);
assert(seq.noteCount === 2, 'noteCount after 1 remove = 2, got ' + seq.noteCount);

seq.clearNotes();
assert(seq.noteCount === 0, 'clearNotes resets count, got ' + seq.noteCount);

// Loop config
seq.setLoopEnabled(true);
seq.setLoopRange(0, 4);
console.log('loopEnabled:', seq.loopEnabled);
assert(seq.loopEnabled === true, 'loopEnabled is true after setLoopEnabled(true), got ' + seq.loopEnabled);

// Playing state
assert(seq.playing === false, 'sequence not playing before play(), got ' + seq.playing);
seq.play();
console.log('playing after play():', seq.playing);
assert(seq.playing === true, 'sequence playing after play(), got ' + seq.playing);

seq.stop();
assert(seq.playing === false, 'sequence not playing after stop(), got ' + seq.playing);

// Automation lanes
let lastVal = -999;
const laneIdx = seq.addAutomationLane((v) => { lastVal = v; });
console.log('lane idx:', laneIdx, 'automationLaneCount:', seq.automationLaneCount);
assert(typeof laneIdx === 'number' && laneIdx >= 0, 'addAutomationLane returns idx, got ' + laneIdx);
assert(seq.automationLaneCount === 1, 'automationLaneCount = 1, got ' + seq.automationLaneCount);

seq.addAutomationPoint(laneIdx, 0, 0.0);
seq.addAutomationPoint(laneIdx, 1, 1.0);
let threwInterp = false;
for (const m of ['linear', 'step', 'smooth']) {
    try { seq.setAutomationInterpMode(laneIdx, m); } catch (e) { threwInterp = true; console.log('interp ' + m + ' threw:', e.message); }
}
assert(!threwInterp, 'all 3 interp modes accepted');

seq.clearAutomationPoints(laneIdx);
seq.removeAutomationLane(laneIdx);
console.log('lanes after remove:', seq.automationLaneCount);
assert(seq.automationLaneCount === 0, 'automationLaneCount = 0 after remove, got ' + seq.automationLaneCount);
