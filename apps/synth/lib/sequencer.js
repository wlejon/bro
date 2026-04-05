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
    var onLoopCompleteCallback = null;

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

            // Restore selected layer's params so keyboard play and LFO
            // use the correct layer's settings between ticks
            var active = Layers.getActive();
            if (active) Layers.applyToEngine(active);
        } else {
            // Arpeggiator: collect unique notes from active layer's step grid
            var Layers2 = Synth.Layers;
            var activeLayer = Layers2.getActive();
            var held = [];

            if (activeLayer) {
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

        // Fire loop-complete when we finish the last step (step NUM_STEPS-1)
        if (currentStep === NUM_STEPS - 1 && onLoopCompleteCallback) {
            var cb = onLoopCompleteCallback;
            onLoopCompleteCallback = null; // one-shot
            // Delay callback to after the last step's note has sounded
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

        onStep: function(cb) { onStepCallback = cb; },

        // One-shot callback: fires after the next complete loop finishes
        onLoopComplete: function(cb) { onLoopCompleteCallback = cb; },

        // Duration of one full loop in milliseconds
        getLoopDuration: function() { return NUM_STEPS * getStepDuration(); },

        // -------------------------------------------------------------------
        // Offline render: synthesize one full loop to Float32Array
        // -------------------------------------------------------------------
        renderOffline: function() {
            var SAMPLE_RATE = 44100;
            var Layers = Synth.Layers;
            var layerCount = Layers.count();
            var stepDur = 60 / bpm / 4; // seconds per 16th-note step
            var loopDur = stepDur * NUM_STEPS;

            // Collect which notes play at each step
            var stepNotes = []; // per step: [{freq, waveform, adsr, pan}, ...]
            var simArpIdx = 0;

            for (var step = 0; step < NUM_STEPS; step++) {
                var notes = [];

                if (mode === 'sequencer') {
                    for (var li = 0; li < layerCount; li++) {
                        var layer = Layers.get(li);
                        if (!layer || layer.muted) continue;
                        var ni = layer.steps[step];
                        if (ni === null || ni === undefined || ni < 0) continue;
                        var note = Synth.notes[ni];
                        if (!note) continue;
                        notes.push({
                            freq: note.freq,
                            waveform: layer.waveform,
                            adsr: { attack: layer.adsr.attack, decay: layer.adsr.decay,
                                    sustain: layer.adsr.sustain, release: layer.adsr.release },
                            pan: layer.pan
                        });
                    }
                } else {
                    // Arpeggiator — replicate the arp stepping logic
                    var activeLayer = Layers.getActive();
                    var held = [];
                    if (activeLayer) {
                        for (var i = 0; i < NUM_STEPS; i++) {
                            if (activeLayer.steps[i] !== null &&
                                held.indexOf(activeLayer.steps[i]) < 0) {
                                held.push(activeLayer.steps[i]);
                            }
                        }
                    }
                    if (held.length > 0) {
                        held.sort(function(a, b) { return a - b; });
                        var idx;
                        switch (arpPattern) {
                            case 'up':
                                simArpIdx = simArpIdx % held.length;
                                idx = held[simArpIdx++];
                                break;
                            case 'down':
                                simArpIdx = simArpIdx % held.length;
                                idx = held[held.length - 1 - simArpIdx++];
                                break;
                            case 'updown':
                                if (held.length === 1) { idx = held[0]; }
                                else {
                                    simArpIdx = simArpIdx % (held.length * 2 - 2);
                                    idx = simArpIdx < held.length
                                        ? held[simArpIdx]
                                        : held[held.length * 2 - 2 - simArpIdx];
                                    simArpIdx++;
                                }
                                break;
                            case 'random':
                                idx = held[Math.floor(Math.random() * held.length)];
                                break;
                            default: idx = held[0];
                        }
                        var note = Synth.notes[idx];
                        if (note && activeLayer) {
                            notes.push({
                                freq: note.freq,
                                waveform: activeLayer.waveform,
                                adsr: { attack: activeLayer.adsr.attack, decay: activeLayer.adsr.decay,
                                        sustain: activeLayer.adsr.sustain, release: activeLayer.adsr.release },
                                pan: activeLayer.pan
                            });
                        }
                    }
                }

                stepNotes.push(notes);
            }

            // Find max release to allow tails
            var maxRelease = 0;
            for (var li2 = 0; li2 < layerCount; li2++) {
                var l = Layers.get(li2);
                if (l && !l.muted) maxRelease = Math.max(maxRelease, l.adsr.release);
            }

            var totalSamples = Math.ceil((loopDur + maxRelease) * SAMPLE_RATE);
            var output = new Float32Array(totalSamples);

            // Render each step's notes
            for (var step2 = 0; step2 < NUM_STEPS; step2++) {
                var startSmp = Math.floor(step2 * stepDur * SAMPLE_RATE);
                var onSmp = Math.floor(stepDur * SAMPLE_RATE);
                var sNotes = stepNotes[step2];

                for (var ni2 = 0; ni2 < sNotes.length; ni2++) {
                    var n = sNotes[ni2];
                    var relSmp = Math.ceil(n.adsr.release * SAMPLE_RATE);
                    var noteSmp = onSmp + relSmp;

                    for (var s = 0; s < noteSmp; s++) {
                        var outIdx = startSmp + s;
                        if (outIdx >= totalSamples) break;

                        var t = s / SAMPLE_RATE;
                        var phase = t * n.freq;

                        // Waveform
                        var val;
                        switch (n.waveform) {
                            case 'square':
                                val = (phase % 1) < 0.5 ? 1 : -1; break;
                            case 'sawtooth':
                                val = 2 * (phase % 1) - 1; break;
                            case 'triangle':
                                var p = phase % 1;
                                val = p < 0.5 ? 4 * p - 1 : 3 - 4 * p; break;
                            default: // sine
                                val = Math.sin(2 * Math.PI * phase);
                        }

                        // ADSR envelope
                        var env;
                        if (t < n.adsr.attack) {
                            env = n.adsr.attack > 0 ? t / n.adsr.attack : 1;
                        } else if (t < n.adsr.attack + n.adsr.decay) {
                            var dp = (t - n.adsr.attack) / (n.adsr.decay || 0.001);
                            env = 1.0 - dp * (1.0 - n.adsr.sustain);
                        } else if (t < stepDur) {
                            env = n.adsr.sustain;
                        } else {
                            var rt = t - stepDur;
                            env = rt < n.adsr.release
                                ? n.adsr.sustain * (1 - rt / n.adsr.release)
                                : 0;
                        }

                        output[outIdx] += val * env * 0.3;
                    }
                }
            }

            // Trim trailing silence
            var end = totalSamples - 1;
            while (end > 0 && Math.abs(output[end]) < 0.0001) end--;
            // But keep at least the loop duration
            var minSmp = Math.ceil(loopDur * SAMPLE_RATE);
            end = Math.max(end, minSmp - 1);
            output = output.subarray(0, end + 1);

            // Clamp
            for (var c = 0; c < output.length; c++) {
                if (output[c] > 1) output[c] = 1;
                else if (output[c] < -1) output[c] = -1;
            }

            return { samples: output, sampleRate: SAMPLE_RATE };
        }
    };
})();
