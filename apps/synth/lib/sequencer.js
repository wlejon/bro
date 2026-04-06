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
    var playbackStartTime = 0; // engine time when playback started (for syncing new sequences)

    // Native sequences — one per layer, each using the layer's own allocator
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

    // Load notes from a layer's step grid into a sequence (no side effects)
    function loadLayerNotes(seq, layer) {
        seq.clearNotes();

        var stepBeats = 0.25; // each step = 1/4 beat (16th note)
        var noteDurBeats = stepBeats * 0.9; // slight gap between notes

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
    }

    // Create the apply function for an automation target
    function makeApplyFn(audioCtx, busId, target) {
        var SC = Synth.SignalChain;
        switch (target) {
            case 'filter-freq':   return function(v) { SC.setFilterFrequency(busId, v); };
            case 'filter-q':      return function(v) { SC.setFilterQ(busId, v); };
            case 'delay-mix':     return function(v) { SC.setDelayMix(busId, v); };
            case 'reverb-mix':    return function(v) { SC.setReverbMix(busId, v); };
            case 'chorus-mix':    return function(v) { SC.setChorusMix(busId, v); };
            case 'volume':        return function(v) { audioCtx.setBusGain(busId, v); };
            case 'pan':           return function(v) { audioCtx.setBusPan(busId, v); };
            default:              return function() {};
        }
    }

    // Load automation lanes from a layer's automation data into a sequence
    function loadLayerAutomation(seq, layer) {
        seq.clearAutomationLanes();
        var audioCtx = Synth.getAudioContext();
        if (!audioCtx || !layer.automation) return;

        for (var i = 0; i < layer.automation.length; i++) {
            var auto = layer.automation[i];
            if (!auto.points || auto.points.length === 0) continue;

            var applyFn = makeApplyFn(audioCtx, layer.busId, auto.target);
            var laneIdx = seq.addAutomationLane(applyFn);
            seq.setAutomationInterpMode(laneIdx, auto.interpMode || 'linear');

            for (var p = 0; p < auto.points.length; p++) {
                seq.addAutomationPoint(laneIdx, auto.points[p].beat, auto.points[p].value);
            }
        }
    }

    // Build a new Sequence using the layer's own allocator
    function buildLayerSequence(layer) {
        var audioCtx = Synth.getAudioContext();
        if (!audioCtx || !layer.layerAlloc) return null;

        var seq = audioCtx.createSequence(layer.layerAlloc.allocator);
        seq.setBPM(bpm);
        seq.setLoopEnabled(true);
        seq.setLoopRange(0, 4); // 16 steps = 4 beats

        loadLayerNotes(seq, layer);
        loadLayerAutomation(seq, layer);
        return seq;
    }

    function destroyAllSequences() {
        for (var i = 0; i < layerSequences.length; i++) {
            layerSequences[i].seq.stop();
        }
        layerSequences = [];
    }

    // Track step position from beat position for UI highlight.
    function updateStepFromBeat() {
        if (!playing) return;

        var audioCtx = Synth.getAudioContext();
        if (!audioCtx) return;
        var t = audioCtx.currentTime;

        // Use first sequence to get current beat for UI
        var newStep = -1;
        if (layerSequences.length > 0) {
            var beat = layerSequences[0].seq.currentBeat(t);
            newStep = Math.floor(beat / 0.25) % NUM_STEPS;
        }

        if (newStep !== currentStep) {
            currentStep = newStep;
            if (onStepCallback) onStepCallback(currentStep);

            // Re-randomize arp notes each loop so "random" isn't static
            if (currentStep === 0) {
                var Layers = Synth.Layers;
                for (var ri = 0; ri < layerSequences.length; ri++) {
                    var rl = Layers.get(layerSequences[ri].layerIdx);
                    if (rl && rl.mode === 'arpeggiator' && rl.arpPattern === 'random') {
                        loadLayerNotes(layerSequences[ri].seq, rl);
                    }
                }
            }

            if (currentStep === 0 && onLoopCompleteCallback) {
                var cb = onLoopCompleteCallback;
                onLoopCompleteCallback = null;
                setTimeout(function() { cb(); }, 0);
            }
        }

        // Update each sequence — no global state swapping needed,
        // each sequence's allocator has its own voiceSetup callback
        for (var i = 0; i < layerSequences.length; i++) {
            layerSequences[i].seq.update(t);
        }

        updateTimerId = setTimeout(updateStepFromBeat, 16);
    }

    function layerHasContent(layer) {
        for (var s = 0; s < NUM_STEPS; s++) {
            if (layer.steps[s] !== null) return true;
        }
        if (layer.automation) {
            for (var i = 0; i < layer.automation.length; i++) {
                if (layer.automation[i].points && layer.automation[i].points.length > 0) return true;
            }
        }
        return false;
    }

    Synth.Sequencer = {
        NUM_STEPS: NUM_STEPS,

        start: function() {
            if (playing) return;
            playing = true;
            currentStep = -1;

            var Layers = Synth.Layers;
            for (var i = 0; i < Layers.count(); i++) {
                var l = Layers.get(i);
                if (l) l.arpIndex = 0;
            }

            destroyAllSequences();
            var count = Layers.count();
            for (var li = 0; li < count; li++) {
                var layer = Layers.get(li);
                if (!layer || layer.muted || !layerHasContent(layer)) continue;

                var seq = buildLayerSequence(layer);
                if (seq) layerSequences.push({ seq: seq, layerIdx: li });
            }

            var audioCtx = Synth.getAudioContext();
            playbackStartTime = audioCtx ? audioCtx.currentTime : 0;
            for (var i = 0; i < layerSequences.length; i++) {
                layerSequences[i].seq.play(playbackStartTime);
            }

            updateStepFromBeat();
        },

        stop: function() {
            playing = false;
            if (updateTimerId) { clearTimeout(updateTimerId); updateTimerId = null; }
            destroyAllSequences();
            currentStep = -1;
            if (onStepCallback) onStepCallback(-1);
        },

        isPlaying: function() { return playing; },
        getCurrentStep: function() { return currentStep; },

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
            for (var i = 0; i < layerSequences.length; i++) {
                layerSequences[i].seq.setBPM(bpm);
            }
        },
        getBPM: function() { return bpm; },

        onStep: function(cb) { onStepCallback = cb; },
        onLoopComplete: function(cb) { onLoopCompleteCallback = cb; },
        getLoopDuration: function() { return NUM_STEPS * getStepDuration(); },

        // Rebuild sequences while playing — updates notes in place where possible
        rebuild: function() {
            if (!playing) return;
            var audioCtx = Synth.getAudioContext();
            var Layers = Synth.Layers;
            var count = Layers.count();

            // Build set of layers that should be playing
            var wantedLayers = {};
            for (var li = 0; li < count; li++) {
                var layer = Layers.get(li);
                if (!layer || layer.muted || !layerHasContent(layer)) continue;
                wantedLayers[li] = layer;
            }

            // Update existing sequences or remove stale ones
            var kept = [];
            for (var i = 0; i < layerSequences.length; i++) {
                var ls = layerSequences[i];
                if (wantedLayers[ls.layerIdx]) {
                    loadLayerNotes(ls.seq, wantedLayers[ls.layerIdx]);
                    loadLayerAutomation(ls.seq, wantedLayers[ls.layerIdx]);
                    kept.push(ls);
                    delete wantedLayers[ls.layerIdx];
                } else {
                    ls.seq.stop();
                }
            }

            // Create new sequences for layers that weren't playing yet.
            // Sync them to existing playback position:
            //   1. Create empty sequence and start it at the synced time
            //   2. Run one update() so lastUpdateBeat advances to current position
            //   3. THEN load the notes — only future notes will fire
            var now = audioCtx ? audioCtx.currentTime : 0;
            var syncBeat = 0;
            if (kept.length > 0) {
                syncBeat = kept[0].seq.currentBeat(now);
            }
            var syncedStart = now - syncBeat * 60 / bpm;

            var remaining = Object.keys(wantedLayers);
            for (var j = 0; j < remaining.length; j++) {
                var idx = parseInt(remaining[j], 10);
                var layer = wantedLayers[idx];
                if (!layer.layerAlloc) continue;

                // Create empty sequence, start synced, advance past current beat
                var seq = audioCtx.createSequence(layer.layerAlloc.allocator);
                seq.setBPM(bpm);
                seq.setLoopEnabled(true);
                seq.setLoopRange(0, 4);
                seq.play(syncedStart);
                seq.update(now); // advances lastUpdateBeat to current position (no notes to fire)

                // Now load the actual notes — only notes after current beat will fire this loop
                loadLayerNotes(seq, layer);
                loadLayerAutomation(seq, layer);
                kept.push({ seq: seq, layerIdx: idx });
            }

            layerSequences = kept;
        }
    };
})();
