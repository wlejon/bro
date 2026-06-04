// Generate (x-vector -> style) training pairs for the audio->style bridge.
// For each cached style vector: build the voice, render a short phrase, and
// encode it to a Qwen ECAPA x-vector. The style is broadcast across all rows by
// createVoice, so the short render still uses exactly the cached style.
//
// Out: D:/projects/voice-sweep/pairs.jsonl  — {id, method, x:[1024], style:[256]}

const fs = require('fs');
const SWEEP = 'D:/projects/voice-sweep';
const kdir = 'D:/projects/brosoundml/weights/kokoro';
const qdir = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
bro.tts.setAssetRoot('D:/projects/brosoundml');

const k = bro.tts.loadKokoro(kdir);
const q = bro.tts.loadQwen(qdir);
const ids = bro.tts.phonemize('Hello, this is my voice.');   // short clip -> fast embed

const rows = fs.readFileSync(SWEEP + '/sweep.jsonl', 'utf8').trim().split('\n').map(s => JSON.parse(s));
console.log('pairs to build:', rows.length, '· render phonemes', ids.length);

const out = [];
const t0 = Date.now ? 0 : 0; // (Date.now unavailable in some sandboxes; we log by count)
for (let i = 0; i < rows.length; i++) {
  const r = rows[i];
  const v = k.createVoice(Float32Array.from(r.style), 's');
  const audio = k.synthesize(ids, v).samples;            // 24 kHz mono
  const x = q.embedSpeaker(audio, { sampleRate: 24000 });
  out.push(JSON.stringify({
    id: r.id, method: r.method,
    x: Array.from(x).map(v => +v.toFixed(6)),
    style: r.style,
  }));
  if ((i + 1) % 50 === 0) console.log('  ', i + 1, '/', rows.length);
}

fs.writeFileSync(SWEEP + '/pairs.jsonl', out.join('\n') + '\n');
console.log('WROTE', out.length, 'pairs ->', SWEEP + '/pairs.jsonl');
console.log('DONE');
