(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var STORAGE_KEY = 'synth-presets';

    var FACTORY = {
        'Init': {
            waveform: 'sine', volume: 0.5,
            adsr: { attack: 0.01, decay: 0.1, sustain: 1.0, release: 0.08 },
            filter: { enabled: false, type: 'lowpass', frequency: 2000, Q: 1.0, gain: 0 },
            effects: { delayEnabled: false, delayTime: 0.3, delayFeedback: 0.3, delayMix: 0.3,
                       reverbEnabled: false, reverbRoomSize: 0.5, reverbDamping: 0.5, reverbMix: 0.2,
                       chorusEnabled: false, chorusRate: 1.0, chorusDepth: 0.003, chorusMix: 0.3,
                       chorusFeedback: 0, chorusBaseDelay: 0.007,
                       compressorEnabled: false, compressorThreshold: -12, compressorRatio: 4,
                       compressorAttack: 10, compressorRelease: 100 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        },
        'Warm Pad': {
            waveform: 'sawtooth', volume: 0.4,
            adsr: { attack: 0.5, decay: 0.4, sustain: 0.6, release: 1.2 },
            filter: { enabled: true, type: 'lowpass', frequency: 800, Q: 1.5, gain: 0 },
            effects: { delayEnabled: true, delayTime: 0.45, delayFeedback: 0.35, delayMix: 0.2,
                       reverbEnabled: true, reverbRoomSize: 0.8, reverbDamping: 0.4, reverbMix: 0.35,
                       chorusEnabled: true, chorusRate: 0.5, chorusDepth: 0.004, chorusMix: 0.3,
                       chorusFeedback: 0, chorusBaseDelay: 0.008,
                       compressorEnabled: false, compressorThreshold: -12, compressorRatio: 4,
                       compressorAttack: 10, compressorRelease: 100 },
            lfo: { enabled: true, rate: 0.3, depth: 0.25, waveform: 'sine', target: 'filter' }
        },
        'Bass': {
            waveform: 'square', volume: 0.5,
            adsr: { attack: 0.005, decay: 0.25, sustain: 0.35, release: 0.1 },
            filter: { enabled: true, type: 'lowpass', frequency: 400, Q: 2.5, gain: 0 },
            effects: { delayEnabled: false, delayTime: 0.3, delayFeedback: 0.3, delayMix: 0.3,
                       reverbEnabled: false, reverbRoomSize: 0.3, reverbDamping: 0.7, reverbMix: 0.1,
                       chorusEnabled: false, chorusRate: 1.0, chorusDepth: 0.003, chorusMix: 0.3,
                       chorusFeedback: 0, chorusBaseDelay: 0.007,
                       compressorEnabled: true, compressorThreshold: -10, compressorRatio: 6,
                       compressorAttack: 5, compressorRelease: 80 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        },
        'Lead': {
            waveform: 'sawtooth', volume: 0.45,
            adsr: { attack: 0.01, decay: 0.2, sustain: 0.5, release: 0.25 },
            filter: { enabled: true, type: 'lowpass', frequency: 2500, Q: 3.0, gain: 0 },
            effects: { delayEnabled: true, delayTime: 0.3, delayFeedback: 0.3, delayMix: 0.2,
                       reverbEnabled: true, reverbRoomSize: 0.5, reverbDamping: 0.5, reverbMix: 0.15,
                       chorusEnabled: false, chorusRate: 1.0, chorusDepth: 0.003, chorusMix: 0.3,
                       chorusFeedback: 0, chorusBaseDelay: 0.007,
                       compressorEnabled: false, compressorThreshold: -12, compressorRatio: 4,
                       compressorAttack: 10, compressorRelease: 100 },
            lfo: { enabled: true, rate: 4.5, depth: 0.1, waveform: 'sine', target: 'pitch' }
        },
        'Pluck': {
            waveform: 'triangle', volume: 0.5,
            adsr: { attack: 0.002, decay: 0.35, sustain: 0.0, release: 0.15 },
            filter: { enabled: true, type: 'lowpass', frequency: 3000, Q: 1.5, gain: 0 },
            effects: { delayEnabled: true, delayTime: 0.2, delayFeedback: 0.25, delayMix: 0.15,
                       reverbEnabled: true, reverbRoomSize: 0.4, reverbDamping: 0.6, reverbMix: 0.2,
                       chorusEnabled: false, chorusRate: 1.0, chorusDepth: 0.003, chorusMix: 0.3,
                       chorusFeedback: 0, chorusBaseDelay: 0.007,
                       compressorEnabled: false, compressorThreshold: -12, compressorRatio: 4,
                       compressorAttack: 10, compressorRelease: 100 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        },
        'Acid': {
            waveform: 'sawtooth', volume: 0.45,
            adsr: { attack: 0.005, decay: 0.15, sustain: 0.0, release: 0.05 },
            filter: { enabled: true, type: 'lowpass', frequency: 600, Q: 10.0, gain: 0 },
            effects: { delayEnabled: true, delayTime: 0.15, delayFeedback: 0.4, delayMix: 0.2,
                       reverbEnabled: false, reverbRoomSize: 0.5, reverbDamping: 0.5, reverbMix: 0.2,
                       chorusEnabled: false, chorusRate: 1.0, chorusDepth: 0.003, chorusMix: 0.3,
                       chorusFeedback: 0, chorusBaseDelay: 0.007,
                       compressorEnabled: true, compressorThreshold: -8, compressorRatio: 8,
                       compressorAttack: 3, compressorRelease: 50 },
            lfo: { enabled: true, rate: 2.5, depth: 0.7, waveform: 'sawtooth', target: 'filter' }
        },
        'Ambient': {
            waveform: 'sine', volume: 0.35,
            adsr: { attack: 1.0, decay: 0.8, sustain: 0.4, release: 2.0 },
            filter: { enabled: true, type: 'lowpass', frequency: 1200, Q: 0.7, gain: 0 },
            effects: { delayEnabled: true, delayTime: 0.6, delayFeedback: 0.5, delayMix: 0.3,
                       reverbEnabled: true, reverbRoomSize: 0.9, reverbDamping: 0.3, reverbMix: 0.5,
                       chorusEnabled: true, chorusRate: 0.3, chorusDepth: 0.005, chorusMix: 0.4,
                       chorusFeedback: 0.1, chorusBaseDelay: 0.01,
                       compressorEnabled: false, compressorThreshold: -12, compressorRatio: 4,
                       compressorAttack: 10, compressorRelease: 100 },
            lfo: { enabled: true, rate: 0.15, depth: 0.4, waveform: 'sine', target: 'filter' }
        }
    };

    function captureState() {
        return {
            waveform: Synth.getWaveform(),
            volume: Synth.getVolume(),
            adsr: Synth.getADSR(),
            filter: Synth.Filter.getState(),
            effects: Synth.Effects.getState(),
            lfo: Synth.LFO.getState()
        };
    }

    function applyState(state) {
        if (!state) return;
        Synth.setWaveform(state.waveform || 'sine');
        Synth.setVolume(state.volume !== undefined ? state.volume : 0.3);
        var adsr = state.adsr || {};
        Synth.setADSR(adsr.attack || 0.01, adsr.decay || 0.1,
                      adsr.sustain !== undefined ? adsr.sustain : 1.0, adsr.release || 0.04);
        Synth.Filter.loadState(state.filter);
        Synth.Effects.loadState(state.effects);
        Synth.LFO.loadState(state.lfo);
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
                applyState(user[name]);
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
