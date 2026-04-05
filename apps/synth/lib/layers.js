// ---------------------------------------------------------------------------
// Layer Management — each layer has its own synth params + step pattern
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

    function createDefaultParams() {
        return {
            waveform: 'sine',
            volume: 1.0,
            pan: 0,
            mode: 'sequencer',    // 'sequencer' or 'arpeggiator'
            arpPattern: 'up',     // up, down, updown, random
            adsr: { attack: 0.01, decay: 0.1, sustain: 1.0, release: 0.08 },
            filter: { enabled: false, type: 'lowpass', frequency: 2000, Q: 1.0, gain: 0 },
            delay: { enabled: false, time: 0.3, feedback: 0.3, mix: 0.3 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        };
    }

    function createLayer(name, params) {
        var idx = layers.length;
        var steps = new Array(NUM_STEPS);
        for (var i = 0; i < NUM_STEPS; i++) steps[i] = null;

        var p = params || createDefaultParams();
        var layer = {
            id: idx,
            name: name || ('Layer ' + (idx + 1)),
            muted: false,
            color: COLORS[idx % COLORS.length],
            waveform: p.waveform || 'sine',
            volume: p.volume !== undefined ? p.volume : 1.0,
            pan: p.pan !== undefined ? p.pan : 0,
            mode: p.mode || 'sequencer',
            arpPattern: p.arpPattern || 'up',
            arpIndex: 0,
            clipId: -1,
            adsr: {
                attack: p.adsr ? p.adsr.attack : 0.01,
                decay: p.adsr ? p.adsr.decay : 0.1,
                sustain: p.adsr ? (p.adsr.sustain !== undefined ? p.adsr.sustain : 1.0) : 1.0,
                release: p.adsr ? p.adsr.release : 0.08
            },
            filter: {
                enabled: p.filter ? (p.filter.enabled || false) : false,
                type: p.filter ? (p.filter.type || 'lowpass') : 'lowpass',
                frequency: p.filter ? (p.filter.frequency || 2000) : 2000,
                Q: p.filter ? (p.filter.Q || 1.0) : 1.0,
                gain: p.filter ? (p.filter.gain || 0) : 0
            },
            delay: {
                enabled: p.delay ? (p.delay.enabled || p.delay.delayEnabled || false) : false,
                time: p.delay ? (p.delay.time || p.delay.delayTime || 0.3) : 0.3,
                feedback: p.delay ? (p.delay.feedback || p.delay.delayFeedback || 0.3) : 0.3,
                mix: p.delay ? (p.delay.mix || p.delay.delayMix || 0.3) : 0.3
            },
            lfo: {
                enabled: p.lfo ? (p.lfo.enabled || false) : false,
                rate: p.lfo ? (p.lfo.rate || 2) : 2,
                depth: p.lfo ? (p.lfo.depth || 0.3) : 0.3,
                waveform: p.lfo ? (p.lfo.waveform || 'sine') : 'sine',
                target: p.lfo ? (p.lfo.target || 'pitch') : 'pitch'
            },
            steps: steps
        };
        return layer;
    }

    // Apply a layer's params to the global engine (for keyboard playing / preview)
    function applyToEngine(layer) {
        if (!layer) return;
        Synth.setWaveform(layer.waveform);
        Synth.setPan(layer.pan);
        Synth.setADSR(layer.adsr.attack, layer.adsr.decay,
                      layer.adsr.sustain, layer.adsr.release);

        // Filter (global effect)
        Synth.Filter.setType(layer.filter.type);
        Synth.Filter.setCutoff(layer.filter.frequency);
        Synth.Filter.setQ(layer.filter.Q);
        Synth.Filter.setEnabled(layer.filter.enabled);

        // Delay (global effect)
        Synth.Effects.setDelayTime(layer.delay.time);
        Synth.Effects.setDelayFeedback(layer.delay.feedback);
        Synth.Effects.setDelayMix(layer.delay.mix);
        Synth.Effects.setDelayEnabled(layer.delay.enabled);

        // LFO
        Synth.LFO.setRate(layer.lfo.rate);
        Synth.LFO.setDepth(layer.lfo.depth);
        Synth.LFO.setWaveform(layer.lfo.waveform);
        Synth.LFO.setTarget(layer.lfo.target);
        Synth.LFO.setEnabled(layer.lfo.enabled);
    }

    // Capture current engine state into a layer
    function captureFromEngine(layer) {
        if (!layer) return;
        layer.waveform = Synth.getWaveform();
        layer.pan = Synth.getPan();
        var adsr = Synth.getADSR();
        layer.adsr.attack = adsr.attack;
        layer.adsr.decay = adsr.decay;
        layer.adsr.sustain = adsr.sustain;
        layer.adsr.release = adsr.release;

        var f = Synth.Filter.getState();
        layer.filter.enabled = f.enabled;
        layer.filter.type = f.type;
        layer.filter.frequency = f.frequency;
        layer.filter.Q = f.Q;
        layer.filter.gain = f.gain;

        var d = Synth.Effects.getState();
        layer.delay.enabled = d.delayEnabled;
        layer.delay.time = d.delayTime;
        layer.delay.feedback = d.delayFeedback;
        layer.delay.mix = d.delayMix;

        var l = Synth.LFO.getState();
        layer.lfo.enabled = l.enabled;
        layer.lfo.rate = l.rate;
        layer.lfo.depth = l.depth;
        layer.lfo.waveform = l.waveform;
        layer.lfo.target = l.target;
    }

    function fireSelectCallbacks() {
        for (var i = 0; i < selectCallbacks.length; i++) {
            selectCallbacks[i](activeIndex);
        }
    }

    Synth.Layers = {
        NUM_STEPS: NUM_STEPS,
        MAX_LAYERS: MAX_LAYERS,
        COLORS: COLORS,

        init: function() {
            // Start with one default layer that mirrors current engine state
            layers = [];
            var layer = createLayer('Layer 1');
            captureFromEngine(layer);
            layers.push(layer);
            activeIndex = 0;
        },

        // Create from current engine state (or given params)
        add: function(name, params) {
            if (layers.length >= MAX_LAYERS) return null;
            var layer = createLayer(name || ('Layer ' + (layers.length + 1)), params);
            layer.id = layers.length;
            layer.color = COLORS[layers.length % COLORS.length];
            layers.push(layer);
            return layer;
        },

        remove: function(index) {
            if (layers.length <= 1) return false; // keep at least one
            if (index < 0 || index >= layers.length) return false;
            layers.splice(index, 1);
            // Re-index
            for (var i = 0; i < layers.length; i++) layers[i].id = i;
            if (activeIndex >= layers.length) activeIndex = layers.length - 1;
            applyToEngine(layers[activeIndex]);
            fireSelectCallbacks();
            return true;
        },

        duplicate: function(index) {
            if (layers.length >= MAX_LAYERS) return null;
            if (index < 0 || index >= layers.length) return null;
            var src = layers[index];
            var dup = createLayer(src.name + ' Copy', {
                waveform: src.waveform,
                volume: src.volume,
                mode: src.mode,
                arpPattern: src.arpPattern,
                adsr: { attack: src.adsr.attack, decay: src.adsr.decay,
                        sustain: src.adsr.sustain, release: src.adsr.release },
                filter: { enabled: src.filter.enabled, type: src.filter.type,
                          frequency: src.filter.frequency, Q: src.filter.Q, gain: src.filter.gain },
                delay: { enabled: src.delay.enabled, time: src.delay.time,
                         feedback: src.delay.feedback, mix: src.delay.mix },
                lfo: { enabled: src.lfo.enabled, rate: src.lfo.rate,
                       depth: src.lfo.depth, waveform: src.lfo.waveform, target: src.lfo.target }
            });
            dup.id = layers.length;
            dup.color = COLORS[layers.length % COLORS.length];
            // Copy steps
            for (var i = 0; i < NUM_STEPS; i++) dup.steps[i] = src.steps[i];
            dup.muted = false;
            layers.push(dup);
            return dup;
        },

        select: function(index) {
            if (index < 0 || index >= layers.length) return;
            activeIndex = index;
            applyToEngine(layers[activeIndex]);
            fireSelectCallbacks();
        },

        get: function(index) { return layers[index] || null; },
        getActive: function() { return layers[activeIndex] || null; },
        getActiveIndex: function() { return activeIndex; },
        count: function() { return layers.length; },
        all: function() { return layers; },

        // Step operations on a specific layer
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

        // Apply layer's synth params temporarily (for sequencer playback)
        applyForPlayback: function(layer) {
            Synth.setWaveform(layer.waveform);
            Synth.setPan(layer.pan);
            Synth.setADSR(layer.adsr.attack, layer.adsr.decay,
                          layer.adsr.sustain, layer.adsr.release);
        },

        // Apply layer's global effects (filter/delay) during playback
        applyEffects: function(layer) {
            Synth.Filter.setType(layer.filter.type);
            Synth.Filter.setCutoff(layer.filter.frequency);
            Synth.Filter.setQ(layer.filter.Q);
            Synth.Filter.setEnabled(layer.filter.enabled);

            Synth.Effects.setDelayTime(layer.delay.time);
            Synth.Effects.setDelayFeedback(layer.delay.feedback);
            Synth.Effects.setDelayMix(layer.delay.mix);
            Synth.Effects.setDelayEnabled(layer.delay.enabled);
        },

        applyToEngine: applyToEngine,
        captureFromEngine: captureFromEngine,

        onSelect: function(cb) { selectCallbacks.push(cb); },

        // Serialize all layers for preset save
        serialize: function() {
            var out = [];
            for (var i = 0; i < layers.length; i++) {
                var l = layers[i];
                out.push({
                    name: l.name, muted: l.muted, color: l.color,
                    waveform: l.waveform, volume: l.volume, pan: l.pan,
                    mode: l.mode, arpPattern: l.arpPattern, clipId: l.clipId,
                    adsr: { attack: l.adsr.attack, decay: l.adsr.decay,
                            sustain: l.adsr.sustain, release: l.adsr.release },
                    filter: { enabled: l.filter.enabled, type: l.filter.type,
                              frequency: l.filter.frequency, Q: l.filter.Q, gain: l.filter.gain },
                    delay: { enabled: l.delay.enabled, time: l.delay.time,
                             feedback: l.delay.feedback, mix: l.delay.mix },
                    lfo: { enabled: l.lfo.enabled, rate: l.lfo.rate,
                           depth: l.lfo.depth, waveform: l.lfo.waveform, target: l.lfo.target },
                    steps: l.steps.slice()
                });
            }
            return out;
        },

        // Deserialize layers from preset
        deserialize: function(data) {
            if (!data || !data.length) return;
            layers = [];
            for (var i = 0; i < data.length && i < MAX_LAYERS; i++) {
                var d = data[i];
                var layer = createLayer(d.name, d);
                layer.id = i;
                layer.muted = d.muted || false;
                layer.color = d.color || COLORS[i % COLORS.length];
                layer.volume = d.volume !== undefined ? d.volume : 1.0;
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
            applyToEngine(layers[0]);
            fireSelectCallbacks();
        }
    };
})();
