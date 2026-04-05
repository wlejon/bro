(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var NUM_STEPS = 16;
    var bpm = 120;
    var currentStep = -1;
    var playing = false;
    var timerId = null;
    var onStepCallback = null;
    var onLoopCompleteCallback = null;

    // Per-layer tracking of last played note for noteOff
    var lastNotes = []; // array of { layerIdx, noteIdx }

    function getStepDuration() {
        return 60000 / bpm / 4; // 16th notes
    }

    function noteOffAll() {
        for (var i = 0; i < lastNotes.length; i++) {
            Synth.noteOff(lastNotes[i].noteIdx, true);
        }
        lastNotes = [];
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

    function tick() {
        if (!playing) return;

        // Note off previous
        noteOffAll();

        currentStep = (currentStep + 1) % NUM_STEPS;

        var Layers = Synth.Layers;
        var count = Layers.count();

        for (var li = 0; li < count; li++) {
            var layer = Layers.get(li);
            if (!layer || layer.muted) continue;

            var noteIdx = -1;

            if (layer.mode === 'arpeggiator') {
                // Arp: only play on steps that have a note
                if (layer.steps[currentStep] !== null) {
                    var held = collectUniqueNotes(layer);
                    noteIdx = pickArpNote(layer, held);
                }
            } else {
                // Sequencer: play the step's note directly
                var sn = layer.steps[currentStep];
                if (sn !== null && sn !== undefined && sn >= 0) {
                    noteIdx = sn;
                }
            }

            if (noteIdx < 0) continue;

            // Apply this layer's voice params (waveform, ADSR, pan, bus routing)
            Layers.applyForPlayback(layer);

            Synth.noteOn(noteIdx, true);
            lastNotes.push({ layerIdx: li, noteIdx: noteIdx });
        }

        // Restore active layer's voice params for keyboard play between ticks
        var active = Layers.getActive();
        if (active) Layers.applyVoiceParams(active);

        if (onStepCallback) onStepCallback(currentStep);

        // Fire loop-complete when we finish the last step (step NUM_STEPS-1)
        if (currentStep === NUM_STEPS - 1 && onLoopCompleteCallback) {
            var cb = onLoopCompleteCallback;
            onLoopCompleteCallback = null; // one-shot
            setTimeout(function() { cb(); }, getStepDuration());
        }

        timerId = setTimeout(tick, getStepDuration());
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
            tick();
        },

        stop: function() {
            playing = false;
            if (timerId) { clearTimeout(timerId); timerId = null; }
            noteOffAll();
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

        setBPM: function(b) { bpm = Math.max(30, Math.min(300, b)); },
        getBPM: function() { return bpm; },

        onStep: function(cb) { onStepCallback = cb; },

        // One-shot callback: fires after the next complete loop finishes
        onLoopComplete: function(cb) { onLoopCompleteCallback = cb; },

        // Duration of one full loop in milliseconds
        getLoopDuration: function() { return NUM_STEPS * getStepDuration(); }
    };
})();
