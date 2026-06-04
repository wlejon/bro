// Smoke test for the Qwen ECAPA speaker encoder exposed to JS.
// Proves the audio->identity front-end for the style adapter:
//   - embedSpeaker returns a finite 1024-D x-vector
//   - it's deterministic (same audio -> identical embedding)
//   - it discriminates speakers (different Kokoro voices -> distant x-vectors,
//     and same voice across two utterances -> close x-vectors)

const kdir = 'D:/projects/brosoundml/weights/kokoro';
const qdir = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
bro.tts.setAssetRoot('D:/projects/brosoundml');

const k = bro.tts.loadKokoro(kdir);
const q = bro.tts.loadQwen(qdir);
console.log('qwen variant', q.variant, '· loaded', q.loaded);

const idsA = bro.tts.phonemize('Hello there. This is a test of the pipeline.');
const idsB = bro.tts.phonemize('The quick brown fox jumps over the lazy dog.');

function render(voiceName, ids) {
  const v = k.loadVoice(kdir + '/voices/' + voiceName + '.bin');
  return k.synthesize(ids, v).samples;   // 24 kHz mono — matches the encoder
}
function embed(samples) { return q.embedSpeaker(samples, { sampleRate: 24000 }); }

function cos(a, b) {
  let d = 0, na = 0, nb = 0;
  for (let i = 0; i < a.length; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
  return d / (Math.sqrt(na) * Math.sqrt(nb) + 1e-9);
}
function finite(a) { let nf = 0; for (let i = 0; i < a.length; i++) if (!isFinite(a[i])) nf++; return nf; }

// female vs male vs a second female — distinct identities
const heartA = embed(render('af_heart', idsA));
const heartB = embed(render('af_heart', idsB));   // same voice, different words
const heartA2 = embed(render('af_heart', idsA));  // exact repeat -> deterministic
const adam = embed(render('am_adam', idsA));
const bella = embed(render('af_bella', idsA));

console.log('dim', heartA.length, '· nonFinite', finite(heartA));
console.log('determinism  cos(heartA, heartA2)      =', cos(heartA, heartA2).toFixed(5), '(expect ~1.0)');
console.log('same speaker cos(heartA, heartB-words) =', cos(heartA, heartB).toFixed(4));
console.log('cross female cos(heartA, bella)        =', cos(heartA, bella).toFixed(4));
console.log('cross gender cos(heartA, adam)         =', cos(heartA, adam).toFixed(4));

const sameOK  = cos(heartA, heartB) > cos(heartA, adam);
const detOK   = cos(heartA, heartA2) > 0.999;
console.log('VERDICT  deterministic:', detOK, '· same-speaker closer than cross-speaker:', sameOK);
console.log('DONE');
