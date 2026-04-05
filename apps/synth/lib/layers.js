// ---------------------------------------------------------------------------
// Layer Management — each layer owns a broaudio bus with its own effect chain
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var NUM_STEPS = 16;
    var MAX_LAYERS = 8;

    var COLORS = [
        '#00e5ff', '#ff6b9d', '#c49bff', '#7bed9f',
        '#ffa94d', '#69d2e7', '#f38181', '#a8e6cf'
    ];

    var layers = [];
    var activeIndex = 0;
    var selectCallbacks = [];

    // Mic signal — owns a bus just like layers
    var micSignal = null; // { busId, filter, delay, reverb, chorus, compressor, lfo }
    var editingMic = false; // true when sidebar is editing mic instead of a layer

    function createDefaultParams() {
        return {
            waveform: 'sine',
            volume: 1.0,
            pan: 0,
            mode: 'sequencer',
            arpPattern: 'up',
            adsr: { attack: 0.01, decay: 0.1, sustain: 1.0, release: 0.08 },
            filter: { enabled: false, type: 'lowpass', frequency: 2000, Q: 1.0, gain: 0 },
            delay: { enabled: false, time: 0.3, feedback: 0.3, mix: 0.3 },
            reverb: { enabled: false, roomSize: 0.5, damping: 0.5, mix: 0.2 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3, feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: false, threshold: -12, ratio: 4, attack: 10, release: 100 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        };
    }

    function createDefaultEffectParams() {
        return {
            filter: { enabled: false, type: 'lowpass', frequency: 2000, Q: 1.0, gain: 0 },
            delay: { enabled: false, time: 0.3, feedback: 0.3, mix: 0.3 },
            reverb: { enabled: false, roomSize: 0.5, damping: 0.5, mix: 0.2 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3, feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: false, threshold: -12, ratio: 4, attack: 10, release: 100 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        };
    }

    function val(v, def) { return v !== undefined ? v : def; }

    function createLayer(name, params) {
        var idx = layers.length;
        var steps = new Array(NUM_STEPS);
        for (var i = 0; i < NUM_STEPS; i++) steps[i] = null;

        var p = params || createDefaultParams();

        // Create a dedicated bus for this layer
        var busId = Synth.SignalChain.createBus();

        var layer = {
            id: idx,
            name: name || ('Layer ' + (idx + 1)),
            muted: false,
            color: COLORS[idx % COLORS.length],
            busId: busId,
            waveform: p.waveform || 'sine',
            volume: val(p.volume, 1.0),
            pan: val(p.pan, 0),
            mode: p.mode || 'sequencer',
            arpPattern: p.arpPattern || 'up',
            arpIndex: 0,
            clipId: -1,
            adsr: {
                attack: p.adsr ? val(p.adsr.attack, 0.01) : 0.01,
                decay: p.adsr ? val(p.adsr.decay, 0.1) : 0.1,
                sustain: p.adsr ? val(p.adsr.sustain, 1.0) : 1.0,
                release: p.adsr ? val(p.adsr.release, 0.08) : 0.08
            },
            filter: {
                enabled: p.filter ? (p.filter.enabled || false) : false,
                type: p.filter ? (p.filter.type || 'lowpass') : 'lowpass',
                frequency: p.filter ? val(p.filter.frequency, 2000) : 2000,
                Q: p.filter ? val(p.filter.Q, 1.0) : 1.0,
                gain: p.filter ? val(p.filter.gain, 0) : 0
            },
            delay: {
                enabled: p.delay ? (p.delay.enabled || p.delay.delayEnabled || false) : false,
                time: p.delay ? (p.delay.time || p.delay.delayTime || 0.3) : 0.3,
                feedback: p.delay ? (p.delay.feedback || p.delay.delayFeedback || 0.3) : 0.3,
                mix: p.delay ? (p.delay.mix || p.delay.delayMix || 0.3) : 0.3
            },
            reverb: {
                enabled: p.reverb ? (p.reverb.enabled || false) : false,
                roomSize: p.reverb ? val(p.reverb.roomSize, 0.5) : 0.5,
                damping: p.reverb ? val(p.reverb.damping, 0.5) : 0.5,
                mix: p.reverb ? val(p.reverb.mix, 0.2) : 0.2
            },
            chorus: {
                enabled: p.chorus ? (p.chorus.enabled || false) : false,
                rate: p.chorus ? val(p.chorus.rate, 1.0) : 1.0,
                depth: p.chorus ? val(p.chorus.depth, 0.003) : 0.003,
                mix: p.chorus ? val(p.chorus.mix, 0.3) : 0.3,
                feedback: p.chorus ? val(p.chorus.feedback, 0) : 0,
                baseDelay: p.chorus ? val(p.chorus.baseDelay, 0.007) : 0.007
            },
            compressor: {
                enabled: p.compressor ? (p.compressor.enabled || false) : false,
                threshold: p.compressor ? val(p.compressor.threshold, -12) : -12,
                ratio: p.compressor ? val(p.compressor.ratio, 4) : 4,
                attack: p.compressor ? val(p.compressor.attack, 10) : 10,
                release: p.compressor ? val(p.compressor.release, 100) : 100
            },
            lfo: {
                enabled: p.lfo ? (p.lfo.enabled || false) : false,
                rate: p.lfo ? val(p.lfo.rate, 2) : 2,
                depth: p.lfo ? val(p.lfo.depth, 0.3) : 0.3,
                waveform: p.lfo ? (p.lfo.waveform || 'sine') : 'sine',
                target: p.lfo ? (p.lfo.target || 'pitch') : 'pitch'
            },
            steps: steps
        };

        // Push effect params to the bus
        Synth.SignalChain.applyParams(busId, layer);

        return layer;
    }

    // Apply voice-level params (waveform, ADSR, pan) and set voice bus routing
    function applyVoiceParams(layer) {
        if (!layer) return;
        Synth.setWaveform(layer.waveform);
        Synth.setPan(layer.pan);
        Synth.setADSR(layer.adsr.attack, layer.adsr.decay,
                      layer.adsr.sustain, layer.adsr.release);
        Synth.setCurrentBus(layer.busId);
    }

    // Apply LFO params to the mod matrix (global, per-voice)
    function applyLfoParams(layer) {
        if (!layer) return;
        Synth.LFO.setRate(layer.lfo.rate);
        Synth.LFO.setDepth(layer.lfo.depth);
        Synth.LFO.setWaveform(layer.lfo.waveform);
        Synth.LFO.setTarget(layer.lfo.target);
        Synth.LFO.setEnabled(layer.lfo.enabled);
    }

    function fireSelectCallbacks() {
        for (var i = 0; i < selectCallbacks.length; i++) {
            selectCallbacks[i](activeIndex);
        }
    }

    function cloneObj(o) { return JSON.parse(JSON.stringify(o)); }

    Synth.Layers = {
        NUM_STEPS: NUM_STEPS,
        MAX_LAYERS: MAX_LAYERS,
        COLORS: COLORS,

        init: function() {
            // Destroy old buses
            for (var i = 0; i < layers.length; i++) {
                if (layers[i].busId > 0) Synth.SignalChain.destroyBus(layers[i].busId);
            }
            layers = [];
            editingMic = false;
            var layer = createLayer('Layer 1');
            layers.push(layer);
            activeIndex = 0;
            applyVoiceParams(layer);
            applyLfoParams(layer);
        },

        add: function(name, params) {
            if (layers.length >= MAX_LAYERS) return null;
            var layer = createLayer(name || ('Layer ' + (layers.length + 1)), params);
            layer.id = layers.length;
            layer.color = COLORS[layers.length % COLORS.length];
            layers.push(layer);
            return layer;
        },

        remove: function(index) {
            if (layers.length <= 1) return false;
            if (index < 0 || index >= layers.length) return false;
            var removed = layers[index];
            if (removed.busId > 0) Synth.SignalChain.destroyBus(removed.busId);
            layers.splice(index, 1);
            for (var i = 0; i < layers.length; i++) layers[i].id = i;
            if (activeIndex >= layers.length) activeIndex = layers.length - 1;
            editingMic = false;
            var active = layers[activeIndex];
            applyVoiceParams(active);
            applyLfoParams(active);
            fireSelectCallbacks();
            return true;
        },

        duplicate: function(index) {
            if (layers.length >= MAX_LAYERS) return null;
            if (index < 0 || index >= layers.length) return null;
            var src = layers[index];
            var dup = createLayer(src.name + ' Copy', {
                waveform: src.waveform, volume: src.volume, pan: src.pan,
                mode: src.mode, arpPattern: src.arpPattern,
                adsr: cloneObj(src.adsr), filter: cloneObj(src.filter),
                delay: cloneObj(src.delay), reverb: cloneObj(src.reverb),
                chorus: cloneObj(src.chorus), compressor: cloneObj(src.compressor),
                lfo: cloneObj(src.lfo)
            });
            dup.id = layers.length;
            dup.color = COLORS[layers.length % COLORS.length];
            for (var i = 0; i < NUM_STEPS; i++) dup.steps[i] = src.steps[i];
            dup.muted = false;
            layers.push(dup);
            return dup;
        },

        select: function(index) {
            if (index < 0 || index >= layers.length) return;
            activeIndex = index;
            editingMic = false;
            var layer = layers[activeIndex];
            applyVoiceParams(layer);
            applyLfoParams(layer);
            fireSelectCallbacks();
        },

        get: function(index) { return layers[index] || null; },
        getActive: function() { return layers[activeIndex] || null; },
        getActiveIndex: function() { return activeIndex; },
        count: function() { return layers.length; },
        all: function() { return layers; },

        // Get the busId for the currently edited signal (layer or mic)
        getActiveBusId: function() {
            if (editingMic && micSignal) return micSignal.busId;
            var layer = layers[activeIndex];
            return layer ? layer.busId : -1;
        },

        // Get the params object for the currently edited signal
        getActiveSignal: function() {
            if (editingMic && micSignal) return micSignal;
            return layers[activeIndex] || null;
        },

        isEditingMic: function() { return editingMic; },

        // --- Mic signal management ---

        initMicBus: function() {
            if (micSignal) return micSignal;
            var busId = Synth.SignalChain.createBus();
            var ctx = Synth.getAudioContext();
            if (ctx) ctx.micBus = busId;
            var defaults = createDefaultEffectParams();
            micSignal = {
                busId: busId,
                name: 'Mic',
                color: '#ff4444',
                filter: defaults.filter,
                delay: defaults.delay,
                reverb: defaults.reverb,
                chorus: defaults.chorus,
                compressor: defaults.compressor,
                lfo: defaults.lfo
            };
            Synth.SignalChain.applyParams(busId, micSignal);
            return micSignal;
        },

        selectMic: function() {
            if (!micSignal) return;
            editingMic = true;
            // Apply mic's LFO to modmatrix (for modulating voice params — less useful for mic,
            // but keeps the UI consistent)
            applyLfoParams(micSignal);
            fireSelectCallbacks();
        },

        getMicSignal: function() { return micSignal; },

        destroyMicBus: function() {
            if (!micSignal) return;
            var ctx = Synth.getAudioContext();
            if (ctx) ctx.micBus = -1;
            Synth.SignalChain.destroyBus(micSignal.busId);
            micSignal = null;
            if (editingMic) {
                editingMic = false;
                fireSelectCallbacks();
            }
        },

        // --- Step grid ---

        setStep: function(layerIdx, stepIdx, noteIdx) {
            var layer = layers[layerIdx];
            if (layer && stepIdx >= 0 && stepIdx < NUM_STEPS) {
                layer.steps[stepIdx] = noteIdx;
            }
        },
        getStep: function(layerIdx, stepIdx) {
            var layer = layers[layerIdx];
            return (layer && stepIdx >= 0 && stepIdx < NUM_STEPS) ? layer.steps[stepIdx] : null;
        },
        clearStep: function(layerIdx, stepIdx) {
            var layer = layers[layerIdx];
            if (layer && stepIdx >= 0 && stepIdx < NUM_STEPS) {
                layer.steps[stepIdx] = null;
            }
        },

        // Apply voice-level params for sequencer playback (waveform, ADSR, pan, bus)
        applyForPlayback: function(layer) {
            Synth.setWaveform(layer.waveform);
            Synth.setPan(layer.pan);
            Synth.setADSR(layer.adsr.attack, layer.adsr.decay,
                          layer.adsr.sustain, layer.adsr.release);
            Synth.setCurrentBus(layer.busId);
        },

        applyVoiceParams: applyVoiceParams,
        applyLfoParams: applyLfoParams,

        onSelect: function(cb) { selectCallbacks.push(cb); },

        serialize: function() {
            var out = [];
            for (var i = 0; i < layers.length; i++) {
                var l = layers[i];
                out.push({
                    name: l.name, muted: l.muted, color: l.color,
                    waveform: l.waveform, volume: l.volume, pan: l.pan,
                    mode: l.mode, arpPattern: l.arpPattern, clipId: l.clipId,
                    adsr: cloneObj(l.adsr), filter: cloneObj(l.filter),
                    delay: cloneObj(l.delay), reverb: cloneObj(l.reverb),
                    chorus: cloneObj(l.chorus), compressor: cloneObj(l.compressor),
                    lfo: cloneObj(l.lfo), steps: l.steps.slice()
                });
            }
            return out;
        },

        deserialize: function(data) {
            if (!data || !data.length) return;
            // Destroy old buses
            for (var i = 0; i < layers.length; i++) {
                if (layers[i].busId > 0) Synth.SignalChain.destroyBus(layers[i].busId);
            }
            layers = [];
            for (var i = 0; i < data.length && i < MAX_LAYERS; i++) {
                var d = data[i];
                var layer = createLayer(d.name, d);
                layer.id = i;
                layer.muted = d.muted || false;
                layer.color = d.color || COLORS[i % COLORS.length];
                layer.volume = val(d.volume, 1.0);
                layer.mode = d.mode || 'sequencer';
                layer.arpPattern = d.arpPattern || 'up';
                layer.clipId = d.clipId !== undefined ? d.clipId : -1;
                if (d.steps) {
                    for (var s = 0; s < NUM_STEPS && s < d.steps.length; s++) {
                        layer.steps[s] = d.steps[s];
                    }
                }
                layers.push(layer);
            }
            if (layers.length === 0) {
                layers.push(createLayer('Layer 1'));
            }
            activeIndex = 0;
            editingMic = false;
            applyVoiceParams(layers[0]);
            applyLfoParams(layers[0]);
            fireSelectCallbacks();
        }
    };
})();
