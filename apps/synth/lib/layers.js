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

    function val(v, def) { return v !== undefined ? v : def; }

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
        return layer;
    }

    function applyToEngine(layer) {
        if (!layer) return;
        Synth.setWaveform(layer.waveform);
        Synth.setPan(layer.pan);
        Synth.setADSR(layer.adsr.attack, layer.adsr.decay,
                      layer.adsr.sustain, layer.adsr.release);

        Synth.Filter.setType(layer.filter.type);
        Synth.Filter.setCutoff(layer.filter.frequency);
        Synth.Filter.setQ(layer.filter.Q);
        Synth.Filter.setEnabled(layer.filter.enabled);

        // Delay
        Synth.Effects.setDelayTime(layer.delay.time);
        Synth.Effects.setDelayFeedback(layer.delay.feedback);
        Synth.Effects.setDelayMix(layer.delay.mix);
        Synth.Effects.setDelayEnabled(layer.delay.enabled);

        // Reverb
        Synth.Effects.setReverbRoomSize(layer.reverb.roomSize);
        Synth.Effects.setReverbDamping(layer.reverb.damping);
        Synth.Effects.setReverbMix(layer.reverb.mix);
        Synth.Effects.setReverbEnabled(layer.reverb.enabled);

        // Chorus
        Synth.Effects.setChorusRate(layer.chorus.rate);
        Synth.Effects.setChorusDepth(layer.chorus.depth);
        Synth.Effects.setChorusMix(layer.chorus.mix);
        Synth.Effects.setChorusFeedback(layer.chorus.feedback);
        Synth.Effects.setChorusBaseDelay(layer.chorus.baseDelay);
        Synth.Effects.setChorusEnabled(layer.chorus.enabled);

        // Compressor
        Synth.Effects.setCompressorThreshold(layer.compressor.threshold);
        Synth.Effects.setCompressorRatio(layer.compressor.ratio);
        Synth.Effects.setCompressorAttack(layer.compressor.attack);
        Synth.Effects.setCompressorRelease(layer.compressor.release);
        Synth.Effects.setCompressorEnabled(layer.compressor.enabled);

        // LFO
        Synth.LFO.setRate(layer.lfo.rate);
        Synth.LFO.setDepth(layer.lfo.depth);
        Synth.LFO.setWaveform(layer.lfo.waveform);
        Synth.LFO.setTarget(layer.lfo.target);
        Synth.LFO.setEnabled(layer.lfo.enabled);
    }

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

        var e = Synth.Effects.getState();
        layer.delay.enabled = e.delayEnabled;
        layer.delay.time = e.delayTime;
        layer.delay.feedback = e.delayFeedback;
        layer.delay.mix = e.delayMix;

        layer.reverb.enabled = e.reverbEnabled;
        layer.reverb.roomSize = e.reverbRoomSize;
        layer.reverb.damping = e.reverbDamping;
        layer.reverb.mix = e.reverbMix;

        layer.chorus.enabled = e.chorusEnabled;
        layer.chorus.rate = e.chorusRate;
        layer.chorus.depth = e.chorusDepth;
        layer.chorus.mix = e.chorusMix;
        layer.chorus.feedback = e.chorusFeedback;
        layer.chorus.baseDelay = e.chorusBaseDelay;

        layer.compressor.enabled = e.compressorEnabled;
        layer.compressor.threshold = e.compressorThreshold;
        layer.compressor.ratio = e.compressorRatio;
        layer.compressor.attack = e.compressorAttack;
        layer.compressor.release = e.compressorRelease;

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

    function cloneObj(o) { return JSON.parse(JSON.stringify(o)); }

    Synth.Layers = {
        NUM_STEPS: NUM_STEPS,
        MAX_LAYERS: MAX_LAYERS,
        COLORS: COLORS,

        init: function() {
            layers = [];
            var layer = createLayer('Layer 1');
            captureFromEngine(layer);
            layers.push(layer);
            activeIndex = 0;
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
            layers.splice(index, 1);
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
            applyToEngine(layers[activeIndex]);
            fireSelectCallbacks();
        },

        get: function(index) { return layers[index] || null; },
        getActive: function() { return layers[activeIndex] || null; },
        getActiveIndex: function() { return activeIndex; },
        count: function() { return layers.length; },
        all: function() { return layers; },

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

        applyForPlayback: function(layer) {
            Synth.setWaveform(layer.waveform);
            Synth.setPan(layer.pan);
            Synth.setADSR(layer.adsr.attack, layer.adsr.decay,
                          layer.adsr.sustain, layer.adsr.release);
        },

        applyEffects: function(layer) {
            Synth.Filter.setType(layer.filter.type);
            Synth.Filter.setCutoff(layer.filter.frequency);
            Synth.Filter.setQ(layer.filter.Q);
            Synth.Filter.setEnabled(layer.filter.enabled);

            Synth.Effects.setDelayTime(layer.delay.time);
            Synth.Effects.setDelayFeedback(layer.delay.feedback);
            Synth.Effects.setDelayMix(layer.delay.mix);
            Synth.Effects.setDelayEnabled(layer.delay.enabled);

            Synth.Effects.setReverbRoomSize(layer.reverb.roomSize);
            Synth.Effects.setReverbDamping(layer.reverb.damping);
            Synth.Effects.setReverbMix(layer.reverb.mix);
            Synth.Effects.setReverbEnabled(layer.reverb.enabled);

            Synth.Effects.setChorusRate(layer.chorus.rate);
            Synth.Effects.setChorusDepth(layer.chorus.depth);
            Synth.Effects.setChorusMix(layer.chorus.mix);
            Synth.Effects.setChorusFeedback(layer.chorus.feedback);
            Synth.Effects.setChorusBaseDelay(layer.chorus.baseDelay);
            Synth.Effects.setChorusEnabled(layer.chorus.enabled);

            Synth.Effects.setCompressorThreshold(layer.compressor.threshold);
            Synth.Effects.setCompressorRatio(layer.compressor.ratio);
            Synth.Effects.setCompressorAttack(layer.compressor.attack);
            Synth.Effects.setCompressorRelease(layer.compressor.release);
            Synth.Effects.setCompressorEnabled(layer.compressor.enabled);
        },

        applyToEngine: applyToEngine,
        captureFromEngine: captureFromEngine,

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
            applyToEngine(layers[0]);
            fireSelectCallbacks();
        }
    };
})();
