// VoiceAllocator polyphony, voice stealing, note on/off, setup callback.

const ctx = new AudioContext();
const alloc = ctx.createVoiceAllocator(4);
assert(alloc, 'allocator created');
assert(alloc.activeVoiceCount === 0, 'starts with 0 active voices, got ' + alloc.activeVoiceCount);

// Configure voices on allocation
let setupCalls = 0;
alloc.setVoiceSetup((voiceId, note, vel) => {
    setupCalls++;
    ctx.setVoiceWaveform(voiceId, 'sine');
    ctx.setVoiceFrequency(voiceId, 440 * Math.pow(2, (note - 69) / 12));
    ctx.setVoiceGain(voiceId, 0.2 * vel);
    ctx.setVoiceAttack(voiceId, 0.005);
    ctx.setVoiceDecay(voiceId, 0.01);
    ctx.setVoiceSustain(voiceId, 0.8);
    ctx.setVoiceRelease(voiceId, 0.05);
});

const v1 = alloc.noteOn(60, 1.0);
const v2 = alloc.noteOn(64, 1.0);
const v3 = alloc.noteOn(67, 1.0);
sleep(50);
console.log('after 3 noteOns: active=', alloc.activeVoiceCount, 'setupCalls=', setupCalls);
assert(setupCalls === 3, 'setup invoked once per noteOn, got ' + setupCalls);
assert(alloc.activeVoiceCount === 3, 'three active voices, got ' + alloc.activeVoiceCount);

assert(alloc.voiceForNote(60) === v1, 'voiceForNote(60) returns v1 (got ' + alloc.voiceForNote(60) + ' expected ' + v1 + ')');
assert(alloc.voiceForNote(99) === -1, 'voiceForNote(unheld) returns -1, got ' + alloc.voiceForNote(99));

// noteOff releases (voice may linger through release tail before activeVoiceCount drops)
alloc.noteOff(60);
sleep(120);
console.log('after noteOff(60) + 120ms: active=', alloc.activeVoiceCount);
assert(alloc.activeVoiceCount <= 2, 'released voice no longer active, got ' + alloc.activeVoiceCount);

// Voice stealing: max=4, so the 5th note should steal
alloc.allNotesOff();
sleep(120);
assert(alloc.activeVoiceCount === 0, 'allNotesOff cleared active count, got ' + alloc.activeVoiceCount);

alloc.setStealPolicy('oldest');
const sv = [];
for (let i = 0; i < 6; i++) {
    sv.push(alloc.noteOn(60 + i, 1.0));
}
sleep(30);
console.log('after 6 noteOns on maxVoices=4: active=', alloc.activeVoiceCount);
assert(alloc.activeVoiceCount <= 4, 'active count capped at maxVoices, got ' + alloc.activeVoiceCount);

// setMaxVoices
alloc.allNotesOff();
sleep(60);
alloc.setMaxVoices(2);
for (let i = 0; i < 4; i++) alloc.noteOn(60 + i, 1.0);
sleep(30);
assert(alloc.activeVoiceCount <= 2, 'after setMaxVoices(2), active count <= 2, got ' + alloc.activeVoiceCount);

// Steal policies should at least accept all documented values without throwing
for (const p of ['oldest', 'quietest', 'samenote', 'none']) {
    let threw = false;
    try { alloc.setStealPolicy(p); } catch (e) { threw = true; console.log('setStealPolicy(' + p + ') threw:', e.message); }
    assert(!threw, 'setStealPolicy(' + p + ') does not throw');
}
