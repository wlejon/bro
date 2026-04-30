// ---------------------------------------------------------------------------
// Layer Management — each layer owns a broaudio bus + voice allocator
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

    // Mic signal — owns a bus just like layers (but no allocator)
    var micSignal = null;
    var editingMic = false;

    // Automation target definitions: parameter name -> { min, max, default, apply(busId, value) }
    var AUTOMATION_TARGETS = {
        'filter-freq':   { label: 'Filter Freq',  min: 20,  max: 20000, default: 2000, log: true },
        'filter-q':      { label: 'Filter Q',     min: 0.1, max: 20,    default: 1.0 },
        'delay-mix':     { label: 'Delay Mix',    min: 0,   max: 1,     default: 0.3 },
        'reverb-mix':    { label: 'Reverb Mix',   min: 0,   max: 1,     default: 0.2 },
        'chorus-mix':    { label: 'Chorus Mix',   min: 0,   max: 1,     default: 0.3 },
        'volume':        { label: 'Volume',       min: 0,   max: 2,     default: 1.0 },
        'pan':           { label: 'Pan',          min: -1,  max: 1,     default: 0 }
    };

    function createDefaultParams() {
        return {
            waveform: 'sine',
            volume: 1.0,
            pan: 0,
            mode: 'sequencer',
            arpPattern: 'up',
            adsr: { attack: 0.01, decay: 0.1, sustain: 1.0, release: 0.08 },
            unison: { count: 1, detune: 0.15, stereoWidth: 0.7 },
            effectOrder: ['filter', 'delay', 'compressor', 'chorus', 'reverb', 'equalizer'],
            filter: { enabled: false, type: 'lowpass', frequency: 2000, Q: 1.0, gain: 0 },
            delay: { enabled: false, time: 0.3, feedback: 0.3, mix: 0.3 },
            reverb: { enabled: false, roomSize: 0.5, damping: 0.5, mix: 0.2 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3, feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: false, threshold: -12, ratio: 4, attack: 10, release: 100, sidechainBusId: -1 },
            eq: { enabled: false, masterGain: 0, bands: [0, 0, 0, 0, 0, 0, 0] },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' },
            automation: []  // array of { target, interpMode, points: [{beat, value}] }
        };
    }

    function createDefaultEffectParams() {
        return {
            filter: { enabled: false, type: 'lowpass', frequency: 2000, Q: 1.0, gain: 0 },
            delay: { enabled: false, time: 0.3, feedback: 0.3, mix: 0.3 },
            reverb: { enabled: false, roomSize: 0.5, damping: 0.5, mix: 0.2 },
            chorus: { enabled: false, rate: 1.0, depth: 0.003, mix: 0.3, feedback: 0, baseDelay: 0.007 },
            compressor: { enabled: false, threshold: -12, ratio: 4, attack: 10, release: 100, sidechainBusId: -1 },
            eq: { enabled: false, masterGain: 0, bands: [0, 0, 0, 0, 0, 0, 0] },
            distortion: { enabled: false, mode: 'softclip', drive: 2.5, mix: 1.0, outputGain: 0.7, crushBits: 8, crushRate: 0.5 },
            lfo: { enabled: false, rate: 2, depth: 0.3, waveform: 'sine', target: 'pitch' }
        };
    }

    function val(v, def) { return v !== undefined ? v : def; }

    // Create or update a layer's voice allocator from its current params
    function ensureAllocator(layer) {
        var voiceParams = {
            waveform: layer.waveform,
            pan: layer.pan,
            busId: layer.busId,
            adsr: layer.adsr,
            unison: layer.unison
        };
        if (layer.layerAlloc) {
            layer.layerAlloc.update(voiceParams);
        } else {
            layer.layerAlloc = Synth.createLayerAllocator(voiceParams);
        }
    }

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
            layerAlloc: null, // set below by ensureAllocator
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
            unison: {
                count: p.unison ? val(p.unison.count, 1) : 1,
                detune: p.unison ? val(p.unison.detune, 0.15) : 0.15,
                stereoWidth: p.unison ? val(p.unison.stereoWidth, 0.7) : 0.7
            },
            effectOrder: p.effectOrder ? p.effectOrder.slice() : ['filter', 'delay', 'compressor', 'chorus', 'reverb', 'equalizer', 'distortion'],
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
                release: p.compressor ? val(p.compressor.release, 100) : 100,
                sidechainBusId: p.compressor ? val(p.compressor.sidechainBusId, -1) : -1
            },
            eq: {
                enabled: p.eq ? (p.eq.enabled || false) : false,
                masterGain: p.eq ? val(p.eq.masterGain, 0) : 0,
                bands: p.eq && p.eq.bands ? p.eq.bands.slice() : [0, 0, 0, 0, 0, 0, 0]
            },
            distortion: {
                enabled: p.distortion ? (p.distortion.enabled || false) : false,
                mode: p.distortion ? (p.distortion.mode || 'softclip') : 'softclip',
                drive: p.distortion ? val(p.distortion.drive, 2.5) : 2.5,
                mix: p.distortion ? val(p.distortion.mix, 1.0) : 1.0,
                outputGain: p.distortion ? val(p.distortion.outputGain, 0.7) : 0.7,
                crushBits: p.distortion ? val(p.distortion.crushBits, 8) : 8,
                crushRate: p.distortion ? val(p.distortion.crushRate, 0.5) : 0.5
            },
            lfo: {
                enabled: p.lfo ? (p.lfo.enabled || false) : false,
                rate: p.lfo ? val(p.lfo.rate, 2) : 2,
                depth: p.lfo ? val(p.lfo.depth, 0.3) : 0.3,
                waveform: p.lfo ? (p.lfo.waveform || 'sine') : 'sine',
                target: p.lfo ? (p.lfo.target || 'pitch') : 'pitch'
            },
            automation: p.automation ? cloneObj(p.automation) : [],
            steps: steps
        };

        // Create the layer's own voice allocator
        ensureAllocator(layer);

        // Push effect params to the bus
        Synth.SignalChain.applyParams(busId, layer);

        return layer;
    }

    function destroyLayer(layer) {
        if (layer.busId > 0) Synth.SignalChain.destroyBus(layer.busId);
        // Allocator is GC'd when no longer referenced
        layer.layerAlloc = null;
    }

    // Apply LFO params to the mod matrix (global — LFO is shared)
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
            if (Synth.releaseAllNotes) Synth.releaseAllNotes();
            for (var i = 0; i < layers.length; i++) destroyLayer(layers[i]);
            layers = [];
            editingMic = false;
            var layer = createLayer('Layer 1');
            layers.push(layer);
            activeIndex = 0;
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
            destroyLayer(layers[index]);
            layers.splice(index, 1);
            for (var i = 0; i < layers.length; i++) layers[i].id = i;
            if (activeIndex >= layers.length) activeIndex = layers.length - 1;
            editingMic = false;
            applyLfoParams(layers[activeIndex]);
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
                adsr: cloneObj(src.adsr), unison: cloneObj(src.unison),
                effectOrder: src.effectOrder.slice(), filter: cloneObj(src.filter),
                delay: cloneObj(src.delay), reverb: cloneObj(src.reverb),
                chorus: cloneObj(src.chorus), compressor: cloneObj(src.compressor),
                eq: cloneObj(src.eq), distortion: cloneObj(src.distortion),
                lfo: cloneObj(src.lfo),
                automation: cloneObj(src.automation)
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
            applyLfoParams(layers[activeIndex]);
            fireSelectCallbacks();
        },

        // Update the active layer's allocator after a voice param change
        // Call this from app.js after changing waveform, ADSR, pan, or unison
        updateActiveAllocator: function() {
            var layer = editingMic ? null : layers[activeIndex];
            if (layer) ensureAllocator(layer);
        },

        get: function(index) { return layers[index] || null; },
        getActive: function() { return layers[activeIndex] || null; },
        getActiveIndex: function() { return activeIndex; },
        count: function() { return layers.length; },
        all: function() { return layers; },

        getActiveBusId: function() {
            if (editingMic && micSignal) return micSignal.busId;
            var layer = layers[activeIndex];
            return layer ? layer.busId : -1;
        },

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
                effectOrder: ['filter', 'delay', 'compressor', 'chorus', 'reverb', 'equalizer', 'distortion'],
                filter: defaults.filter,
                delay: defaults.delay,
                reverb: defaults.reverb,
                chorus: defaults.chorus,
                compressor: defaults.compressor,
                eq: defaults.eq,
                distortion: defaults.distortion,
                lfo: defaults.lfo
            };
            Synth.SignalChain.applyParams(busId, micSignal);
            return micSignal;
        },

        selectMic: function() {
            if (!micSignal) return;
            editingMic = true;
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

        AUTOMATION_TARGETS: AUTOMATION_TARGETS,
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
                    adsr: cloneObj(l.adsr), unison: cloneObj(l.unison),
                    effectOrder: l.effectOrder.slice(), filter: cloneObj(l.filter),
                    delay: cloneObj(l.delay), reverb: cloneObj(l.reverb),
                    chorus: cloneObj(l.chorus), compressor: cloneObj(l.compressor),
                    eq: cloneObj(l.eq), distortion: cloneObj(l.distortion),
                    lfo: cloneObj(l.lfo),
                    automation: cloneObj(l.automation), steps: l.steps.slice()
                });
            }
            return out;
        },

        deserialize: function(data) {
            if (!data || !data.length) return;
            for (var i = 0; i < layers.length; i++) destroyLayer(layers[i]);
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
            applyLfoParams(layers[0]);
            fireSelectCallbacks();
        }
    };
})();
