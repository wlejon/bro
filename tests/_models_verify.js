// Verify bro.models against UNMODIFIED upstream files (no dev paths), into a
// temp BRO_MODELS_DIR: proves the network download path, the dataset URL form,
// upstream-format Whisper loading (brosoundml upcast), and the added_tokens.json
// tokenizer merge (brolm). Run: BRO_MODELS_DIR=<tmp> bro-headless <app> this.js
const fs = require('fs');

const SPECS = [
  { id: 'whisper.config', repo: 'openai/whisper-tiny', kind: 'model', file: 'config.json' },
  { id: 'whisper.model',  repo: 'openai/whisper-tiny', kind: 'model', file: 'model.safetensors' },
  { id: 'whisper.vocab',  repo: 'openai/whisper-tiny', kind: 'model', file: 'vocab.json' },
  { id: 'whisper.merges', repo: 'openai/whisper-tiny', kind: 'model', file: 'merges.txt' },
  { id: 'whisper.added',  repo: 'openai/whisper-tiny', kind: 'model', file: 'added_tokens.json' },
  { id: 'wake', repo: 'wlejon/brosoundml-data', kind: 'dataset', file: 'wake/computer.bw' },
];

console.log('cacheDir=' + bro.models.cacheDir());
let lastLog = 0;
const paths = await bro.models.ensure(SPECS, {
  onProgress: (p) => {
    if (p.cached) { console.log('  ' + p.id + ' (cached)'); return; }
    if (p.received - lastLog > 8 * 1024 * 1024 || p.received === p.total) {
      lastLog = p.received;
      console.log('  ' + p.id + ' ' + (p.received / 1048576).toFixed(1) +
                  '/' + (p.total / 1048576).toFixed(1) + ' MB');
    }
  },
});

console.log('wake downloaded, exists=' + fs.existsSync(paths['wake']));
assert(fs.existsSync(paths['wake']), 'wake/computer.bw should be in cache');

// Load Whisper from the cached upstream checkout (exercises the F16/BF16->F32
// upload path; openai/whisper-tiny config.json + model.safetensors).
const dir = paths['whisper.model'].replace(/[\/\\][^\/\\]*$/, '');
const whisper = await new Promise((res, rej) =>
  bro.stt.loadWhisper(dir, { onReady: res, onError: (m) => rej(new Error(m)) }));
console.log('whisper loaded=' + !!whisper);
assert(whisper, 'loadWhisper should succeed against unmodified upstream');

// Load the tokenizer with the SEPARATE upstream added_tokens.json. Without the
// merge, the "<|...|>" specials are missing and buildPrompt can't form the
// start-of-transcript / language / task prompt.
const tok = await new Promise((res, rej) =>
  bro.stt.loadTokenizer({
    vocabPath: paths['whisper.vocab'],
    mergesPath: paths['whisper.merges'],
    addedTokensPath: paths['whisper.added'],
    onReady: res, onError: (m) => rej(new Error(m)),
  }));
const prompt = tok.buildPrompt('en', 'transcribe', false);
const ids = Array.from(prompt || []);
console.log('sttPrompt ids=' + JSON.stringify(ids));
assert(ids.length >= 3, 'buildPrompt needs the merged specials (SOT/lang/task)');

console.log('VERIFY_OK');
