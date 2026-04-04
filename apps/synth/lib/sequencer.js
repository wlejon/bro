(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var NUM_STEPS = 16;
    var bpm = 120;
    var currentStep = -1;
    var playing = false;
    var timerId = null;
    var mode = 'sequencer'; // 'sequencer' or 'arpeggiator'
    var arpPattern = 'up';  // up, down, updown, random
    var arpIndex = 0;
    var onStepCallback = null;

    // Per-layer tracking of last played note for noteOff
    var lastNotes = []; // array of { layerIdx, noteIdx }

    function getStepDuration() {
        return 60000 / bpm / 4; // 16th notes
    }

    function noteOffAll() {
        for (var i = 0; i < lastNotes.length; i++) {
            Synth.noteOff(lastNotes[i].noteIdx);
        }
        lastNotes = [];
    }

    function tick() {
        if (!playing) return;

        // Note off previous
        noteOffAll();

        currentStep = (currentStep + 1) % NUM_STEPS;

        if (mode === 'sequencer') {
            var Layers = Synth.Layers;
            var count = Layers.count();
            var effectsApplied = false;

            for (var li = 0; li < count; li++) {
                var layer = Layers.get(li);
                if (!layer || layer.muted) continue;

                var noteIdx = layer.steps[currentStep];
                if (noteIdx === null || noteIdx === undefined || noteIdx < 0) continue;

                // Apply this layer's oscillator params before noteOn
                Layers.applyForPlayback(layer);

                // Apply filter/delay from first active layer this step
                if (!effectsApplied) {
                    Layers.applyEffects(layer);
                    effectsApplied = true;
                }

                Synth.noteOn(noteIdx);
                lastNotes.push({ layerIdx: li, noteIdx: noteIdx });
            }

            // If no layer played, restore active layer's effects
            if (!effectsApplied) {
                var active = Layers.getActive();
                if (active) Layers.applyEffects(active);
            }
        } else {
            // Arpeggiator: use active layer's steps as source notes
            var Layers2 = Synth.Layers;
            var activeLayer = Layers2.getActive();
            var held = [];

            Synth.getActiveNotes().forEach(function(entry, idx) {
                held.push(idx);
            });

            // Fallback to active layer's steps
            if (held.length === 0 && activeLayer) {
                for (var i = 0; i < NUM_STEPS; i++) {
                    if (activeLayer.steps[i] !== null && held.indexOf(activeLayer.steps[i]) < 0) {
                        held.push(activeLayer.steps[i]);
                    }
                }
            }

            if (held.length > 0) {
                held.sort(function(a, b) { return a - b; });
                var idx;
                switch (arpPattern) {
                    case 'up':
                        arpIndex = arpIndex % held.length;
                        idx = held[arpIndex];
                        arpIndex++;
                        break;
                    case 'down':
                        arpIndex = arpIndex % held.length;
                        idx = held[held.length - 1 - arpIndex];
                        arpIndex++;
                        break;
                    case 'updown':
                        if (held.length === 1) {
                            idx = held[0];
                        } else {
                            arpIndex = arpIndex % (held.length * 2 - 2);
                            if (arpIndex < held.length) {
                                idx = held[arpIndex];
                            } else {
                                idx = held[held.length * 2 - 2 - arpIndex];
                            }
                            arpIndex++;
                        }
                        break;
                    case 'random':
                        idx = held[Math.floor(Math.random() * held.length)];
                        break;
                    default:
                        idx = held[0];
                }

                // Apply active layer's params for arp
                if (activeLayer) {
                    Layers2.applyForPlayback(activeLayer);
                    Layers2.applyEffects(activeLayer);
                }

                Synth.noteOn(idx);
                lastNotes.push({ layerIdx: Layers2.getActiveIndex(), noteIdx: idx });
            }
        }

        if (onStepCallback) onStepCallback(currentStep);

        timerId = setTimeout(tick, getStepDuration());
    }

    Synth.Sequencer = {
        NUM_STEPS: NUM_STEPS,

        start: function() {
            if (playing) return;
            playing = true;
            currentStep = -1;
            arpIndex = 0;
            tick();
        },

        stop: function() {
            playing = false;
            if (timerId) { clearTimeout(timerId); timerId = null; }
            noteOffAll();
            currentStep = -1;
            if (onStepCallback) onStepCallback(-1);

            // Restore active layer's params
            var active = Synth.Layers ? Synth.Layers.getActive() : null;
            if (active) Synth.Layers.applyToEngine(active);
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

        setMode: function(m) { mode = m; arpIndex = 0; },
        getMode: function() { return mode; },

        setArpPattern: function(p) { arpPattern = p; arpIndex = 0; },
        getArpPattern: function() { return arpPattern; },

        onStep: function(cb) { onStepCallback = cb; }
    };
})();
