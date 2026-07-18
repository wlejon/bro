// AI Chat starter — probe the GPU backend (bro.gpu), load a local GGUF
// checkpoint with bro.lm (Qwen3 or Mistral 3.1, dispatched on the filename),
// and stream chat completions into the page.
//
// No weights ship with a new project: click "Load model…" and point it at a
// .gguf you already have. Full API reference: docs/gpu-api.js and
// docs/lm-api.js in the bro repo.

const fs = require('fs');
const path = require('path');

const loadBtn = document.getElementById('load-btn');
const modelName = document.getElementById('model-name');
const statusEl = document.getElementById('status');
const chatEl = document.getElementById('chat');
const composer = document.getElementById('composer');
const promptEl = document.getElementById('prompt');
const sendBtn = document.getElementById('send-btn');

const state = {
    model: null,
    tokenizer: null,
    family: null,       // 'qwen3' | 'mistral3'
    handle: null,       // in-flight bro.lm.generate handle
    messages: [{ role: 'system', content: 'You are a helpful, concise assistant.' }],
};

// ── Status line ─────────────────────────────────────────────────────────────

function setStatus(text, kind) {
    statusEl.textContent = text;
    statusEl.className = 'status' + (kind ? ' ' + kind : '');
}

// ── GPU probe (bro.gpu — always present, honest about CPU fallback) ─────────

function probeGpu() {
    const badge = document.getElementById('gpu-badge');
    const detail = document.getElementById('gpu-detail');
    badge.textContent = bro.gpu.backend.toUpperCase();   // 'CUDA' | 'METAL' | 'CPU'
    badge.className = 'badge ' + (bro.gpu.available ? 'ok' : 'warn');

    const bits = [];
    const name = bro.gpu.deviceName();
    if (name) bits.push(name);
    const mem = bro.gpu.memoryInfo();
    if (mem) {
        bits.push((mem.freeBytes / 1e9).toFixed(1) + ' / ' +
                  (mem.totalBytes / 1e9).toFixed(1) + ' GB free');
    }
    bits.push('compiled: ' + bro.gpu.compiledBackends.join(', '));
    detail.textContent = bits.join(' · ');
}

// ── Chat log ────────────────────────────────────────────────────────────────

function addBubble(role, text) {
    const div = document.createElement('div');
    div.className = 'bubble ' + role;
    div.textContent = text;
    chatEl.appendChild(div);
    chatEl.scrollTop = chatEl.scrollHeight;
    return div;
}

function setBusy(busy) {
    promptEl.disabled = busy || !state.model;
    sendBtn.textContent = busy ? 'Stop' : 'Send';
    sendBtn.disabled = !state.model;
}

// ── Model load (button-triggered — the dialog blocks until you pick) ────────

function loadModel(ggufPath) {
    const base = path.basename(ggufPath);
    setStatus('Loading ' + base + '… (blocks while weights upload)');
    try {
        if (base.toLowerCase().includes('mistral')) {
            // Mistral GGUFs carry no tokenizer — tekken.json must sit next
            // to the checkpoint (see docs/lm-api.js).
            const tekken = path.join(path.dirname(ggufPath), 'tekken.json');
            if (!fs.existsSync(tekken)) {
                setStatus('Mistral checkpoints need tekken.json next to the ' +
                          '.gguf (' + tekken + ' not found).', 'warn');
                return;
            }
            const r = bro.lm.loadMistral(ggufPath, { tokenizerPath: tekken });
            state.model = r.model;
            state.tokenizer = r.tokenizer;
            state.family = 'mistral3';
        } else {
            const r = bro.lm.loadQwen(ggufPath);
            state.model = r.model;
            state.tokenizer = r.tokenizer;
            state.family = 'qwen3';
        }
    } catch (e) {
        setStatus('Load failed: ' + (e && e.message ? e.message : e), 'error');
        return;
    }
    modelName.textContent = base + ' (' + state.family + ', ' + bro.gpu.backend + ')';
    setStatus('Model ready.' + (bro.gpu.available ? '' :
              ' Running on CPU — expect slow generation.'), bro.gpu.available ? 'ok' : 'warn');
    promptEl.placeholder = 'Say something…';
    setBusy(false);
    promptEl.focus();
}

loadBtn.addEventListener('click', () => {
    const files = showOpenFileDialog('GGUF model|gguf');
    if (files.length) loadModel(files[0]);
});

// ── Chat (async streaming via bro.lm.generate) ──────────────────────────────

function eosId() {
    return state.family === 'mistral3' ? state.tokenizer.eosId
                                       : state.tokenizer.imEndId;
}

function send(text) {
    state.messages.push({ role: 'user', content: text });
    addBubble('user', text);

    const prompt = state.tokenizer.applyChatTemplate(state.messages, true);
    // The Mistral template emits its own <s>, so no addSpecial there either.
    const ids = state.family === 'mistral3'
        ? state.tokenizer.encode(prompt, false)
        : state.tokenizer.encode(prompt);

    const maxNew = 512;
    state.model.allocateCache(ids.length + maxNew);

    const bubble = addBubble('assistant', '…');
    const acc = [];
    setBusy(true);
    setStatus('Generating… (Stop cancels)');

    state.handle = bro.lm.generate(state.model, ids, {
        maxNewTokens: maxNew,
        eosId: eosId(),
        sampling: { temperature: 0.7, topK: 40, topP: 0.95 },
        onToken: (id) => {
            // Tokens can be partial UTF-8 — always re-decode the accumulation.
            acc.push(id);
            bubble.textContent = state.tokenizer.decode(acc);
            chatEl.scrollTop = chatEl.scrollHeight;
        },
        onDone: (outIds, info) => {
            state.handle = null;
            setBusy(false);
            if (info.error) {
                bubble.textContent = '(generation failed)';
                setStatus('Generation failed: ' + info.error, 'error');
                return;
            }
            const reply = state.tokenizer.decode(Array.from(outIds)).trim();
            bubble.textContent = reply || '(empty reply)';
            state.messages.push({ role: 'assistant', content: reply });
            setStatus(info.cancelled ? 'Stopped.' : 'Ready.');
            promptEl.focus();
        },
    });
}

composer.addEventListener('submit', (e) => {
    e.preventDefault();
    if (state.handle) {           // Stop button
        state.handle.cancel();
        return;
    }
    if (!state.model) return;
    const text = promptEl.value.trim();
    if (!text) return;
    promptEl.value = '';
    send(text);
});

// ── Startup ─────────────────────────────────────────────────────────────────

probeGpu();

if (typeof bro.lm.loadQwen !== 'function') {
    // Compiled-out stub ({ available: false }) — e.g. a minimal/app-profile
    // build without the AI tower.
    loadBtn.disabled = true;
    setStatus('This build has no language-model support (bro.lm is a stub). ' +
              'Rebuild with the "full" profile to chat.', 'warn');
} else {
    setStatus('No model loaded — click "Load model…" and pick a local .gguf ' +
              'checkpoint (Qwen3, or Mistral 3.1 with tekken.json alongside). ' +
              'New projects ship no weights.');
}
