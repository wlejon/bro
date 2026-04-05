(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var NUM_STEPS = 16;
    var bpm = 120;
    var currentStep = -1;
    var playing = false;
    var updateTimerId = null;
    var onStepCallback = null;
    var onLoopCompleteCallback = null;

    // Native sequences — one per layer for sample-accurate timing
    var layerSequences = []; // array of { seq, layerIdx }

    function getStepDuration() {
        return 60000 / bpm / 4; // 16th notes
    }

    // Collect unique note indices from a layer's step grid (sorted ascending)
    function collectUniqueNotes(layer) {
        var held = [];
        for (var i = 0; i < NUM_STEPS; i++) {
            if (layer.steps[i] !== null && held.indexOf(layer.steps[i]) < 0) {
                held.push(layer.steps[i]);
            }
        }
        held.sort(function(a, b) { return a - b; });
        return held;
    }

    // Pick the next arp note from a sorted array, advancing layer.arpIndex
    function pickArpNote(layer, held) {
        if (held.length === 0) return -1;
        var idx;
        var pattern = layer.arpPattern || 'up';
        switch (pattern) {
            case 'up':
                layer.arpIndex = layer.arpIndex % held.length;
                idx = held[layer.arpIndex];
                layer.arpIndex++;
                break;
            case 'down':
                layer.arpIndex = layer.arpIndex % held.length;
                idx = held[held.length - 1 - layer.arpIndex];
                layer.arpIndex++;
                break;
            case 'updown':
                if (held.length === 1) {
                    idx = held[0];
                } else {
                    layer.arpIndex = layer.arpIndex % (held.length * 2 - 2);
                    if (layer.arpIndex < held.length) {
                        idx = held[layer.arpIndex];
                    } else {
                        idx = held[held.length * 2 - 2 - layer.arpIndex];
                    }
                    layer.arpIndex++;
                }
                break;
            case 'random':
                idx = held[Math.floor(Math.random() * held.length)];
                break;
            default:
                idx = held[0];
        }
        return idx;
    }

    // Build native NoteEvents from a layer's step grid and load into a Sequence
    function buildLayerSequence(layer) {
        var audioCtx = Synth.getAudioContext();
        var allocator = Synth.getAllocator();
        if (!audioCtx || !allocator) return null;

        var seq = audioCtx.createSequence(allocator);
        seq.setBPM(bpm);
        seq.setLoopEnabled(true);
        // 16 steps = 4 beats in 4/4 at 16th note resolution
        seq.setLoopRange(0, 4);

        var stepBeats = 0.25; // each step = 1/4 beat (16th note)
        var noteDurBeats = stepBeats * 0.9; // slight gap between notes

        // Apply this layer's voice params before building notes
        Synth.Layers.applyForPlayback(layer);

        if (layer.mode === 'arpeggiator') {
            var held = collectUniqueNotes(layer);
            layer.arpIndex = 0;
            for (var s = 0; s < NUM_STEPS; s++) {
                if (layer.steps[s] === null) continue;
                var noteIdx = pickArpNote(layer, held);
                if (noteIdx < 0) continue;
                var note = Synth.notes[noteIdx];
                if (!note) continue;
                seq.addNote(s * stepBeats, note.midi, 1.0, noteDurBeats);
            }
        } else {
            for (var s = 0; s < NUM_STEPS; s++) {
                var sn = layer.steps[s];
                if (sn === null || sn === undefined || sn < 0) continue;
                var note = Synth.notes[sn];
                if (!note) continue;
                seq.addNote(s * stepBeats, note.midi, 1.0, noteDurBeats);
            }
        }

        return seq;
    }

    function destroyAllSequences() {
        for (var i = 0; i < layerSequences.length; i++) {
            layerSequences[i].seq.stop();
        }
        layerSequences = [];
    }

    // Track step position from beat position for UI highlight.
    // Each layer's sequence is updated separately so we can switch voice params
    // (waveform, ADSR, bus routing, unison) before each layer's notes fire.
    function updateStepFromBeat() {
        if (!playing) return;

        var audioCtx = Synth.getAudioContext();
        if (!audioCtx) return;
        var t = audioCtx.currentTime;
        var Layers = Synth.Layers;

        // Use first sequence to get current beat for UI
        var newStep = -1;
        if (layerSequences.length > 0) {
            var beat = layerSequences[0].seq.currentBeat(t);
            newStep = Math.floor(beat / 0.25) % NUM_STEPS;
        }

        if (newStep !== currentStep) {
            currentStep = newStep;
            if (onStepCallback) onStepCallback(currentStep);

            // Fire loop-complete when wrapping from last step
            if (currentStep === 0 && onLoopCompleteCallback) {
                var cb = onLoopCompleteCallback;
                onLoopCompleteCallback = null;
                setTimeout(function() { cb(); }, 0);
            }
        }

        // Update each layer's sequence with that layer's voice params active
        for (var i = 0; i < layerSequences.length; i++) {
            var ls = layerSequences[i];
            var layer = Layers.get(ls.layerIdx);
            if (layer) Layers.applyForPlayback(layer);
            ls.seq.update(t);
        }

        // Restore active layer's voice params for keyboard play between ticks
        var active = Layers.getActive();
        if (active) Layers.applyVoiceParams(active);

        updateTimerId = setTimeout(updateStepFromBeat, 16); // ~60fps UI update
    }

    Synth.Sequencer = {
        NUM_STEPS: NUM_STEPS,

        start: function() {
            if (playing) return;
            playing = true;
            currentStep = -1;

            // Reset arp indices on all layers
            var Layers = Synth.Layers;
            for (var i = 0; i < Layers.count(); i++) {
                var l = Layers.get(i);
                if (l) l.arpIndex = 0;
            }

            // Build and start a native sequence per non-muted layer
            destroyAllSequences();
            var count = Layers.count();
            for (var li = 0; li < count; li++) {
                var layer = Layers.get(li);
                if (!layer || layer.muted) continue;

                // Check if layer has any notes
                var hasNotes = false;
                for (var s = 0; s < NUM_STEPS; s++) {
                    if (layer.steps[s] !== null) { hasNotes = true; break; }
                }
                if (!hasNotes) continue;

                var seq = buildLayerSequence(layer);
                if (seq) {
                    layerSequences.push({ seq: seq, layerIdx: li });
                }
            }

            // Start all sequences at the same time for sync
            var audioCtx = Synth.getAudioContext();
            var startTime = audioCtx ? audioCtx.currentTime : 0;
            for (var i = 0; i < layerSequences.length; i++) {
                layerSequences[i].seq.play(startTime);
            }

            // Start UI update loop
            updateStepFromBeat();

            // Restore active layer's voice params for keyboard play
            var active = Layers.getActive();
            if (active) Layers.applyVoiceParams(active);
        },

        stop: function() {
            playing = false;
            if (updateTimerId) { clearTimeout(updateTimerId); updateTimerId = null; }
            destroyAllSequences();
            currentStep = -1;
            if (onStepCallback) onStepCallback(-1);

            // Restore active layer's voice params
            var active = Synth.Layers ? Synth.Layers.getActive() : null;
            if (active) Synth.Layers.applyVoiceParams(active);
        },

        isPlaying: function() { return playing; },
        getCurrentStep: function() { return currentStep; },

        // Convenience: operate on active layer
        setStep: function(step, noteIdx) {
            if (Synth.Layers) {
                Synth.Layers.setStep(Synth.Layers.getActiveIndex(), step, noteIdx);
            }
        },
        getStep: function(step) {
            if (Synth.Layers) {
                return Synth.Layers.getStep(Synth.Layers.getActiveIndex(), step);
            }
            return null;
        },
        clearStep: function(step) {
            if (Synth.Layers) {
                Synth.Layers.clearStep(Synth.Layers.getActiveIndex(), step);
            }
        },

        setBPM: function(b) {
            bpm = Math.max(30, Math.min(300, b));
            // Update running sequences
            for (var i = 0; i < layerSequences.length; i++) {
                layerSequences[i].seq.setBPM(bpm);
            }
        },
        getBPM: function() { return bpm; },

        onStep: function(cb) { onStepCallback = cb; },

        // One-shot callback: fires after the next complete loop finishes
        onLoopComplete: function(cb) { onLoopCompleteCallback = cb; },

        // Duration of one full loop in milliseconds
        getLoopDuration: function() { return NUM_STEPS * getStepDuration(); },

        // Rebuild sequences while playing (e.g., after step grid edit)
        rebuild: function() {
            if (!playing) return;
            var audioCtx = Synth.getAudioContext();
            var Layers = Synth.Layers;
            var wasPlaying = playing;

            destroyAllSequences();
            var count = Layers.count();
            for (var li = 0; li < count; li++) {
                var layer = Layers.get(li);
                if (!layer || layer.muted) continue;
                var hasNotes = false;
                for (var s = 0; s < NUM_STEPS; s++) {
                    if (layer.steps[s] !== null) { hasNotes = true; break; }
                }
                if (!hasNotes) continue;
                var seq = buildLayerSequence(layer);
                if (seq) layerSequences.push({ seq: seq, layerIdx: li });
            }

            var startTime = audioCtx ? audioCtx.currentTime : 0;
            for (var i = 0; i < layerSequences.length; i++) {
                layerSequences[i].seq.play(startTime);
            }

            // Restore active layer params
            var active = Layers.getActive();
            if (active) Layers.applyVoiceParams(active);
        }
    };
})();
