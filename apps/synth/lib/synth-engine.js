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
            allocator.setVoiceSetup(function(voiceId, note, velocity) {
                audioCtx.setVoiceNote(voiceId, note, velocity);
            });

            // Init LFO with mod matrix
            Synth.LFO.init(audioCtx);
        } catch (e) {
            console.warn('AudioContext unavailable:', e.message);
        }
        return audioCtx;
    };

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
            // Clip instrument mode: play clip at pitch-shifted rate
            var semitoneOffset = noteIdx - CLIP_BASE_NOTE;
            var rate = Math.pow(2, semitoneOffset / 12);
            var pbId = audioCtx.playClip(Synth.customClipId, 1.0, false);
            if (pbId >= 0) {
                audioCtx.setPlaybackRate(pbId, rate);
                if (Synth.voicePan) audioCtx.setPlaybackPan(pbId, Synth.voicePan);
                activeNotes.set(noteIdx, { clipPlaybackId: pbId });
            }
        } else {
            // Oscillator mode via VoiceAllocator
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

    // Configure the voice allocator's setup callback for current synth state
    Synth.applyVoiceConfig = function() {
        if (!allocator) return;
        var wf = waveform;
        var adsr = { attack: adsrParams.attack, decay: adsrParams.decay,
                     sustain: adsrParams.sustain, release: adsrParams.release };
        var pan = Synth.voicePan || 0;

        allocator.setVoiceSetup(function(voiceId, note, velocity) {
            audioCtx.setVoiceNote(voiceId, note, velocity);
            var osc = audioCtx.createOscillator();
            // We don't use the OscillatorNode directly — configure the voice via engine
            // The allocator already created the voice; we just need to set params
            // Note: voiceId is the engine voice ID, configure it directly
        });
    };

    Synth.setWaveform = function(wf) { waveform = wf; };
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
    };
    Synth.getADSR = function() {
        return { attack: adsrParams.attack, decay: adsrParams.decay,
                 sustain: adsrParams.sustain, release: adsrParams.release };
    };

    Synth.voicePan = 0;
    Synth.setPan = function(p) { Synth.voicePan = Math.max(-1, Math.min(1, p)); };
    Synth.getPan = function() { return Synth.voicePan; };

    Synth.getLastPlayedNote = function() { return lastPlayedNote; };
    Synth.getAudioContext = function() { return audioCtx; };
    Synth.getAnalyser = function() { return analyser; };
    Synth.getMasterGain = function() { return masterGain; };
    Synth.getActiveNotes = function() { return activeNotes; };
    Synth.getAllocator = function() { return allocator; };

    // Configure voice allocator callback with layer-specific params
    Synth.configureAllocator = function(wf, pan, adsr) {
        if (!allocator || !audioCtx) return;
        allocator.setVoiceSetup(function(voiceId, note, velocity) {
            audioCtx.setVoiceNote(voiceId, note, velocity);
            // Voice is already created by allocator — set waveform/ADSR/pan
            var osc = { type: wf }; // We need to set via engine directly
            // The createVoice was done by allocator internally; set params on the voice ID
            // Since we can't call engine directly from JS, we use a temporary oscillator-like pattern:
            // Actually the VoiceAllocator creates voices and we configure them here.
            // We don't have direct setWaveform on audioCtx for arbitrary voiceId...
            // But we do have createOscillator which creates a voice. The allocator manages voices internally.
            // For now, the allocator's voiceSetup callback gets the voiceId and we can't set params
            // on it directly from JS except through the oscillator wrapper.
            // TODO: expose direct voice config methods, or have allocator use OscillatorNodes
        });
    };

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
