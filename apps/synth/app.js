// ---------------------------------------------------------------------------
// Audio Synth + Visualizer
// ---------------------------------------------------------------------------

// Helper: querySelectorAll returns a NodeList which may lack .forEach
function $$(sel) { return Array.from(document.querySelectorAll(sel)); }

let audioCtx, analyser, masterGain;
try {
    audioCtx = new AudioContext();
    analyser = audioCtx.createAnalyser();
    analyser.fftSize = 2048;
    analyser.smoothingTimeConstant = 0.85;
    // Master gain merges synth + mic before analyser
    masterGain = audioCtx.createGain();
    masterGain.gain.value = 1.0;
    masterGain.connect(analyser);
    analyser.source = 2; // blend output mix + mic
    analyser.connect(audioCtx.destination);
} catch (e) {
    console.warn('AudioContext unavailable:', e.message);
}

// State
let waveform = 'sine';
let synthVolume = 0.3;
let micVolume = 0.5;
let vizMode = 'spectrum';
let micEnabled = false;
let micStream = null;
let micSourceNode = null;
let micAnalyser = null; // separate analyser for mic level + pitch
let activeNotes = new Map(); // note -> { osc, gain }

// Note definitions — 3 octaves starting at C3
const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
const BASE_OCTAVE = 3;
const NUM_OCTAVES = 3;
const notes = [];

for (let oct = BASE_OCTAVE; oct < BASE_OCTAVE + NUM_OCTAVES; oct++) {
    for (let i = 0; i < 12; i++) {
        const name = NOTE_NAMES[i] + oct;
        const midi = (oct + 1) * 12 + i;
        const freq = 440 * Math.pow(2, (midi - 69) / 12);
        const isBlack = [1,3,6,8,10].includes(i);
        notes.push({ name, freq, midi, isBlack, octave: oct, noteIndex: i });
    }
}

// Keyboard mapping (computer keys -> note indices)
const KEY_MAP = {
    'z': 0, 's': 1, 'x': 2, 'd': 3, 'c': 4, 'v': 5, 'g': 6,
    'b': 7, 'h': 8, 'n': 9, 'j': 10, 'm': 11,
    'q': 12, '2': 13, 'w': 14, '3': 15, 'e': 16, 'r': 17, '5': 18,
    't': 19, '6': 20, 'y': 21, '7': 22, 'u': 23,
    'i': 24, '9': 25, 'o': 26, '0': 27, 'p': 28,
};

// ---------------------------------------------------------------------------
// Build piano keyboard UI
// ---------------------------------------------------------------------------

const keyboard = document.getElementById('keyboard');
const whiteKeys = notes.filter(n => !n.isBlack);
const blackKeys = notes.filter(n => n.isBlack);

// Create white keys
whiteKeys.forEach((note, i) => {
    const el = document.createElement('div');
    el.className = 'white-key';
    const noteIdx = notes.indexOf(note);
    el.setAttribute('data-note-idx', noteIdx.toString());

    const binding = Object.entries(KEY_MAP).find(([,v]) => v === noteIdx);
    const label = document.createElement('div');
    label.className = 'key-label';
    label.textContent = binding ? binding[0].toUpperCase() : '';
    el.appendChild(label);

    keyboard.appendChild(el);
    note.element = el;
});

// Create black keys positioned over white keys
blackKeys.forEach(note => {
    const noteIdx = notes.indexOf(note);
    const whiteIdx = whiteKeys.filter(w => notes.indexOf(w) < noteIdx).length;
    const whiteKeyWidth = 100 / whiteKeys.length;
    const leftPos = whiteIdx * whiteKeyWidth - whiteKeyWidth * 0.3;

    const el = document.createElement('div');
    el.className = 'black-key';
    el.setAttribute('data-note-idx', noteIdx.toString());
    el.style.left = leftPos + '%';
    el.style.width = (whiteKeyWidth * 0.6) + '%';

    const binding = Object.entries(KEY_MAP).find(([,v]) => v === noteIdx);
    const label = document.createElement('div');
    label.className = 'key-label';
    label.textContent = binding ? binding[0].toUpperCase() : '';
    el.appendChild(label);

    keyboard.appendChild(el);
    note.element = el;
});

// ---------------------------------------------------------------------------
// Synth engine
// ---------------------------------------------------------------------------

function noteOn(noteIdx) {
    if (!audioCtx) return;
    if (noteIdx < 0 || noteIdx >= notes.length) return;
    if (activeNotes.has(noteIdx)) return;

    const note = notes[noteIdx];
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();

    osc.type = waveform;
    osc.frequency.value = note.freq;
    gain.gain.value = synthVolume;

    osc.connect(gain).connect(masterGain);
    osc.start();

    activeNotes.set(noteIdx, { osc, gain });
    if (note.element) note.element.classList.add('pressed');

    document.getElementById('note-display').textContent = note.name;
    document.getElementById('freq-display').textContent = note.freq.toFixed(1) + ' Hz';
}

function noteOff(noteIdx) {
    const entry = activeNotes.get(noteIdx);
    if (!entry) return;

    entry.osc.stop();
    activeNotes.delete(noteIdx);

    const note = notes[noteIdx];
    if (note && note.element) note.element.classList.remove('pressed');

    if (activeNotes.size === 0) {
        document.getElementById('note-display').textContent = '--';
        document.getElementById('freq-display').textContent = '-- Hz';
    }
}

// ---------------------------------------------------------------------------
// Mic setup
// ---------------------------------------------------------------------------

async function initMic() {
    if (!audioCtx || micSourceNode) return;
    try {
        micStream = await navigator.mediaDevices.getUserMedia({ audio: true });
        micSourceNode = audioCtx.createMediaStreamSource(micStream);

        // Mic analyser for level metering and pitch detection (reads raw mic buffer)
        micAnalyser = audioCtx.createAnalyser();
        micAnalyser.fftSize = 2048;
        micAnalyser.smoothingTimeConstant = 0.8;
        micAnalyser.source = 1; // mic-only for level/pitch detection
        // Pre-allocate buffers for mic metering/pitch (avoid per-frame alloc)
        micLevelBuf = new Uint8Array(micAnalyser.frequencyBinCount);
        micFreqBuf = new Uint8Array(micAnalyser.frequencyBinCount);

        // Set initial mic state via engine-level controls
        audioCtx.micMuted = true; // start muted
        audioCtx.micMonitorGain = micVolume;
    } catch (err) {
        console.error('Mic access failed:', err);
    }
}

function setMicEnabled(enabled) {
    micEnabled = enabled;
    const btn = document.getElementById('mic-toggle');
    if (enabled) {
        btn.classList.remove('mic-off');
        btn.classList.add('mic-on');
    } else {
        btn.classList.remove('mic-on');
        btn.classList.add('mic-off');
    }
    // Control mute at the engine level (affects both playback and viz blend)
    if (audioCtx) audioCtx.micMuted = !enabled;
}

// ---------------------------------------------------------------------------
// Pitch detection (FFT peak — O(n), uses already-computed frequency data)
// ---------------------------------------------------------------------------

let micLevelBuf = null;
let micFreqBuf = null;

function detectPitch(analyserNode) {
    if (!analyserNode || !micFreqBuf) return null;

    analyserNode.getByteFrequencyData(micFreqBuf);

    // Find the bin with highest magnitude (skip bin 0 = DC)
    const sampleRate = audioCtx.sampleRate;
    const binCount = analyserNode.frequencyBinCount;
    const binWidth = sampleRate / analyserNode.fftSize;

    // Only search bins corresponding to 60–1500 Hz
    const minBin = Math.max(1, Math.floor(60 / binWidth));
    const maxBin = Math.min(binCount - 1, Math.ceil(1500 / binWidth));

    let bestBin = minBin;
    let bestVal = 0;
    for (let i = minBin; i <= maxBin; i++) {
        if (micFreqBuf[i] > bestVal) {
            bestVal = micFreqBuf[i];
            bestBin = i;
        }
    }

    if (bestVal < 20) return null; // too quiet

    // Parabolic interpolation for sub-bin accuracy
    const prev = bestBin > 0 ? micFreqBuf[bestBin - 1] : 0;
    const next = bestBin < binCount - 1 ? micFreqBuf[bestBin + 1] : 0;
    const denom = prev - 2 * bestVal + next;
    const offset = denom !== 0 ? 0.5 * (prev - next) / denom : 0;

    return (bestBin + offset) * binWidth;
}

function freqToNoteName(freq) {
    const midi = 12 * Math.log2(freq / 440) + 69;
    const noteIdx = Math.round(midi) % 12;
    const octave = Math.floor(Math.round(midi) / 12) - 1;
    const cents = Math.round((midi - Math.round(midi)) * 100);
    return {
        name: NOTE_NAMES[noteIdx < 0 ? noteIdx + 12 : noteIdx] + octave,
        cents: cents
    };
}

// ---------------------------------------------------------------------------
// Keyboard input
// ---------------------------------------------------------------------------

const keysDown = new Set();

document.documentElement.addEventListener('keydown', (e) => {
    if (e.repeat) return;
    const key = e.key.toLowerCase();
    if (key in KEY_MAP && !keysDown.has(key)) {
        keysDown.add(key);
        noteOn(KEY_MAP[key]);
    }
});

document.documentElement.addEventListener('keyup', (e) => {
    const key = e.key.toLowerCase();
    if (key in KEY_MAP) {
        keysDown.delete(key);
        noteOff(KEY_MAP[key]);
    }
});

// Mouse input on keyboard
let mouseDown = false;
let mouseNoteIdx = -1;

function handleKeyMouseDown(el) {
    const idx = el.getAttribute('data-note-idx');
    if (idx !== null) {
        mouseNoteIdx = parseInt(idx);
        noteOn(mouseNoteIdx);
    }
}

function handleKeyMouseEnter(el) {
    if (!mouseDown) return;
    const idx = el.getAttribute('data-note-idx');
    if (idx !== null) {
        const newIdx = parseInt(idx);
        if (newIdx !== mouseNoteIdx) {
            if (mouseNoteIdx >= 0) noteOff(mouseNoteIdx);
            mouseNoteIdx = newIdx;
            noteOn(mouseNoteIdx);
        }
    }
}

function attachKeyHandlers(el) {
    el.addEventListener('mousedown', () => {
        mouseDown = true;
        handleKeyMouseDown(el);
    });
    el.addEventListener('mousemove', () => {
        handleKeyMouseEnter(el);
    });
}
$$('.white-key').forEach(attachKeyHandlers);
$$('.black-key').forEach(attachKeyHandlers);

document.documentElement.addEventListener('mouseup', () => {
    if (mouseDown) {
        mouseDown = false;
        if (mouseNoteIdx >= 0) {
            noteOff(mouseNoteIdx);
            mouseNoteIdx = -1;
        }
    }
});

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

// Waveform buttons
$$('#wave-btns .btn').forEach(btn => {
    btn.addEventListener('click', () => {
        $$('#wave-btns .btn').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        waveform = btn.getAttribute('data-wave');
    });
});

// Viz mode buttons
$$('#viz-btns .btn').forEach(btn => {
    btn.addEventListener('click', () => {
        $$('#viz-btns .btn').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        vizMode = btn.getAttribute('data-viz');
        if (vizMode === 'spectrogram') spectrogramData = [];
    });
});

// Mic toggle
document.getElementById('mic-toggle').addEventListener('click', async () => {
    if (!micSourceNode) {
        await initMic();
        if (!micSourceNode) return; // failed
    }
    setMicEnabled(!micEnabled);
});

// Synth volume slider
document.getElementById('volume').addEventListener('input', (e) => {
    synthVolume = parseInt(e.target.value) / 100;
    activeNotes.forEach(entry => {
        entry.gain.gain.value = synthVolume;
    });
});

// Mic monitor volume slider
document.getElementById('mic-volume').addEventListener('input', (e) => {
    micVolume = parseInt(e.target.value) / 100;
    if (audioCtx) audioCtx.micMonitorGain = micVolume;
});

// ---------------------------------------------------------------------------
// Visualization
// ---------------------------------------------------------------------------

const canvas = document.getElementById('viz');
const ctx = canvas.getContext('2d');

let spectrogramData = [];
const SPECTROGRAM_HISTORY = 100;

function hslColor(value) {
    const hue = (1 - value) * 270;
    const sat = 80 + value * 20;
    const light = 15 + value * 55;
    return 'hsl(' + hue + ', ' + sat + '%, ' + light + '%)';
}

function getGradientColor(value) {
    const r = Math.floor(value * 255);
    const g = Math.floor((1 - value * 0.7) * 230);
    const b = Math.floor(200 + value * 55);
    return 'rgb(' + r + ', ' + g + ', ' + b + ')';
}

let frameCount = 0;
let lastFpsTime = performance.now();
let fps = 0;

function draw() {
    requestAnimationFrame(draw);

    frameCount++;
    const now = performance.now();
    if (now - lastFpsTime >= 1000) {
        fps = frameCount;
        frameCount = 0;
        lastFpsTime = now;
        document.getElementById('fps-display').textContent = fps + ' fps';
    }

    const W = ctx.canvasWidth;
    const H = ctx.canvasHeight;

    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#0a0a0f';
    ctx.fillRect(0, 0, W, H);

    // Update mic level indicator and pitch detection
    updateMicInfo();

    if (!analyser) return;

    if (vizMode === 'spectrum') {
        drawSpectrum(W, H);
    } else if (vizMode === 'wave') {
        drawWaveform(W, H);
    } else if (vizMode === 'spectrogram') {
        drawSpectrogram(W, H);
    }
}

// ---------------------------------------------------------------------------
// Mic info (level meter + pitch detection)
// ---------------------------------------------------------------------------

let micPitchCounter = 0;
const micLevelEl = document.getElementById('mic-level-bar');
const micNoteEl = document.getElementById('mic-note');
const micFreqEl = document.getElementById('mic-freq');

function updateMicInfo() {
    if (!micAnalyser || !micLevelBuf) return;

    // Throttle everything to every 6 frames
    micPitchCounter++;
    if (micPitchCounter % 6 !== 0) return;

    // Level meter — reuse pre-allocated buffer
    micAnalyser.getByteFrequencyData(micLevelBuf);
    let sum = 0;
    const len = micLevelBuf.length;
    // Sample every 4th bin for speed
    for (let i = 0; i < len; i += 4) sum += micLevelBuf[i];
    const avgLevel = sum / (len / 4) / 255;
    micLevelEl.style.height = Math.min(100, avgLevel * 300) + '%';

    if (avgLevel > 0.6) micLevelEl.style.background = '#cc3333';
    else if (avgLevel > 0.3) micLevelEl.style.background = '#cccc33';
    else micLevelEl.style.background = '#33cc33';

    if (!micEnabled) {
        micNoteEl.textContent = 'Mic: --';
        micFreqEl.textContent = '-- Hz';
        return;
    }

    // Pitch detection reuses the same FFT data (detectPitch calls getByteFrequencyData)
    const freq = detectPitch(micAnalyser);
    if (freq && freq > 50 && freq < 2000) {
        const noteInfo = freqToNoteName(freq);
        const centsStr = noteInfo.cents >= 0 ? '+' + noteInfo.cents : '' + noteInfo.cents;
        micNoteEl.textContent = 'Mic: ' + noteInfo.name + ' (' + centsStr + 'c)';
        micFreqEl.textContent = freq.toFixed(1) + ' Hz';
    } else {
        micNoteEl.textContent = 'Mic: --';
        micFreqEl.textContent = '-- Hz';
    }
}

// ---------------------------------------------------------------------------
// Draw functions
// ---------------------------------------------------------------------------

function drawSpectrum(W, H) {
    const bufLen = analyser.frequencyBinCount;
    const data = new Uint8Array(bufLen);
    analyser.getByteFrequencyData(data);

    const numBars = Math.min(128, bufLen);
    const barWidth = W / numBars;
    const padding = 1;

    for (let i = 0; i < numBars; i++) {
        const logMin = Math.log(1);
        const logMax = Math.log(bufLen);
        const startBin = Math.floor(Math.exp(logMin + (logMax - logMin) * i / numBars));
        const endBin = Math.floor(Math.exp(logMin + (logMax - logMin) * (i + 1) / numBars));

        let sum = 0;
        let count = 0;
        for (let j = startBin; j < endBin && j < bufLen; j++) {
            sum += data[j];
            count++;
        }
        const value = count > 0 ? sum / count / 255 : 0;

        const barH = value * H * 0.9;
        const x = i * barWidth + padding;
        const w = barWidth - padding * 2;

        ctx.fillStyle = getGradientColor(value);
        ctx.fillRect(x, H - barH, w, barH);

        if (value > 0.3) {
            ctx.fillStyle = 'rgba(0, 229, 255, ' + (value * 0.3) + ')';
            ctx.fillRect(x, H - barH - 2, w, 4);
        }
    }

    // Subtle grid lines
    ctx.strokeStyle = '#1a1a2a';
    ctx.lineWidth = 1;
    for (let y = 0; y < H; y += H / 8) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(W, y);
        ctx.stroke();
    }
}

function drawWaveform(W, H) {
    const bufLen = analyser.fftSize;
    const data = new Float32Array(bufLen);
    analyser.getFloatTimeDomainData(data);

    const midY = H / 2;

    // Center line
    ctx.strokeStyle = '#1a1a2a';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, midY);
    ctx.lineTo(W, midY);
    ctx.stroke();

    // Glow effect
    for (let pass = 0; pass < 2; pass++) {
        ctx.strokeStyle = pass === 0 ? 'rgba(0, 229, 255, 0.3)' : '#00e5ff';
        ctx.lineWidth = pass === 0 ? 6 : 2;
        ctx.beginPath();

        for (let i = 0; i < bufLen; i++) {
            const x = (i / bufLen) * W;
            const y = midY + data[i] * midY * 0.8;
            if (i === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        }
        ctx.stroke();
    }
}

function drawSpectrogram(W, H) {
    const bufLen = analyser.frequencyBinCount;
    const data = new Uint8Array(bufLen);
    analyser.getByteFrequencyData(data);

    const numFreqBins = 64;
    const reduced = new Uint8Array(numFreqBins);
    for (let f = 0; f < numFreqBins; f++) {
        const logMin = Math.log(1);
        const logMax = Math.log(bufLen);
        const startBin = Math.floor(Math.exp(logMin + (logMax - logMin) * f / numFreqBins));
        const endBin = Math.floor(Math.exp(logMin + (logMax - logMin) * (f + 1) / numFreqBins));
        let sum = 0, count = 0;
        for (let j = startBin; j < endBin && j < bufLen; j++) { sum += data[j]; count++; }
        reduced[f] = count > 0 ? Math.floor(sum / count) : 0;
    }

    spectrogramData.push(reduced);
    if (spectrogramData.length > SPECTROGRAM_HISTORY) {
        spectrogramData.shift();
    }

    const colW = W / SPECTROGRAM_HISTORY;
    const binH = H / numFreqBins;

    for (let t = 0; t < spectrogramData.length; t++) {
        const col = spectrogramData[t];
        const x = t * colW;

        for (let f = 0; f < numFreqBins; f++) {
            const value = col[f] / 255;
            if (value < 0.02) continue;

            const y = H - (f + 1) * binH;
            ctx.fillStyle = hslColor(value);
            ctx.fillRect(x, y, colW + 1, binH + 1);
        }
    }
}

// Start render loop
draw();
