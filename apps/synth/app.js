// ---------------------------------------------------------------------------
// Audio Synth + Visualizer
// ---------------------------------------------------------------------------

function $$(sel) { return Array.from(document.querySelectorAll(sel)); }

let audioCtx, analyser, masterGain;
try {
    audioCtx = new AudioContext();
    analyser = audioCtx.createAnalyser();
    analyser.fftSize = 2048;
    analyser.smoothingTimeConstant = 0.85;
    masterGain = audioCtx.createGain();
    masterGain.gain.value = 1.0;
    masterGain.connect(analyser);
    analyser.source = 2;
    analyser.connect(audioCtx.destination);
} catch (e) {
    console.warn('AudioContext unavailable:', e.message);
}

// State
let waveform = 'sine';
let synthVolume = 0.3;
let micVolume = 0.5;
let micEnabled = false;
let micStream = null;
let micSourceNode = null;
let micAnalyser = null;
let activeNotes = new Map(); // noteIdx -> { osc, gain }

// Note definitions -- 7 octaves (C1-B7) for full range
const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
const BASE_OCTAVE = 1;
const NUM_OCTAVES = 7;
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

// Keyboard mapping -- home row layout (relative to current view)
// White keys on home row: a s d f g h j k l ;
// Black keys on row above: w e   t y u   o p
//
// Maps key -> relative semitone offset within the current view
const KEY_MAP = {
    'a': 0,   // C
    'w': 1,   // C#
    's': 2,   // D
    'e': 3,   // D#
    'd': 4,   // E
    'f': 5,   // F
    't': 6,   // F#
    'g': 7,   // G
    'y': 8,   // G#
    'h': 9,   // A
    'u': 10,  // A#
    'j': 11,  // B
    'k': 12,  // C (next octave)
    'o': 13,  // C#
    'l': 14,  // D
    'p': 15,  // D#
    ';': 16,  // E
};

// View offset: which note index the view starts at
// Default to C3 (index 24 in our 7-octave array starting at C1)
const VIEW_NOTES = 17; // C through E of next octave
let viewOffset = 24;   // C3
const MIN_VIEW = 0;
const MAX_VIEW = notes.length - VIEW_NOTES;

// ---------------------------------------------------------------------------
// Build piano keyboard UI
// ---------------------------------------------------------------------------

const keyboard = document.getElementById('keyboard');
const octaveDisplay = document.getElementById('octave-display');

function buildKeyboard() {
    // Clear existing keys and element refs
    keyboard.innerHTML = '';
    for (let i = 0; i < notes.length; i++) {
        notes[i].element = null;
    }

    // Slice the visible notes
    const viewNotes = notes.slice(viewOffset, viewOffset + VIEW_NOTES);
    const visWhite = viewNotes.filter(function(n) { return !n.isBlack; });
    const visBlack = viewNotes.filter(function(n) { return n.isBlack; });

    // Create white keys
    visWhite.forEach(function(note) {
        const el = document.createElement('div');
        el.className = 'white-key';
        const noteIdx = notes.indexOf(note);
        const relIdx = noteIdx - viewOffset;
        el.setAttribute('data-note-idx', noteIdx.toString());

        if (activeNotes.has(noteIdx)) el.classList.add('pressed');

        const binding = Object.entries(KEY_MAP).find(function(entry) { return entry[1] === relIdx; });
        const label = document.createElement('div');
        label.className = 'key-label';
        label.textContent = binding ? binding[0].toUpperCase() : '';
        el.appendChild(label);

        // Note name label
        const nameLabel = document.createElement('div');
        nameLabel.className = 'key-note-label';
        nameLabel.textContent = note.name;
        el.appendChild(nameLabel);

        keyboard.appendChild(el);
        note.element = el;
    });

    // Create black keys positioned over white keys
    visBlack.forEach(function(note) {
        const noteIdx = notes.indexOf(note);
        const relIdx = noteIdx - viewOffset;
        const whiteIdx = visWhite.filter(function(w) { return notes.indexOf(w) < noteIdx; }).length;
        const whiteKeyWidth = 100 / visWhite.length;
        const leftPos = whiteIdx * whiteKeyWidth - whiteKeyWidth * 0.3;

        const el = document.createElement('div');
        el.className = 'black-key';
        el.setAttribute('data-note-idx', noteIdx.toString());
        el.style.left = leftPos + '%';
        el.style.width = (whiteKeyWidth * 0.6) + '%';

        if (activeNotes.has(noteIdx)) el.classList.add('pressed');

        const binding = Object.entries(KEY_MAP).find(function(entry) { return entry[1] === relIdx; });
        const label = document.createElement('div');
        label.className = 'key-label';
        label.textContent = binding ? binding[0].toUpperCase() : '';
        el.appendChild(label);

        keyboard.appendChild(el);
        note.element = el;
    });

    // Attach mouse handlers to new keys
    $$('.white-key').forEach(attachKeyHandlers);
    $$('.black-key').forEach(attachKeyHandlers);

    // Update octave display
    var startNote = notes[viewOffset];
    octaveDisplay.textContent = startNote.name;
}

function shiftView(semitones) {
    // Release any keys currently held via keyboard before shifting
    keysDown.forEach(function(key) {
        if (key in KEY_MAP) {
            noteOff(KEY_MAP[key] + viewOffset);
        }
    });
    keysDown.clear();

    var newOffset = viewOffset + semitones;
    if (newOffset < MIN_VIEW) newOffset = MIN_VIEW;
    if (newOffset > MAX_VIEW) newOffset = MAX_VIEW;
    if (newOffset !== viewOffset) {
        viewOffset = newOffset;
        buildKeyboard();
    }
}

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

        micAnalyser = audioCtx.createAnalyser();
        micAnalyser.fftSize = 2048;
        micAnalyser.smoothingTimeConstant = 0.8;
        micAnalyser.source = 1;
        micLevelBuf = new Uint8Array(micAnalyser.frequencyBinCount);
        micFreqBuf = new Uint8Array(micAnalyser.frequencyBinCount);

        audioCtx.micMuted = true;
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
    if (audioCtx) audioCtx.micMuted = !enabled;
}

// ---------------------------------------------------------------------------
// Pitch detection (FFT peak)
// ---------------------------------------------------------------------------

let micLevelBuf = null;
let micFreqBuf = null;

function detectPitch(analyserNode) {
    if (!analyserNode || !micFreqBuf) return null;

    analyserNode.getByteFrequencyData(micFreqBuf);

    const sampleRate = audioCtx.sampleRate;
    const binCount = analyserNode.frequencyBinCount;
    const binWidth = sampleRate / analyserNode.fftSize;

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

    if (bestVal < 20) return null;

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

document.documentElement.addEventListener('keydown', function(e) {
    if (e.repeat) return;
    const key = e.key.toLowerCase();

    // Tab / Shift+Tab to shift view by one octave
    if (e.key === 'Tab') {
        e.preventDefault();
        shiftView(e.shiftKey ? -12 : 12);
        return;
    }

    if (key in KEY_MAP && !keysDown.has(key)) {
        keysDown.add(key);
        noteOn(KEY_MAP[key] + viewOffset);
    }
});

document.documentElement.addEventListener('keyup', function(e) {
    const key = e.key.toLowerCase();
    if (key in KEY_MAP) {
        keysDown.delete(key);
        noteOff(KEY_MAP[key] + viewOffset);
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
    el.addEventListener('mousedown', function() {
        mouseDown = true;
        handleKeyMouseDown(el);
    });
    el.addEventListener('mousemove', function() {
        handleKeyMouseEnter(el);
    });
}

document.documentElement.addEventListener('mouseup', function() {
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
$$('#wave-btns .btn').forEach(function(btn) {
    btn.addEventListener('click', function() {
        $$('#wave-btns .btn').forEach(function(b) { b.classList.remove('active'); });
        btn.classList.add('active');
        waveform = btn.getAttribute('data-wave');
    });
});

// Mic toggle
document.getElementById('mic-toggle').addEventListener('click', async function() {
    if (!micSourceNode) {
        await initMic();
        if (!micSourceNode) return;
    }
    setMicEnabled(!micEnabled);
});

// Synth volume slider
document.getElementById('volume').addEventListener('input', function(e) {
    synthVolume = parseInt(e.target.value) / 100;
    activeNotes.forEach(function(entry) {
        entry.gain.gain.value = synthVolume;
    });
});

// Mic monitor volume slider
document.getElementById('mic-volume').addEventListener('input', function(e) {
    micVolume = parseInt(e.target.value) / 100;
    if (audioCtx) audioCtx.micMonitorGain = micVolume;
});

// ---------------------------------------------------------------------------
// Visualization -- waveform oscilloscope
// ---------------------------------------------------------------------------

const canvas = document.getElementById('viz');
const ctx = canvas.getContext('2d');

// Pre-allocate analysis buffers
let waveData = null;

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

    // Dark background
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#0a0a0f';
    ctx.fillRect(0, 0, W, H);

    updateMicInfo();

    if (!analyser) return;

    // Allocate buffer on first use or if fftSize changed
    if (!waveData || waveData.length !== analyser.fftSize) {
        waveData = new Float32Array(analyser.fftSize);
    }
    analyser.getFloatTimeDomainData(waveData);

    drawWaveform(W, H, waveData);
}

function drawWaveform(W, H, data) {
    const bufLen = data.length;
    const midY = H / 2;

    // Subtle grid
    ctx.strokeStyle = '#141420';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, midY);
    ctx.lineTo(W, midY);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(0, midY * 0.5);
    ctx.lineTo(W, midY * 0.5);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(0, midY * 1.5);
    ctx.lineTo(W, midY * 1.5);
    ctx.stroke();

    // Compute RMS for adaptive glow intensity
    let rms = 0;
    for (let i = 0; i < bufLen; i++) rms += data[i] * data[i];
    rms = Math.sqrt(rms / bufLen);
    const intensity = Math.min(1.0, rms * 4);

    // Zero-crossing trigger to stabilize display
    let triggerOffset = 0;
    const searchEnd = Math.floor(bufLen / 4);
    for (let i = 1; i < searchEnd; i++) {
        if (data[i - 1] <= 0 && data[i] > 0) {
            triggerOffset = i;
            break;
        }
    }

    const drawLen = Math.min(bufLen - triggerOffset, Math.floor(bufLen * 0.75));

    // Pass 1: wide glow
    if (intensity > 0.01) {
        ctx.strokeStyle = 'rgba(0, 229, 255, ' + (intensity * 0.15) + ')';
        ctx.lineWidth = 10;
        ctx.beginPath();
        for (let i = 0; i < drawLen; i++) {
            const x = (i / drawLen) * W;
            const y = midY + data[triggerOffset + i] * midY * 0.85;
            if (i === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        }
        ctx.stroke();
    }

    // Pass 2: medium glow
    if (intensity > 0.01) {
        ctx.strokeStyle = 'rgba(0, 229, 255, ' + (intensity * 0.3) + ')';
        ctx.lineWidth = 4;
        ctx.beginPath();
        for (let i = 0; i < drawLen; i++) {
            const x = (i / drawLen) * W;
            const y = midY + data[triggerOffset + i] * midY * 0.85;
            if (i === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        }
        ctx.stroke();
    }

    // Pass 3: crisp line
    const r = Math.floor(intensity * 80);
    const g = Math.floor(200 + intensity * 55);
    const b = 255;
    ctx.strokeStyle = 'rgb(' + r + ', ' + g + ', ' + b + ')';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let i = 0; i < drawLen; i++) {
        const x = (i / drawLen) * W;
        const y = midY + data[triggerOffset + i] * midY * 0.85;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
    }
    ctx.stroke();

    // Pass 4: bright peak dots at extremes
    if (intensity > 0.05) {
        ctx.fillStyle = 'rgba(255, 255, 255, ' + (intensity * 0.4) + ')';
        let prevY = midY;
        let prevDy = 0;
        for (let i = 0; i < drawLen; i++) {
            const y = midY + data[triggerOffset + i] * midY * 0.85;
            const dy = y - prevY;
            if (prevDy > 0 && dy <= 0 && Math.abs(prevY - midY) > H * 0.15) {
                const x = ((i - 1) / drawLen) * W;
                ctx.beginPath();
                ctx.arc(x, prevY, 2, 0, 6.283);
                ctx.fill();
            } else if (prevDy < 0 && dy >= 0 && Math.abs(prevY - midY) > H * 0.15) {
                const x = ((i - 1) / drawLen) * W;
                ctx.beginPath();
                ctx.arc(x, prevY, 2, 0, 6.283);
                ctx.fill();
            }
            prevDy = dy;
            prevY = y;
        }
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

    micPitchCounter++;
    if (micPitchCounter % 6 !== 0) return;

    micAnalyser.getByteFrequencyData(micLevelBuf);
    let sum = 0;
    const len = micLevelBuf.length;
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
// Init
// ---------------------------------------------------------------------------

buildKeyboard();
draw();
