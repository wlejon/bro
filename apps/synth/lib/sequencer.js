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

            // Per-layer simulated arp index
            var simArpIndices = [];
            for (var la = 0; la < layerCount; la++) simArpIndices.push(0);

            for (var step = 0; step < NUM_STEPS; step++) {
                var notes = [];

                for (var li = 0; li < layerCount; li++) {
                    var layer = Layers.get(li);
                    if (!layer || layer.muted) continue;

                    var noteIdx = -1;

                    if (layer.mode === 'arpeggiator') {
                        if (layer.steps[step] !== null) {
                            var held = collectUniqueNotes(layer);
                            if (held.length > 0) {
                                // Simulate arp pick using per-layer index
                                var simLayer = { arpPattern: layer.arpPattern, arpIndex: simArpIndices[li] };
                                noteIdx = pickArpNote(simLayer, held);
                                simArpIndices[li] = simLayer.arpIndex;
                            }
                        }
                    } else {
                        var sn = layer.steps[step];
                        if (sn !== null && sn !== undefined && sn >= 0) {
                            noteIdx = sn;
                        }
                    }

                    if (noteIdx < 0) continue;
                    var note = Synth.notes[noteIdx];
                    if (!note) continue;
                    notes.push({
                        freq: note.freq,
                        waveform: layer.waveform,
                        adsr: { attack: layer.adsr.attack, decay: layer.adsr.decay,
                                sustain: layer.adsr.sustain, release: layer.adsr.release },
                        pan: layer.pan
                    });
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
