// End-to-end round-trip eval of the audio->style bridge, on the real voices
// (anchors) that were held out of B's training.
//   x (anchor x-vector) --B--> style' --Kokoro--> audio' --ECAPA--> x_rt
// Good bridge: cos(x, x_rt) high (identity survives the round trip) and well
// above the "just use the average voice" baseline; cos(style', trueStyle) high.

const fs = require('fs');
const DIR = 'D:/projects/voice-sweep';
const kdir = 'D:/projects/brosoundml/weights/kokoro';
const qdir = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
bro.tts.setAssetRoot('D:/projects/brosoundml');

const br = JSON.parse(fs.readFileSync(DIR + '/bridge.json', 'utf8'));
const pairs = fs.readFileSync(DIR + '/pairs.jsonl', 'utf8').trim().split('\n').map(s => JSON.parse(s));
const anchorsMeta = JSON.parse(fs.readFileSync(DIR + '/anchors.json', 'utf8'));
const { D, M, B, xm, ym } = br;

const k = bro.tts.loadKokoro(kdir);
const q = bro.tts.loadQwen(qdir);
const ids = bro.tts.phonemize('Hello, this is my voice.');     // same short phrase as pairs

function apply(x) {                                            // style = ym + (x-xm).B
  const s = new Float64Array(M);
  for (let m = 0; m < M; m++) s[m] = ym[m];
  for (let j = 0; j < D; j++) { const xc = x[j] - xm[j]; if (!xc) continue; const bj = j * M; for (let m = 0; m < M; m++) s[m] += xc * B[bj + m]; }
  return s;
}
function embedStyle(style) {
  const v = k.createVoice(Float32Array.from(style), 's');
  return q.embedSpeaker(k.synthesize(ids, v).samples, { sampleRate: 24000 });
}
const cos = (a, b) => { let d = 0, na = 0, nb = 0; for (let i = 0; i < b.length; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return d / (Math.sqrt(na) * Math.sqrt(nb) + 1e-12); };

// baseline: identity of the centroid ("average") voice
const xCentroid = embedStyle(anchorsMeta.centroid);

const anchors = pairs.filter(p => p.method === 'anchor');
let sumStyle = 0, sumX = 0, sumBase = 0, n = 0, worstX = 1, worstName = '';
const perName = [];
for (let i = 0; i < anchors.length; i++) {
  const a = anchors[i];
  const stylePred = apply(a.x);
  const xrt = embedStyle(stylePred);
  const cStyle = cos(stylePred, a.style);
  const cX = cos(a.x, xrt);
  const cBase = cos(a.x, xCentroid);
  sumStyle += cStyle; sumX += cX; sumBase += cBase; n++;
  if (cX < worstX) { worstX = cX; worstName = anchorsMeta.names[i]; }
  perName.push([anchorsMeta.names[i], cStyle, cX]);
}
console.log('anchors evaluated:', n);
console.log('mean cos(style_pred, true_style) =', (sumStyle / n).toFixed(4));
console.log('mean cos(x, x_roundtrip)         =', (sumX / n).toFixed(4), '  <- bridge preserves identity');
console.log('mean cos(x, avg-voice) BASELINE  =', (sumBase / n).toFixed(4), '  <- beat this');
console.log('worst round-trip:', worstName, worstX.toFixed(4));
perName.sort((a, b) => a[2] - b[2]);
console.log('lowest 4 round-trips:', JSON.stringify(perName.slice(0, 4).map(p => [p[0], +p[2].toFixed(3)])));
console.log('highest 4 round-trips:', JSON.stringify(perName.slice(-4).map(p => [p[0], +p[2].toFixed(3)])));
console.log('DONE');
