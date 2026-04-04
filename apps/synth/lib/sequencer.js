(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var NUM_STEPS = 16;
    var steps = new Array(NUM_STEPS);  // each: noteIdx or null
    var bpm = 120;
    var currentStep = -1;
    var playing = false;
    var timerId = null;
    var mode = 'sequencer'; // 'sequencer' or 'arpeggiator'
    var arpPattern = 'up';  // up, down, updown, random
    var arpDirection = 1;
    var arpIndex = 0;
    var lastNoteIdx = -1;
    var onStepCallback = null;

    for (var i = 0; i < NUM_STEPS; i++) steps[i] = null;

    function getStepDuration() {
        return 60000 / bpm / 4; // 16th notes
    }

    function tick() {
        if (!playing) return;

        // Note off previous
        if (lastNoteIdx >= 0) {
            Synth.noteOff(lastNoteIdx);
            lastNoteIdx = -1;
        }

        currentStep = (currentStep + 1) % NUM_STEPS;

        if (mode === 'sequencer') {
            var noteIdx = steps[currentStep];
            if (noteIdx !== null && noteIdx >= 0) {
                Synth.noteOn(noteIdx);
                lastNoteIdx = noteIdx;
            }
        } else {
            // Arpeggiator: cycle through held notes
            var held = [];
            Synth.getActiveNotes().forEach(function(entry, idx) {
                held.push(idx);
            });
            // Also check sequencer steps as source notes
            if (held.length === 0) {
                for (var i = 0; i < NUM_STEPS; i++) {
                    if (steps[i] !== null && held.indexOf(steps[i]) < 0) {
                        held.push(steps[i]);
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
                Synth.noteOn(idx);
                lastNoteIdx = idx;
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
            if (lastNoteIdx >= 0) { Synth.noteOff(lastNoteIdx); lastNoteIdx = -1; }
            currentStep = -1;
            if (onStepCallback) onStepCallback(-1);
        },

        isPlaying: function() { return playing; },
        getCurrentStep: function() { return currentStep; },

        setStep: function(step, noteIdx) {
            if (step >= 0 && step < NUM_STEPS) steps[step] = noteIdx;
        },
        getStep: function(step) {
            return (step >= 0 && step < NUM_STEPS) ? steps[step] : null;
        },
        clearStep: function(step) {
            if (step >= 0 && step < NUM_STEPS) steps[step] = null;
        },
        getSteps: function() { return steps.slice(); },

        setBPM: function(b) { bpm = Math.max(30, Math.min(300, b)); },
        getBPM: function() { return bpm; },

        setMode: function(m) { mode = m; arpIndex = 0; },
        getMode: function() { return mode; },

        setArpPattern: function(p) { arpPattern = p; arpIndex = 0; },
        getArpPattern: function() { return arpPattern; },

        onStep: function(cb) { onStepCallback = cb; }
    };
})();
