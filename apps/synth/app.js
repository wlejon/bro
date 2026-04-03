// ---------------------------------------------------------------------------
// Audio Synth + Visualizer
// ---------------------------------------------------------------------------

let audioCtx, analyser;
try {
    audioCtx = new AudioContext();
    analyser = audioCtx.createAnalyser();
    analyser.fftSize = 2048;
    analyser.smoothingTimeConstant = 0.85;
} catch (e) {
    console.warn('AudioContext unavailable:', e.message);
}

// State
let waveform = 'sine';
let volume = 0.3;
let vizMode = 'spectrum';
let source = 'synth'; // 'synth' or 'mic'
let micStream = null;
let micSource = null;
let activeNotes = new Map(); // note -> { osc, gain }

// Note definitions — 3 octaves starting at C3
const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
const BASE_OCTAVE = 3;
const NUM_OCTAVES = 3;
const notes = [];

for (let oct = BASE_OCTAVE; oct < BASE_OCTAVE + NUM_OCTAVES; oct++) {
    for (let i = 0; i < 12; i++) {
        const name = NOTE_NAMES[i] + oct;
        const midi = (oct + 1) * 12 + i; // MIDI note number
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

// Create white keys first
whiteKeys.forEach((note, i) => {
    const el = document.createElement('div');
    el.className = 'white-key';
    el.setAttribute('data-note-idx', notes.indexOf(note).toString());

    // Key binding label
    const binding = Object.entries(KEY_MAP).find(([,v]) => v === notes.indexOf(note));
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
    // Find which white key this black key sits between
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
    gain.gain.value = volume;

    osc.connect(gain).connect(audioCtx.destination);
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
    if (note.element) note.element.classList.remove('pressed');

    if (activeNotes.size === 0) {
        document.getElementById('note-display').textContent = '--';
        document.getElementById('freq-display').textContent = '-- Hz';
    }
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

keyboard.addEventListener('mousedown', (e) => {
    mouseDown = true;
    const idx = e.target.getAttribute('data-note-idx');
    if (idx !== null) noteOn(parseInt(idx));
});

document.documentElement.addEventListener('mouseup', () => {
    if (mouseDown) {
        mouseDown = false;
        activeNotes.forEach((_, idx) => noteOff(idx));
    }
});

keyboard.addEventListener('mousemove', (e) => {
    if (!mouseDown) return;
    const idx = e.target.getAttribute('data-note-idx');
    if (idx !== null) {
        const noteIdx = parseInt(idx);
        if (!activeNotes.has(noteIdx)) {
            activeNotes.forEach((_, i) => noteOff(i));
            noteOn(noteIdx);
        }
    }
});

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

// Waveform buttons
document.getElementById('wave-btns').addEventListener('click', (e) => {
    if (!e.target.getAttribute('data-wave')) return;
    document.querySelectorAll('#wave-btns .btn').forEach(b => b.classList.remove('active'));
    e.target.classList.add('active');
    waveform = e.target.getAttribute('data-wave');
});

// Viz mode buttons
document.getElementById('viz-btns').addEventListener('click', (e) => {
    if (!e.target.getAttribute('data-viz')) return;
    document.querySelectorAll('#viz-btns .btn').forEach(b => b.classList.remove('active'));
    e.target.classList.add('active');
    vizMode = e.target.getAttribute('data-viz');
    // Clear spectrogram when switching
    if (vizMode === 'spectrogram') spectrogramData = [];
});

// Source buttons
document.getElementById('source-btns').addEventListener('click', async (e) => {
    if (!e.target.getAttribute('data-source')) return;
    document.querySelectorAll('#source-btns .btn').forEach(b => b.classList.remove('active'));
    e.target.classList.add('active');
    source = e.target.getAttribute('data-source');

    if (source === 'mic' && !micStream) {
        try {
            micStream = await navigator.mediaDevices.getUserMedia({ audio: true });
            micSource = audioCtx.createMediaStreamSource(micStream);
            // Connect mic to analyser (switches analyser to mic input)
            micSource.connect(analyser);
        } catch (err) {
            console.error('Mic access failed:', err);
            source = 'synth';
            document.querySelector('[data-source="synth"]').classList.add('active');
            e.target.classList.remove('active');
        }
    } else if (source === 'synth') {
        // Switch analyser back to output
        analyser.fftSize = analyser.fftSize; // Trigger re-creation — source is set via connect()
    }
});

// Volume slider
document.getElementById('volume').addEventListener('input', (e) => {
    volume = parseInt(e.target.value) / 100;
    // Update active notes
    activeNotes.forEach(entry => {
        entry.gain.gain.value = volume;
    });
});

// ---------------------------------------------------------------------------
// Visualization
// ---------------------------------------------------------------------------

const canvas = document.getElementById('viz');
const ctx = canvas.getContext('2d');

let spectrogramData = [];
const SPECTROGRAM_HISTORY = 200;

// Color palette for spectrum
function hslColor(value) {
    // Map 0-1 to hue range (270 blue -> 0 red)
    const hue = (1 - value) * 270;
    const sat = 80 + value * 20;
    const light = 15 + value * 55;
    return `hsl(${hue}, ${sat}%, ${light}%)`;
}

function getGradientColor(value) {
    // Cyan to magenta gradient
    const r = Math.floor(value * 255);
    const g = Math.floor((1 - value * 0.7) * 230);
    const b = Math.floor(200 + value * 55);
    return `rgb(${r}, ${g}, ${b})`;
}

let frameCount = 0;
let lastFpsTime = performance.now();
let fps = 0;

function draw() {
    requestAnimationFrame(draw);

    // FPS counter
    frameCount++;
    const now = performance.now();
    if (now - lastFpsTime >= 1000) {
        fps = frameCount;
        frameCount = 0;
        lastFpsTime = now;
        document.getElementById('fps-display').textContent = fps + ' fps';
    }

    const W = canvas.width;
    const H = canvas.height;

    ctx.fillStyle = '#0a0a0f';
    ctx.fillRect(0, 0, W, H);

    if (!analyser) return;

    if (vizMode === 'spectrum') {
        drawSpectrum(W, H);
    } else if (vizMode === 'wave') {
        drawWaveform(W, H);
    } else if (vizMode === 'spectrogram') {
        drawSpectrogram(W, H);
    }
}

function drawSpectrum(W, H) {
    const bufLen = analyser.frequencyBinCount;
    const data = new Uint8Array(bufLen);
    analyser.getByteFrequencyData(data);

    // Use logarithmic frequency scale — group bins into visual bars
    const numBars = Math.min(128, bufLen);
    const barWidth = W / numBars;
    const padding = 1;

    for (let i = 0; i < numBars; i++) {
        // Logarithmic bin mapping
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

        // Gradient fill
        ctx.fillStyle = getGradientColor(value);
        ctx.fillRect(x, H - barH, w, barH);

        // Glow on top
        if (value > 0.3) {
            ctx.fillStyle = `rgba(0, 229, 255, ${value * 0.3})`;
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

    // Background grid
    ctx.strokeStyle = '#1a1a2a';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, midY);
    ctx.lineTo(W, midY);
    ctx.stroke();

    // Glow effect — draw twice with different widths
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

    // Add current frame
    spectrogramData.push(new Uint8Array(data));
    if (spectrogramData.length > SPECTROGRAM_HISTORY) {
        spectrogramData.shift();
    }

    // Draw spectrogram — time flows left to right, frequency bottom to top
    const colW = W / SPECTROGRAM_HISTORY;
    const numFreqBins = Math.min(256, bufLen);

    for (let t = 0; t < spectrogramData.length; t++) {
        const col = spectrogramData[t];
        const x = t * colW;

        for (let f = 0; f < numFreqBins; f++) {
            // Log frequency mapping
            const logIdx = Math.floor(Math.exp(Math.log(1) + (Math.log(bufLen) - Math.log(1)) * f / numFreqBins));
            const value = col[Math.min(logIdx, bufLen - 1)] / 255;
            if (value < 0.02) continue; // Skip near-silence for perf

            const binH = H / numFreqBins;
            const y = H - (f + 1) * binH;

            ctx.fillStyle = hslColor(value);
            ctx.fillRect(x, y, colW + 1, binH + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Canvas resize
// ---------------------------------------------------------------------------

function resizeCanvas() {
    const rect = canvas.getBoundingClientRect();
    canvas.width = rect.width;
    canvas.height = rect.height;
}

// Initial size and start
resizeCanvas();
draw();
