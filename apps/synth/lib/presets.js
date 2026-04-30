// ---------------------------------------------------------------------------
// Presets — save/load synth configurations (applies to active layer's bus)
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var STORAGE_KEY = 'synth-presets';

    var FACTORY = {
        'Init': {
            waveform: 'sine', volume: 0.5,
            adsr: { attack: 0.01, decay: 0.1, sustain: 1.0, release: 0.08 },
            filter: { enabled: false, type: 'lowpass', frequency: 2000, Q: 1.0, gain: 0 },
            delay: { enabled: false, time: 0.3, feedback: 0.3, mix: 0.3 },
            reverb: { enabled: false, roomSize: 0.5, damping: 0.5, mix: 0.2 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3,
                      feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: false, threshold: -12, ratio: 4,
                          attack: 10, release: 100 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        },
        'Warm Pad': {
            waveform: 'sawtooth', volume: 0.4,
            adsr: { attack: 0.5, decay: 0.4, sustain: 0.6, release: 1.2 },
            filter: { enabled: true, type: 'lowpass', frequency: 800, Q: 1.5, gain: 0 },
            delay: { enabled: true, time: 0.45, feedback: 0.35, mix: 0.2 },
            reverb: { enabled: true, roomSize: 0.8, damping: 0.4, mix: 0.35 },
            chorus: { enabled: true, rate: 0.5, depth: 0.004, mix: 0.3,
                      feedback: 0, baseDelay: 0.008 },
            compressor: { enabled: false, threshold: -12, ratio: 4,
                          attack: 10, release: 100 },
            lfo: { enabled: true, rate: 0.3, depth: 0.25, waveform: 'sine', target: 'filter' }
        },
        'Bass': {
            waveform: 'square', volume: 0.5,
            adsr: { attack: 0.005, decay: 0.25, sustain: 0.35, release: 0.1 },
            filter: { enabled: true, type: 'lowpass', frequency: 400, Q: 2.5, gain: 0 },
            delay: { enabled: false, time: 0.3, feedback: 0.3, mix: 0.3 },
            reverb: { enabled: false, roomSize: 0.3, damping: 0.7, mix: 0.1 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3,
                      feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: true, threshold: -10, ratio: 6,
                          attack: 5, release: 80 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        },
        'Lead': {
            waveform: 'sawtooth', volume: 0.45,
            adsr: { attack: 0.01, decay: 0.2, sustain: 0.5, release: 0.25 },
            filter: { enabled: true, type: 'lowpass', frequency: 2500, Q: 3.0, gain: 0 },
            delay: { enabled: true, time: 0.3, feedback: 0.3, mix: 0.2 },
            reverb: { enabled: true, roomSize: 0.5, damping: 0.5, mix: 0.15 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3,
                      feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: false, threshold: -12, ratio: 4,
                          attack: 10, release: 100 },
            lfo: { enabled: true, rate: 4.5, depth: 0.1, waveform: 'sine', target: 'pitch' }
        },
        'Pluck': {
            waveform: 'triangle', volume: 0.5,
            adsr: { attack: 0.002, decay: 0.35, sustain: 0.0, release: 0.15 },
            filter: { enabled: true, type: 'lowpass', frequency: 3000, Q: 1.5, gain: 0 },
            delay: { enabled: true, time: 0.2, feedback: 0.25, mix: 0.15 },
            reverb: { enabled: true, roomSize: 0.4, damping: 0.6, mix: 0.2 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3,
                      feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: false, threshold: -12, ratio: 4,
                          attack: 10, release: 100 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        },
        'Acid': {
            waveform: 'sawtooth', volume: 0.45,
            adsr: { attack: 0.005, decay: 0.15, sustain: 0.0, release: 0.05 },
            filter: { enabled: true, type: 'lowpass', frequency: 600, Q: 10.0, gain: 0 },
            delay: { enabled: true, time: 0.15, feedback: 0.4, mix: 0.2 },
            reverb: { enabled: false, roomSize: 0.5, damping: 0.5, mix: 0.2 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3,
                      feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: true, threshold: -8, ratio: 8,
                          attack: 3, release: 50 },
            lfo: { enabled: true, rate: 2.5, depth: 0.7, waveform: 'sawtooth', target: 'filter' }
        },
        'Ambient': {
            waveform: 'sine', volume: 0.35,
            adsr: { attack: 1.0, decay: 0.8, sustain: 0.4, release: 2.0 },
            filter: { enabled: true, type: 'lowpass', frequency: 1200, Q: 0.7, gain: 0 },
            delay: { enabled: true, time: 0.6, feedback: 0.5, mix: 0.3 },
            reverb: { enabled: true, roomSize: 0.9, damping: 0.3, mix: 0.5 },
            chorus: { enabled: true, rate: 0.3, depth: 0.005, mix: 0.4,
                      feedback: 0.1, baseDelay: 0.01 },
            compressor: { enabled: false, threshold: -12, ratio: 4,
                          attack: 10, release: 100 },
            lfo: { enabled: true, rate: 0.15, depth: 0.4, waveform: 'sine', target: 'filter' }
        }
    };

    // Convert old flat effects format to new per-section format
    function normalizePreset(state) {
        if (!state) return state;
        // Already in new format
        if (state.delay && state.delay.time !== undefined) return state;
        // Convert old effects blob
        var e = state.effects || {};
        return {
            waveform: state.waveform,
            volume: state.volume,
            adsr: state.adsr,
            filter: state.filter,
            delay: {
                enabled: e.delayEnabled || false,
                time: e.delayTime || 0.3,
                feedback: e.delayFeedback || 0.3,
                mix: e.delayMix || 0.3
            },
            reverb: {
                enabled: e.reverbEnabled || false,
                roomSize: e.reverbRoomSize !== undefined ? e.reverbRoomSize : 0.5,
                damping: e.reverbDamping !== undefined ? e.reverbDamping : 0.5,
                mix: e.reverbMix !== undefined ? e.reverbMix : 0.2
            },
            chorus: {
                enabled: e.chorusEnabled || false,
                rate: e.chorusRate !== undefined ? e.chorusRate : 1.0,
                depth: e.chorusDepth !== undefined ? e.chorusDepth : 0.003,
                mix: e.chorusMix !== undefined ? e.chorusMix : 0.3,
                feedback: e.chorusFeedback || 0,
                baseDelay: e.chorusBaseDelay !== undefined ? e.chorusBaseDelay : 0.007
            },
            compressor: {
                enabled: e.compressorEnabled || false,
                threshold: e.compressorThreshold !== undefined ? e.compressorThreshold : -12,
                ratio: e.compressorRatio !== undefined ? e.compressorRatio : 4,
                attack: e.compressorAttack !== undefined ? e.compressorAttack : 10,
                release: e.compressorRelease !== undefined ? e.compressorRelease : 100
            },
            lfo: state.lfo
        };
    }

    function captureState() {
        var layer = Synth.Layers.getActive();
        if (!layer) return {};
        return {
            waveform: layer.waveform,
            volume: Synth.getVolume(),
            adsr: JSON.parse(JSON.stringify(layer.adsr)),
            filter: JSON.parse(JSON.stringify(layer.filter)),
            delay: JSON.parse(JSON.stringify(layer.delay)),
            reverb: JSON.parse(JSON.stringify(layer.reverb)),
            chorus: JSON.parse(JSON.stringify(layer.chorus)),
            compressor: JSON.parse(JSON.stringify(layer.compressor)),
            eq: JSON.parse(JSON.stringify(layer.eq)),
            distortion: JSON.parse(JSON.stringify(layer.distortion)),
            lfo: JSON.parse(JSON.stringify(layer.lfo))
        };
    }

    function applyState(state) {
        state = normalizePreset(state);
        if (!state) return;

        Synth.setVolume(state.volume !== undefined ? state.volume : 0.3);
        var adsr = state.adsr || {};

        // Apply LFO to modmatrix
        Synth.LFO.loadState(state.lfo);

        // Apply params to the active layer and update its allocator
        var layer = Synth.Layers.getActive();
        if (layer) {
            layer.waveform = state.waveform || 'sine';
            if (state.adsr) {
                layer.adsr.attack = adsr.attack || 0.01;
                layer.adsr.decay = adsr.decay || 0.1;
                layer.adsr.sustain = adsr.sustain !== undefined ? adsr.sustain : 1.0;
                layer.adsr.release = adsr.release || 0.04;
            }
            if (state.filter) {
                layer.filter.enabled = state.filter.enabled || false;
                layer.filter.type = state.filter.type || 'lowpass';
                layer.filter.frequency = state.filter.frequency !== undefined ? state.filter.frequency : 2000;
                layer.filter.Q = state.filter.Q !== undefined ? state.filter.Q : 1.0;
                layer.filter.gain = state.filter.gain || 0;
            }
            if (state.delay) {
                layer.delay.enabled = state.delay.enabled || false;
                layer.delay.time = state.delay.time || 0.3;
                layer.delay.feedback = state.delay.feedback || 0.3;
                layer.delay.mix = state.delay.mix || 0.3;
            }
            if (state.reverb) {
                layer.reverb.enabled = state.reverb.enabled || false;
                layer.reverb.roomSize = state.reverb.roomSize !== undefined ? state.reverb.roomSize : 0.5;
                layer.reverb.damping = state.reverb.damping !== undefined ? state.reverb.damping : 0.5;
                layer.reverb.mix = state.reverb.mix !== undefined ? state.reverb.mix : 0.2;
            }
            if (state.chorus) {
                layer.chorus.enabled = state.chorus.enabled || false;
                layer.chorus.rate = state.chorus.rate !== undefined ? state.chorus.rate : 1.0;
                layer.chorus.depth = state.chorus.depth !== undefined ? state.chorus.depth : 0.003;
                layer.chorus.mix = state.chorus.mix !== undefined ? state.chorus.mix : 0.3;
                layer.chorus.feedback = state.chorus.feedback || 0;
                layer.chorus.baseDelay = state.chorus.baseDelay !== undefined ? state.chorus.baseDelay : 0.007;
            }
            if (state.compressor) {
                layer.compressor.enabled = state.compressor.enabled || false;
                layer.compressor.threshold = state.compressor.threshold !== undefined ? state.compressor.threshold : -12;
                layer.compressor.ratio = state.compressor.ratio !== undefined ? state.compressor.ratio : 4;
                layer.compressor.attack = state.compressor.attack !== undefined ? state.compressor.attack : 10;
                layer.compressor.release = state.compressor.release !== undefined ? state.compressor.release : 100;
            }
            if (state.eq) {
                layer.eq.enabled = state.eq.enabled || false;
                layer.eq.masterGain = state.eq.masterGain || 0;
                layer.eq.bands = state.eq.bands ? state.eq.bands.slice() : [0, 0, 0, 0, 0, 0, 0];
            }
            if (state.distortion && layer.distortion) {
                layer.distortion.enabled = state.distortion.enabled || false;
                layer.distortion.mode = state.distortion.mode || 'softclip';
                layer.distortion.drive = state.distortion.drive !== undefined ? state.distortion.drive : 2.5;
                layer.distortion.mix = state.distortion.mix !== undefined ? state.distortion.mix : 1.0;
                layer.distortion.outputGain = state.distortion.outputGain !== undefined ? state.distortion.outputGain : 0.7;
                layer.distortion.crushBits = state.distortion.crushBits !== undefined ? state.distortion.crushBits : 8;
                layer.distortion.crushRate = state.distortion.crushRate !== undefined ? state.distortion.crushRate : 0.5;
            }
            if (state.lfo) {
                layer.lfo.enabled = state.lfo.enabled || false;
                layer.lfo.rate = state.lfo.rate || 2;
                layer.lfo.depth = state.lfo.depth || 0.3;
                layer.lfo.waveform = state.lfo.waveform || 'sine';
                layer.lfo.target = state.lfo.target || 'pitch';
                layer.lfo.sync = state.lfo.sync || false;
            }

            Synth.SignalChain.applyParams(layer.busId, layer);
            Synth.Layers.updateActiveAllocator();
        }
    }

    function getUserPresets() {
        var json = localStorage.getItem(STORAGE_KEY);
        return json ? JSON.parse(json) : {};
    }

    function saveUserPresets(presets) {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(presets));
    }

    Synth.Presets = {
        FACTORY: FACTORY,

        list: function() {
            var names = Object.keys(FACTORY);
            var user = getUserPresets();
            Object.keys(user).forEach(function(k) {
                if (names.indexOf(k) < 0) names.push(k);
            });
            return names;
        },

        isFactory: function(name) {
            return name in FACTORY;
        },

        load: function(name) {
            if (name in FACTORY) {
                applyState(FACTORY[name]);
                return true;
            }
            var user = getUserPresets();
            if (name in user) {
                applyState(normalizePreset(user[name]));
                return true;
            }
            return false;
        },

        save: function(name) {
            var user = getUserPresets();
            user[name] = captureState();
            saveUserPresets(user);
        },

        delete: function(name) {
            if (name in FACTORY) return;
            var user = getUserPresets();
            delete user[name];
            saveUserPresets(user);
        },

        apply: applyState,
        capture: captureState
    };
})();
