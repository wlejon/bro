// Boot the real Qwen TTS Lab headless and verify (a) the new Qwen-manifold voice
// basis loads, and (b) the identity-decoupling state machine: enroll/clone set a
// FAITHFUL 'clone' identity, while sculpting the map/sliders sets a 'design' one,
// with the meta reflecting which is live. Runs in the app's main JS context, so
// the lab's globals (qwen, variant, voiceBasis, designedXvec, identitySource,
// seedVoice, moveMapTo, updateDesignerMeta) are all in scope.
//
// Run:  bro-headless ../broworkshop/demos/qwen-tts-lab tests/_qwen_lab_smoke.js

function pumpUntil(p, b) { const s = Date.now(); while (!p() && Date.now() - s < b) sleep(50); return p(); }
const BASE = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';

// the lab auto-loads a default model on init(); let it settle, then switch to Base.
assert(pumpUntil(() => qwen && variant, 180000), 'default model loaded');
if (variant !== 'base') { loadModel(BASE); assert(pumpUntil(() => qwen && variant === 'base', 180000), 'Base model loaded'); }
console.log('[lab] variant=' + variant);

// (a) the new Qwen-manifold basis
assert(voiceBasis, 'voice basis loaded');
assert(voiceBasis.k === 24, 'k=24, got ' + voiceBasis.k);
assert(/VoiceDesign/.test(voiceBasis.source || ''), 'basis is the VoiceDesign corpus: ' + voiceBasis.source);
console.log('[lab] basis n=' + voiceBasis.n + ' k=' + voiceBasis.k + ' source="' + voiceBasis.source + '"');
console.log('[lab] axisName[0..3]=' + JSON.stringify(voiceBasis.axisName.slice(0, 4)));

// (b1) seeding a manifold point → 'design'
seedVoice('__mean__');
assert(identitySource === 'design', 'seed sets design, got ' + identitySource);

// (b2) a faithful clone identity → 'clone', meta says faithful
designedXvec = new Float32Array(voiceBasis.dim);
for (let i = 0; i < 16; i++) designedXvec[i] = 0.1 * (i + 1);
identitySource = 'clone';
updateDesignerMeta();
const cloneMeta = document.querySelector('#designer-meta').textContent;
assert(/faithful/.test(cloneMeta), 'clone meta shows faithful identity: "' + cloneMeta + '"');
console.log('[lab] clone meta: "' + cloneMeta + '"');

// (b3) sculpting the map switches identity back to a designed point
moveMapTo(0.6, -0.4);
assert(identitySource === 'design', 'sculpt switches to design, got ' + identitySource);
const designMeta = document.querySelector('#designer-meta').textContent;
assert(/designed x-vector/.test(designMeta), 'design meta shows designed x-vector: "' + designMeta + '"');
console.log('[lab] design meta: "' + designMeta + '"');

console.log('[lab] PASS');
