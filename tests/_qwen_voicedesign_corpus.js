// Generate a Qwen-TTS voice-design CORPUS: drive the 1.7B VoiceDesign checkpoint
// across an attribute-prompt grid, embed each rendered clip with the standalone
// ECAPA speaker encoder, and write one labeled x-vector per recipe. This is the
// raw material for _qwen_voice_axes.js — supervised, DISENTANGLED voice-design
// axes that cover Qwen's OWN realizable voice manifold, not the narrow CAMEO PCA
// the qwen_voice_basis sliders are built from.
//
// Why generate instead of reuse CAMEO: the CAMEO basis is PCA over a few dozen
// real actors, so it spans at most an (N-1)-dim slice of the 1024-D x-vector space
// and reflects that population's variance, not what Qwen can produce. Here we
// SAMPLE the producible manifold directly — author attribute prompts, let Qwen
// realize them, embed the result in the SAME ECAPA space Base conditions on.
//
// The design trick that makes the axes clean: every attribute level is sampled
// INDEPENDENTLY and uniformly (a randomized factorial), so the attributes are
// de-correlated by construction. _qwen_voice_axes.js then ridge-regresses the
// x-vector on the attribute matrix; each regression weight is the PARTIAL
// direction for one attribute with the others held fixed — i.e. a disentangled
// axis. (Per-clip diff-of-means would also work given the independence, but the
// regression de-correlates residual leakage too.)
//
// Labels are the PROMPTED attribute levels (we assume VoiceDesign realizes them);
// the axes are validated against REAL Base synthesis in _qwen_voice_axes_sweep.js
// (gate on the model's f0 / brightness, never on this script's assumptions).
//
// Run (GPU — VoiceDesign needs it; the encoder runs host-side):
//   bro-headless tests/_smoke_app tests/_qwen_voicedesign_corpus.js
//   BRO_CORPUS_N=600 bro-headless tests/_smoke_app tests/_qwen_voicedesign_corpus.js

const fs = require('fs');

// Absolute paths: bro.tts and brokit's fs resolve relative paths against
// different bases under bro-headless, so don't rely on cwd.
const VD_DIR  = 'D:/projects/brosoundml/weights/qwen-tts/1.7B-voicedesign';
const ENC_DIR = 'D:/projects/brosoundml-data/qwen-tts/speaker-encoder';
// MODE=grid → tight attribute factorial (good for supervised axes, but PCA over it
// is lopsided — variance piles on pitch). MODE=diverse → rich free-text character
// prompts spanning many timbres, so PCA variance spreads across axes (a balanced,
// broad basis, the right input for the voice-map sliders). MODE=emotion → each of N
// diverse base voices rendered with all 12 EMO emotions (6 basic + 6 dyad blends),
// for a within-voice contrastive emotion basis on Qwen's manifold. Default: diverse.
const MODE = (_env('BRO_CORPUS_MODE') || 'diverse').toLowerCase();
const OUT_DIR = 'D:/projects/brosoundml-data/qwen-tts/voicedesign-corpus' +
  (MODE === 'diverse' ? '-diverse' : MODE === 'emotion' ? '-emotion' : '');
// A phonetically broad, fixed sentence: identity-bearing, ~3 s, same for every
// recipe so the only thing that varies across rows is the requested voice.
const TEXT = 'The quick brown fox jumps over the lazy dog while she sells sea shells.';
function _env(k) { try { return process.env[k]; } catch (e) { return null; } }
const N = (() => { const e = +(_env('BRO_CORPUS_N') || 0); return e > 0 ? e : 64; })();

// ── attribute taxonomy: numeric level → instruct fragment ─────────────────────
// gender/pitch/brightness/weight/roughness are bipolar (signed); age is ordinal;
// breathiness is a 0/1 quality. Levels are sampled independently (see SAMPLE).
const AGE   = { '-2': 'child-like', '-1': 'youthful', '0': 'adult', '1': 'middle-aged', '2': 'elderly' };
const GENDER= { '-1': 'feminine', '0': 'androgynous', '1': 'masculine' };
const PITCH = { '-2': 'very low-pitched', '-1': 'low-pitched', '0': 'medium-pitched', '1': 'high-pitched', '2': 'very high-pitched' };
const BRIGHT= { '-1': 'warm and dark', '0': '', '1': 'bright and crisp' };
const WEIGHT= { '-1': 'light and thin', '0': '', '1': 'deep and resonant' };
const ROUGH = { '-1': 'smooth and clear', '0': '', '1': 'rough and gravelly' };
const BREATH= { '0': '', '1': 'breathy and soft' };
// the level pools each attribute is drawn from (uniform, independent)
const POOL = {
  gender: [-1, 0, 1], age: [-2, -1, 0, 1, 2], pitch: [-2, -1, 0, 1, 2],
  bright: [-1, 0, 1], weight: [-1, 0, 1], rough: [-1, 0, 1], breath: [0, 1],
};
const ATTRS = ['gender', 'age', 'pitch', 'bright', 'weight', 'rough', 'breath'];

function instructOf(r) {
  const head = 'A ' + AGE[r.age] + ', ' + GENDER[r.gender] + ' voice';
  const qual = [PITCH[r.pitch], BRIGHT[r.bright], WEIGHT[r.weight], ROUGH[r.rough], BREATH[r.breath]].filter(Boolean);
  return head + ': ' + qual.join(', ') + '.';
}

// seeded xorshift32 — reproducible recipe sampling
let _s = (0x1234abcd) >>> 0;
function rnd() { _s ^= _s << 13; _s >>>= 0; _s ^= _s >> 17; _s ^= _s << 5; _s >>>= 0; return _s / 4294967296; }
function pick(arr) { return arr[Math.floor(rnd() * arr.length)]; }
function sampleRecipe() { const r = {}; for (const a of ATTRS) r[a] = pick(POOL[a]); return r; }

// ── diverse mode: rich free-text character prompts ────────────────────────────
// Variance is spread across MANY perceptual dimensions (timbre adjectives, roles,
// energy) instead of a tight pitch/age/gender grid, so PCA over the result is
// balanced rather than V1-dominated. We still record ordinal gender/age/pitch (for
// axis annotation in the basis builder); timbre lives only in the free text.
const D_AGE    = [['child-like', -2], ['youthful', -1], ['adult', 0], ['middle-aged', 1], ['elderly', 2]];
const D_GENDER = [['woman', -1], ['man', 1], ['person', 0]];
const D_PITCH  = [['very low-pitched', -2], ['low-pitched', -1], ['', 0], ['high-pitched', 1], ['very high-pitched', 2]];
// timbre adjectives — the rich axis the grid was missing (pick 1–3)
const D_TIMBRE = ['raspy', 'gravelly', 'silky', 'smooth', 'nasal', 'booming', 'thin', 'reedy', 'warm',
  'metallic', 'breathy', 'crisp', 'mellow', 'sharp', 'husky', 'bright', 'dark', 'rich', 'hollow',
  'resonant', 'velvety', 'brittle', 'airy', 'throaty', 'clear', 'rough', 'plummy', 'gritty'];
const D_ROLE   = ['', '', '', 'radio announcer', 'storyteller', 'news anchor', 'opera singer',
  'cartoon character', 'drill sergeant', 'wizard', 'pirate', 'professor', 'noir detective', 'game-show host'];
const D_ENERGY = ['', '', 'energetic', 'calm', 'languid', 'brisk', 'weary', 'excited'];

// A base voice (no energy/emotion tail) — the part held FIXED across emotions in
// emotion mode, and the body of a diverse-mode prompt.
function diverseBase() {
  const [ageW, age] = pick(D_AGE), [genW, gender] = pick(D_GENDER), [pchW, pitch] = pick(D_PITCH);
  const role = pick(D_ROLE);
  const nT = 1 + Math.floor(rnd() * 3);                  // 1–3 timbre adjectives, distinct
  const tset = new Set(); while (tset.size < nT) tset.add(pick(D_TIMBRE));
  const timbre = [...tset];
  const subject = (ageW + ' ' + (role || genW)).trim();
  const quals = [pchW].filter(Boolean).concat(timbre);
  return { head: 'A ' + subject + ' with a ' + quals.join(', ') + ' voice',
           attrs: { gender, age, pitch, timbre: timbre.join('+'), role: role || '' } };
}
function sampleDiverse() {
  const b = diverseBase();
  const energy = pick(D_ENERGY);
  const tail = energy ? ', speaking in ' + (/^[aeiou]/i.test(energy) ? 'an ' : 'a ') + energy + ' way' : '';
  return { attrs: { ...b.attrs, energy: energy || '' }, instruct: b.head + tail + '.' };
}

// ── emotion mode: 12 emotions over the 6 basic axes (6 basic + 6 dyad blends) ──
// The 6 BASIC are the sliders; the 6 BLENDS are 0.5/0.5 dyads of two basics, so the
// emotion-basis regression (_qwen_emotion_basis_vd.js) sees the inter-emotion space
// and "slider A + slider B" lands where Qwen actually renders the blend. `comp` is
// each emotion's composition over BASIC6 — the regression design row.
const BASIC6 = ['ANG', 'DIS', 'FEA', 'HAP', 'SAD', 'SUR'];
const EMO = [
  { code: 'ANG', word: 'angry',        comp: { ANG: 1 } },
  { code: 'DIS', word: 'disgusted',    comp: { DIS: 1 } },
  { code: 'FEA', word: 'fearful',      comp: { FEA: 1 } },
  { code: 'HAP', word: 'happy',        comp: { HAP: 1 } },
  { code: 'SAD', word: 'sad',          comp: { SAD: 1 } },
  { code: 'SUR', word: 'surprised',    comp: { SUR: 1 } },
  { code: 'CON', word: 'contemptuous', comp: { ANG: 0.5, DIS: 0.5 } },   // contempt
  { code: 'OUT', word: 'outraged',     comp: { ANG: 0.5, SUR: 0.5 } },   // outrage
  { code: 'EXC', word: 'excited',      comp: { HAP: 0.5, SUR: 0.5 } },   // excitement
  { code: 'BIT', word: 'bittersweet',  comp: { HAP: 0.5, SAD: 0.5 } },   // bittersweet
  { code: 'AWE', word: 'awestruck',    comp: { FEA: 0.5, SUR: 0.5 } },   // awe
  { code: 'ANX', word: 'anxious',      comp: { FEA: 0.5, SAD: 0.5 } },   // anxiety
];
function emotionInstruct(head, word) {
  return head + ', speaking in ' + (/^[aeiou]/i.test(word) ? 'an ' : 'a ') + word + ' tone.';
}

function pumpUntil(pred, budgetMs) {
  const start = Date.now();
  while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
  return pred();
}

// ── load VoiceDesign (GPU, async) + the standalone ECAPA encoder (host-side) ──
bro.tts.setAssetRoot('D:/projects/brosoundml');
let vd = null, vdErr = null;
bro.tts.loadQwen(VD_DIR, { onReady: (q) => { vd = q; }, onError: (m) => { vdErr = m; } });
if (!pumpUntil(() => vd || vdErr, 240000)) { console.log('TIMEOUT loading VoiceDesign'); throw new Error('vd load timeout'); }
if (vdErr) throw new Error('VoiceDesign load: ' + vdErr);
console.log('VoiceDesign loaded · variant', vd.variant, '· size', vd.modelSize);

const enc = bro.tts.loadSpeakerEncoder(ENC_DIR);
console.log('speaker encoder loaded');

fs.mkdirSync(OUT_DIR, { recursive: true });
const outPath = OUT_DIR + '/xvecs.jsonl';
fs.writeFileSync(outPath, '');           // truncate; rows are appended as they render

let DIM = 0, ok = 0, bad = 0;
const t0 = Date.now();

// Greedy synth + ECAPA embed of one prompt → x-vector (or null on empty/non-finite).
function renderEmbed(instruct) {
  const buf = vd.synthesize(TEXT, { instruct, language: 'english' });   // greedy → deterministic per prompt
  if (!buf || !buf.samples || !buf.samples.length) return null;
  const x = enc.embedSpeaker(buf.samples, { sampleRate: buf.sampleRate });
  for (let d = 0; d < x.length; d++) if (!isFinite(x[d])) return null;
  if (!DIM) DIM = x.length;
  return x;
}
function progress(done, total) {
  const el = (Date.now() - t0) / 1000;
  console.log('  ' + done + '/' + total + ' · ok ' + ok + ' · ' + el.toFixed(0) + 's · ' + (el / done).toFixed(2) + 's/clip');
}

if (MODE === 'emotion') {
  // N base voices, each rendered with all 12 emotions (the base voice held fixed).
  const total = N * EMO.length;
  let done = 0;
  for (let v = 0; v < N; v++) {
    const base = diverseBase();
    for (const em of EMO) {
      const instruct = emotionInstruct(base.head, em.word);
      try {
        const x = renderEmbed(instruct);
        if (!x) { bad++; console.log('  [v' + v + ' ' + em.code + '] empty/non-finite'); }
        else { fs.appendFileSync(outPath, JSON.stringify({ voice: v, emotion: em.code, comp: em.comp, attrs: base.attrs, instruct, x: Array.from(x, (q) => +q.toFixed(6)) }) + '\n'); ok++; }
      } catch (e) { bad++; console.log('  [v' + v + ' ' + em.code + '] synth failed: ' + e.message); }
      done++;
      if (done % 16 === 0 || done === total) progress(done, total);
    }
  }
} else {
  for (let i = 0; i < N; i++) {
    const rec = MODE === 'diverse' ? sampleDiverse() : (() => { const r = sampleRecipe(); return { attrs: r, instruct: instructOf(r) }; })();
    try {
      const x = renderEmbed(rec.instruct);
      if (!x) { bad++; console.log('  [' + i + '] empty/non-finite: ' + rec.instruct); }
      else { fs.appendFileSync(outPath, JSON.stringify({ id: i, attrs: rec.attrs, instruct: rec.instruct, x: Array.from(x, (q) => +q.toFixed(6)) }) + '\n'); ok++; }
    } catch (e) { bad++; console.log('  [' + i + '] synth failed: ' + e.message); }
    if ((i + 1) % 16 === 0 || i + 1 === N) progress(i + 1, N);
  }
}

fs.writeFileSync(OUT_DIR + '/corpus_meta.json', JSON.stringify({
  n: ok, requested: N, dim: DIM, mode: MODE, text: TEXT,
  source: 'Qwen3-TTS 1.7B VoiceDesign, greedy, self-rendered; ECAPA x-vectors',
  grid: MODE === 'grid' ? { attrs: ATTRS, pools: POOL } : undefined,
  diverse: MODE === 'diverse' ? { timbre: D_TIMBRE, roles: D_ROLE.filter(Boolean), energy: D_ENERGY.filter(Boolean) } : undefined,
  emotion: MODE === 'emotion' ? { voices: N, basic: BASIC6, emotions: EMO.map((e) => ({ code: e.code, word: e.word, comp: e.comp })) } : undefined,
}, null, 2));

console.log('—');
console.log('wrote', ok, 'rows (' + bad + ' dropped) · dim', DIM, '->', outPath);
console.log('DONE');
