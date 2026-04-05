(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var audioCtx = null;
    var analyser = null;
    var masterGain = null;
    var allocator = null;
    var modMatrix = null;
    var activeNotes = new Map(); // noteIdx -> { clipPlaybackId } (clip mode only)

    var waveform = 'sine';
    var synthVolume = 0.5;
    var octaveShift = 0;
    var lastPlayedNote = 24; // C3
    var adsrParams = { attack: 0.01, decay: 0.1, sustain: 1.0, release: 0.04 };

    Synth.init = function() {
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
            audioCtx.masterGain = synthVolume;

            // Create voice allocator for polyphonic synth
            allocator = audioCtx.createVoiceAllocator(16);
            allocator.setStealPolicy('oldest');
            updateVoiceSetup();

            // Init LFO with mod matrix
            Synth.LFO.init(audioCtx);
        } catch (e) {
            console.warn('AudioContext unavailable:', e.message);
        }
        return audioCtx;
    };

    // Configure the voice allocator callback with current synth params.
    // Called on init and whenever waveform/ADSR/pan change.
    function updateVoiceSetup() {
        if (!allocator || !audioCtx) return;
        allocator.setVoiceSetup(function(voiceId, note, velocity) {
            var freq = 440 * Math.pow(2, (note - 69) / 12);
            audioCtx.setVoiceNote(voiceId, note, velocity);
            audioCtx.setVoiceWaveform(voiceId, waveform);
            audioCtx.setVoiceFrequency(voiceId, freq);
            audioCtx.setVoiceGain(voiceId, 3.0);
            audioCtx.setVoicePan(voiceId, Synth.voicePan || 0);
            audioCtx.setVoiceAttack(voiceId, adsrParams.attack);
            audioCtx.setVoiceDecay(voiceId, adsrParams.decay);
            audioCtx.setVoiceSustain(voiceId, adsrParams.sustain);
            audioCtx.setVoiceRelease(voiceId, adsrParams.release);
        });
    }

    // Base note for clip instrument (C4 = middle C, noteIdx 36 in our 7-octave range)
    var CLIP_BASE_NOTE = 36;

    // silent: if true, skip piano key highlight, lastPlayedNote, and status bar updates
    Synth.noteOn = function(noteIdx, silent) {
        if (!audioCtx) return;
        var notes = Synth.notes;
        if (noteIdx < 0 || noteIdx >= notes.length) return;
        if (activeNotes.has(noteIdx)) return;

        var note = notes[noteIdx];

        if (Synth.useClipMode && Synth.customClipId >= 0) {
            var semitoneOffset = noteIdx - CLIP_BASE_NOTE;
            var rate = Math.pow(2, semitoneOffset / 12);
            var pbId = audioCtx.playClip(Synth.customClipId, 1.0, false);
            if (pbId >= 0) {
                audioCtx.setPlaybackRate(pbId, rate);
                if (Synth.voicePan) audioCtx.setPlaybackPan(pbId, Synth.voicePan);
                activeNotes.set(noteIdx, { clipPlaybackId: pbId });
            }
        } else {
            allocator.noteOn(note.midi, 1.0, audioCtx.currentTime);
            activeNotes.set(noteIdx, { midi: note.midi });
        }

        if (!silent) {
            if (note.element) note.element.classList.add('pressed');
            lastPlayedNote = noteIdx;
            document.getElementById('note-display').textContent = note.name;
            document.getElementById('freq-display').textContent = note.freq.toFixed(1) + ' Hz';
        }
    };

    Synth.noteOff = function(noteIdx, silent) {
        var entry = activeNotes.get(noteIdx);
        if (!entry) return;

        if (entry.clipPlaybackId !== undefined) {
            audioCtx.stopPlayback(entry.clipPlaybackId);
        } else if (entry.midi !== undefined) {
            allocator.noteOff(entry.midi, audioCtx.currentTime);
        }
        activeNotes.delete(noteIdx);

        if (!silent) {
            var note = Synth.notes[noteIdx];
            if (note && note.element) note.element.classList.remove('pressed');
        }

        if (activeNotes.size === 0 && !silent) {
            document.getElementById('note-display').textContent = '--';
            document.getElementById('freq-display').textContent = '-- Hz';
        }
    };

    Synth.setWaveform = function(wf) { waveform = wf; updateVoiceSetup(); };
    Synth.getWaveform = function() { return waveform; };

    Synth.setVolume = function(v) {
        synthVolume = v;
        if (audioCtx) audioCtx.masterGain = v;
    };
    Synth.getVolume = function() { return synthVolume; };

    Synth.setADSR = function(a, d, s, r) {
        adsrParams.attack = a;
        adsrParams.decay = d;
        adsrParams.sustain = s;
        adsrParams.release = r;
        updateVoiceSetup();
    };
    Synth.getADSR = function() {
        return { attack: adsrParams.attack, decay: adsrParams.decay,
                 sustain: adsrParams.sustain, release: adsrParams.release };
    };

    Synth.voicePan = 0;
    Synth.setPan = function(p) { Synth.voicePan = Math.max(-1, Math.min(1, p)); updateVoiceSetup(); };
    Synth.getPan = function() { return Synth.voicePan; };

    Synth.getLastPlayedNote = function() { return lastPlayedNote; };
    Synth.getAudioContext = function() { return audioCtx; };
    Synth.getAnalyser = function() { return analyser; };
    Synth.getMasterGain = function() { return masterGain; };
    Synth.getActiveNotes = function() { return activeNotes; };
    Synth.getAllocator = function() { return allocator; };

    // Note definitions -- 7 octaves (C1-B7)
    var NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
    var BASE_OCTAVE = 1;
    var NUM_OCTAVES = 7;
    var notes = [];

    for (var oct = BASE_OCTAVE; oct < BASE_OCTAVE + NUM_OCTAVES; oct++) {
        for (var i = 0; i < 12; i++) {
            var name = NOTE_NAMES[i] + oct;
            var midi = (oct + 1) * 12 + i;
            var freq = 440 * Math.pow(2, (midi - 69) / 12);
            var isBlack = [1,3,6,8,10].indexOf(i) >= 0;
            notes.push({ name: name, freq: freq, midi: midi, isBlack: isBlack,
                         octave: oct, noteIndex: i, element: null });
        }
    }

    Synth.notes = notes;
    Synth.NOTE_NAMES = NOTE_NAMES;

    // Clip instrument mode state
    Synth.useClipMode = false;
    Synth.customClipId = -1;
    Synth.customClipSamples = null;

    // Mic
    var micEnabled = false;
    var micSourceNode = null;
    var micAnalyser = null;
    var micVolume = 0.5;
    var micLevelBuf = null;
    var micFreqBuf = null;

    Synth.initMic = async function() {
        if (!audioCtx || micSourceNode) return;
        try {
            await navigator.mediaDevices.getUserMedia({ audio: true });
            micSourceNode = audioCtx.createMediaStreamSource(
                await navigator.mediaDevices.getUserMedia({ audio: true }));

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
    };

    Synth.setMicEnabled = function(enabled) {
        micEnabled = enabled;
        if (audioCtx) audioCtx.micMuted = !enabled;
    };
    Synth.isMicEnabled = function() { return micEnabled; };
    Synth.hasMic = function() { return !!micSourceNode; };

    Synth.setMicVolume = function(v) {
        micVolume = v;
        if (audioCtx) audioCtx.micMonitorGain = v;
    };

    Synth.getMicAnalyser = function() { return micAnalyser; };
    Synth.getMicLevelBuf = function() { return micLevelBuf; };
    Synth.getMicFreqBuf = function() { return micFreqBuf; };

    Synth.detectPitch = function() {
        if (!micAnalyser || !micFreqBuf) return null;
        micAnalyser.getByteFrequencyData(micFreqBuf);

        var sampleRate = audioCtx.sampleRate;
        var binCount = micAnalyser.frequencyBinCount;
        var binWidth = sampleRate / micAnalyser.fftSize;

        var minBin = Math.max(1, Math.floor(60 / binWidth));
        var maxBin = Math.min(binCount - 1, Math.ceil(1500 / binWidth));

        var bestBin = minBin, bestVal = 0;
        for (var i = minBin; i <= maxBin; i++) {
            if (micFreqBuf[i] > bestVal) { bestVal = micFreqBuf[i]; bestBin = i; }
        }
        if (bestVal < 20) return null;

        var prev = bestBin > 0 ? micFreqBuf[bestBin - 1] : 0;
        var next = bestBin < binCount - 1 ? micFreqBuf[bestBin + 1] : 0;
        var denom = prev - 2 * bestVal + next;
        var offset = denom !== 0 ? 0.5 * (prev - next) / denom : 0;
        return (bestBin + offset) * binWidth;
    };

    Synth.freqToNoteName = function(freq) {
        var midi = 12 * Math.log2(freq / 440) + 69;
        var noteIdx = Math.round(midi) % 12;
        var octave = Math.floor(Math.round(midi) / 12) - 1;
        var cents = Math.round((midi - Math.round(midi)) * 100);
        return {
            name: NOTE_NAMES[noteIdx < 0 ? noteIdx + 12 : noteIdx] + octave,
            cents: cents
        };
    };
})();
