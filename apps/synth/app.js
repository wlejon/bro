// ---------------------------------------------------------------------------
// Synth App — wires all modules together
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    function $$(sel) { return Array.from(document.querySelectorAll(sel)); }

    // Init audio engine
    Synth.init();
    Synth.Filter.init();
    Synth.Effects.init();

    // Init keyboard
    Synth.Keyboard.init(
        document.getElementById('keyboard'),
        document.getElementById('octave-display')
    );

    // Init visualizer
    Synth.Visualizer.init(document.getElementById('viz'));
    Synth.Visualizer.draw();

    // -----------------------------------------------------------------------
    // Helpers: update value display next to slider
    // -----------------------------------------------------------------------
    function showVal(id, text) {
        var el = document.getElementById(id);
        if (el) el.textContent = text;
    }

    function updateToggle(btn, active) {
        btn.classList.toggle('active', active);
        btn.textContent = active ? 'On' : 'Off';
    }

    // -----------------------------------------------------------------------
    // Waveform buttons
    // -----------------------------------------------------------------------
    $$('#wave-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#wave-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            Synth.setWaveform(btn.getAttribute('data-wave'));
        });
    });

    // -----------------------------------------------------------------------
    // Volume
    // -----------------------------------------------------------------------
    document.getElementById('volume').addEventListener('input', function(e) {
        Synth.setVolume(parseInt(e.target.value) / 100);
    });

    // -----------------------------------------------------------------------
    // Mic
    // -----------------------------------------------------------------------
    document.getElementById('mic-toggle').addEventListener('click', async function() {
        if (!Synth.hasMic()) {
            await Synth.initMic();
            if (!Synth.hasMic()) return;
        }
        var enabled = !Synth.isMicEnabled();
        Synth.setMicEnabled(enabled);
        var btn = document.getElementById('mic-toggle');
        btn.classList.toggle('mic-on', enabled);
        btn.classList.toggle('mic-off', !enabled);
    });

    document.getElementById('mic-volume').addEventListener('input', function(e) {
        Synth.setMicVolume(parseInt(e.target.value) / 100);
    });

    // -----------------------------------------------------------------------
    // ADSR sliders (with value readouts)
    // -----------------------------------------------------------------------
    function formatMs(ms) { return ms >= 1000 ? (ms / 1000).toFixed(1) + 's' : Math.round(ms) + 'ms'; }

    document.getElementById('adsr-a').addEventListener('input', function(e) {
        var ms = parseInt(e.target.value);
        var adsr = Synth.getADSR();
        adsr.attack = ms / 1000;
        Synth.setADSR(adsr.attack, adsr.decay, adsr.sustain, adsr.release);
        showVal('adsr-a-val', formatMs(ms));
    });

    document.getElementById('adsr-d').addEventListener('input', function(e) {
        var ms = parseInt(e.target.value);
        var adsr = Synth.getADSR();
        adsr.decay = ms / 1000;
        Synth.setADSR(adsr.attack, adsr.decay, adsr.sustain, adsr.release);
        showVal('adsr-d-val', formatMs(ms));
    });

    document.getElementById('adsr-s').addEventListener('input', function(e) {
        var pct = parseInt(e.target.value);
        var adsr = Synth.getADSR();
        adsr.sustain = pct / 100;
        Synth.setADSR(adsr.attack, adsr.decay, adsr.sustain, adsr.release);
        showVal('adsr-s-val', pct + '%');
    });

    document.getElementById('adsr-r').addEventListener('input', function(e) {
        var ms = parseInt(e.target.value);
        var adsr = Synth.getADSR();
        adsr.release = ms / 1000;
        Synth.setADSR(adsr.attack, adsr.decay, adsr.sustain, adsr.release);
        showVal('adsr-r-val', formatMs(ms));
    });

    // -----------------------------------------------------------------------
    // Filter controls
    // -----------------------------------------------------------------------
    var filterToggle = document.getElementById('filter-toggle');
    filterToggle.addEventListener('click', function() {
        var enabled = !Synth.Filter.isEnabled();
        Synth.Filter.setEnabled(enabled);
        updateToggle(filterToggle, enabled);
    });

    $$('#filter-type-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#filter-type-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            Synth.Filter.setType(btn.getAttribute('data-type'));
        });
    });

    document.getElementById('filter-cutoff').addEventListener('input', function(e) {
        var pct = parseInt(e.target.value) / 100;
        var freq = 20 * Math.pow(1000, pct);
        Synth.Filter.setCutoff(freq);
        showVal('filter-cutoff-val', freq >= 1000 ? (freq / 1000).toFixed(1) + 'kHz' : Math.round(freq) + 'Hz');
    });

    document.getElementById('filter-q').addEventListener('input', function(e) {
        var q = parseInt(e.target.value) / 10;
        Synth.Filter.setQ(q);
        showVal('filter-q-val', q.toFixed(1));
    });

    // -----------------------------------------------------------------------
    // Delay controls
    // -----------------------------------------------------------------------
    var delayToggle = document.getElementById('delay-toggle');
    delayToggle.addEventListener('click', function() {
        var enabled = !Synth.Effects.isDelayEnabled();
        Synth.Effects.setDelayEnabled(enabled);
        updateToggle(delayToggle, enabled);
    });

    document.getElementById('delay-time').addEventListener('input', function(e) {
        var ms = parseInt(e.target.value);
        Synth.Effects.setDelayTime(ms / 1000);
        showVal('delay-time-val', formatMs(ms));
    });

    document.getElementById('delay-feedback').addEventListener('input', function(e) {
        var pct = parseInt(e.target.value);
        Synth.Effects.setDelayFeedback(pct / 100);
        showVal('delay-fb-val', pct + '%');
    });

    document.getElementById('delay-mix').addEventListener('input', function(e) {
        var pct = parseInt(e.target.value);
        Synth.Effects.setDelayMix(pct / 100);
        showVal('delay-mix-val', pct + '%');
    });

    // -----------------------------------------------------------------------
    // LFO controls
    // -----------------------------------------------------------------------
    var lfoToggle = document.getElementById('lfo-toggle');
    lfoToggle.addEventListener('click', function() {
        var enabled = !Synth.LFO.isEnabled();
        Synth.LFO.setEnabled(enabled);
        updateToggle(lfoToggle, enabled);
    });

    document.getElementById('lfo-rate').addEventListener('input', function(e) {
        // Exponential mapping: 0.1Hz to 10Hz over 0-100 slider range
        var pct = parseInt(e.target.value) / 100;
        var hz = 0.1 * Math.pow(100, pct);
        Synth.LFO.setRate(hz);
        showVal('lfo-rate-val', hz < 1 ? hz.toFixed(2) + 'Hz' : hz.toFixed(1) + 'Hz');
    });

    document.getElementById('lfo-depth').addEventListener('input', function(e) {
        var pct = parseInt(e.target.value);
        Synth.LFO.setDepth(pct / 100);
        showVal('lfo-depth-val', pct + '%');
    });

    $$('#lfo-target-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#lfo-target-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            Synth.LFO.setTarget(btn.getAttribute('data-target'));
        });
    });

    // -----------------------------------------------------------------------
    // Presets
    // -----------------------------------------------------------------------
    var presetSelect = document.getElementById('preset-select');

    function populatePresets() {
        presetSelect.innerHTML = '';
        Synth.Presets.list().forEach(function(name) {
            var opt = document.createElement('option');
            opt.value = name;
            opt.textContent = name;
            presetSelect.appendChild(opt);
        });
    }
    populatePresets();

    presetSelect.addEventListener('change', function() {
        Synth.Presets.load(this.value);
        syncUIToState();
    });

    document.getElementById('preset-save').addEventListener('click', function() {
        var name = presetSelect.value;
        if (Synth.Presets.isFactory(name)) name = 'My ' + name;
        Synth.Presets.save(name);
        populatePresets();
        presetSelect.value = name;
    });

    // -----------------------------------------------------------------------
    // Sync UI to state (after preset load)
    // -----------------------------------------------------------------------
    function syncUIToState() {
        // Waveform
        $$('#wave-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-wave') === Synth.getWaveform());
        });
        document.getElementById('volume').value = Math.round(Synth.getVolume() * 100);

        // ADSR
        var adsr = Synth.getADSR();
        var aMs = Math.round(adsr.attack * 1000);
        var dMs = Math.round(adsr.decay * 1000);
        var sPct = Math.round(adsr.sustain * 100);
        var rMs = Math.round(adsr.release * 1000);
        document.getElementById('adsr-a').value = aMs;
        document.getElementById('adsr-d').value = dMs;
        document.getElementById('adsr-s').value = sPct;
        document.getElementById('adsr-r').value = rMs;
        showVal('adsr-a-val', formatMs(aMs));
        showVal('adsr-d-val', formatMs(dMs));
        showVal('adsr-s-val', sPct + '%');
        showVal('adsr-r-val', formatMs(rMs));

        // Filter
        var fState = Synth.Filter.getState();
        updateToggle(filterToggle, fState.enabled);
        $$('#filter-type-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-type') === fState.type);
        });
        var cutPct = Math.log(fState.frequency / 20) / Math.log(1000);
        document.getElementById('filter-cutoff').value = Math.round(cutPct * 100);
        document.getElementById('filter-q').value = Math.round(fState.Q * 10);
        var freq = fState.frequency;
        showVal('filter-cutoff-val', freq >= 1000 ? (freq / 1000).toFixed(1) + 'kHz' : Math.round(freq) + 'Hz');
        showVal('filter-q-val', fState.Q.toFixed(1));

        // Delay
        var dState = Synth.Effects.getState();
        updateToggle(delayToggle, dState.delayEnabled);
        var dtMs = Math.round(dState.delayTime * 1000);
        document.getElementById('delay-time').value = dtMs;
        document.getElementById('delay-feedback').value = Math.round(dState.delayFeedback * 100);
        document.getElementById('delay-mix').value = Math.round(dState.delayMix * 100);
        showVal('delay-time-val', formatMs(dtMs));
        showVal('delay-fb-val', Math.round(dState.delayFeedback * 100) + '%');
        showVal('delay-mix-val', Math.round(dState.delayMix * 100) + '%');

        // LFO
        var lState = Synth.LFO.getState();
        updateToggle(lfoToggle, lState.enabled);
        // Reverse exponential: pct = log(hz/0.1) / log(100)
        var ratePct = Math.log(lState.rate / 0.1) / Math.log(100);
        document.getElementById('lfo-rate').value = Math.round(ratePct * 100);
        document.getElementById('lfo-depth').value = Math.round(lState.depth * 100);
        showVal('lfo-rate-val', lState.rate < 1 ? lState.rate.toFixed(2) + 'Hz' : lState.rate.toFixed(1) + 'Hz');
        showVal('lfo-depth-val', Math.round(lState.depth * 100) + '%');
        $$('#lfo-target-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-target') === lState.target);
        });
    }

    // -----------------------------------------------------------------------
    // Sequencer
    // -----------------------------------------------------------------------
    var seqGrid = document.getElementById('seq-grid');
    var seqStepEls = [];

    for (var i = 0; i < Synth.Sequencer.NUM_STEPS; i++) {
        var step = document.createElement('div');
        step.className = 'seq-step';
        step.setAttribute('data-step', i.toString());
        seqStepEls.push(step);
        seqGrid.appendChild(step);

        (function(idx) {
            step.addEventListener('click', function() {
                var current = Synth.Sequencer.getStep(idx);
                if (current !== null) {
                    Synth.Sequencer.clearStep(idx);
                    this.classList.remove('active');
                    this.textContent = '';
                } else {
                    // Use last played note, or C of current view octave
                    var noteIdx = Synth.getLastPlayedNote();
                    Synth.Sequencer.setStep(idx, noteIdx);
                    this.classList.add('active');
                    this.textContent = Synth.notes[noteIdx].name;
                }
            });
        })(i);
    }

    Synth.Sequencer.onStep(function(step) {
        seqStepEls.forEach(function(el, i) {
            el.classList.toggle('playing', i === step);
        });
    });

    document.getElementById('seq-play').addEventListener('click', function() {
        if (Synth.Sequencer.isPlaying()) {
            Synth.Sequencer.stop();
            this.textContent = 'Play';
        } else {
            Synth.Sequencer.start();
            this.textContent = 'Stop';
        }
    });

    document.getElementById('seq-bpm').addEventListener('input', function(e) {
        Synth.Sequencer.setBPM(parseInt(e.target.value));
        document.getElementById('seq-bpm-display').textContent = e.target.value;
    });

    $$('#seq-mode-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#seq-mode-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            Synth.Sequencer.setMode(btn.getAttribute('data-mode'));
        });
    });

    $$('#arp-pattern-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#arp-pattern-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            Synth.Sequencer.setArpPattern(btn.getAttribute('data-pattern'));
        });
    });

    // Load default preset
    Synth.Presets.load('Init');
    syncUIToState();

    // -----------------------------------------------------------------------
    // View switching
    // -----------------------------------------------------------------------
    var currentView = 'synth';

    $$('#view-tabs .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            var view = btn.getAttribute('data-view');
            if (view === currentView) return;
            currentView = view;
            $$('#view-tabs .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');

            document.getElementById('synth-view').style.display = view === 'synth' ? 'flex' : 'none';
            document.getElementById('editor-view').style.display = view === 'editor' ? 'flex' : 'none';

            if (view === 'editor') {
                Synth.Visualizer.pause();
                Synth.ClipEditor.draw();
            } else {
                Synth.Visualizer.resume();
            }
        });
    });

    // -----------------------------------------------------------------------
    // Clip Editor init & wiring
    // -----------------------------------------------------------------------
    Synth.ClipEditor.init(document.getElementById('editor-canvas'));

    var isRecording = false;

    // Transport
    document.getElementById('ed-play').addEventListener('click', function() { Synth.ClipEditor.play(); });
    document.getElementById('ed-stop').addEventListener('click', function() { Synth.ClipEditor.stop(); });
    document.getElementById('ed-loop').addEventListener('click', function() {
        var on = Synth.ClipEditor.toggleLoop();
        document.getElementById('ed-loop').classList.toggle('active', on);
    });

    // Record
    document.getElementById('ed-record').addEventListener('click', function() {
        var btn = document.getElementById('ed-record');
        if (!isRecording) {
            Synth.ClipEditor.record();
            isRecording = true;
            btn.classList.add('recording');
            btn.textContent = 'Stop';
        } else {
            Synth.ClipEditor.stopRecording();
            isRecording = false;
            btn.classList.remove('recording');
            btn.textContent = 'Rec';
        }
    });

    // File I/O
    document.getElementById('ed-load').addEventListener('click', function() {
        var files = showOpenFileDialog('Audio Files|wav');
        if (files && files.length > 0) {
            try { Synth.ClipEditor.loadFromFile(files[0]); }
            catch (e) { console.error('Load failed:', e.message); }
        }
    });

    document.getElementById('ed-save').addEventListener('click', function() {
        var path = showSaveFileDialog('WAV Files|wav', 'clip.wav');
        if (path) {
            // Ensure .wav extension
            if (path.indexOf('.wav') < 0 && path.indexOf('.WAV') < 0) path += '.wav';
            try { Synth.ClipEditor.saveToFile(path); }
            catch (e) { console.error('Save failed:', e.message); }
        }
    });

    // Edit operations
    document.getElementById('ed-undo').addEventListener('click', function() { Synth.ClipEditor.undo(); });
    document.getElementById('ed-redo').addEventListener('click', function() { Synth.ClipEditor.redo(); });
    document.getElementById('ed-cut').addEventListener('click', function() { Synth.ClipEditor.cut(); });
    document.getElementById('ed-copy').addEventListener('click', function() { Synth.ClipEditor.copy(); });
    document.getElementById('ed-paste').addEventListener('click', function() { Synth.ClipEditor.paste(); });
    document.getElementById('ed-delete').addEventListener('click', function() { Synth.ClipEditor.deleteSelection(); });
    document.getElementById('ed-silence').addEventListener('click', function() { Synth.ClipEditor.silenceSelection(); });
    document.getElementById('ed-trim').addEventListener('click', function() { Synth.ClipEditor.trimToSelection(); });
    document.getElementById('ed-select-all').addEventListener('click', function() { Synth.ClipEditor.selectAll(); });

    // Zoom
    document.getElementById('ed-zoom-in').addEventListener('click', function() { Synth.ClipEditor.zoomIn(); });
    document.getElementById('ed-zoom-out').addEventListener('click', function() { Synth.ClipEditor.zoomOut(); });
    document.getElementById('ed-zoom-fit').addEventListener('click', function() { Synth.ClipEditor.zoomToFit(); });
    document.getElementById('ed-zoom-sel').addEventListener('click', function() { Synth.ClipEditor.zoomToSelection(); });

    // Process
    document.getElementById('ed-normalize').addEventListener('click', function() { Synth.ClipEditor.normalize(); });
    document.getElementById('ed-reverse').addEventListener('click', function() { Synth.ClipEditor.reverse(); });
    document.getElementById('ed-fade-in').addEventListener('click', function() { Synth.ClipEditor.fadeIn(); });
    document.getElementById('ed-fade-out').addEventListener('click', function() { Synth.ClipEditor.fadeOut(); });

    document.getElementById('ed-gain').addEventListener('input', function(e) {
        showVal('ed-gain-val', e.target.value + 'dB');
    });
    document.getElementById('ed-gain-apply').addEventListener('click', function() {
        Synth.ClipEditor.adjustGain(parseInt(document.getElementById('ed-gain').value));
        document.getElementById('ed-gain').value = 0;
        showVal('ed-gain-val', '0dB');
    });

    // Pitch
    document.getElementById('ed-pitch').addEventListener('input', function(e) {
        var v = parseInt(e.target.value);
        showVal('ed-pitch-val', (v >= 0 ? '+' : '') + v);
    });
    document.getElementById('ed-pitch-apply').addEventListener('click', function() {
        var semi = parseInt(document.getElementById('ed-pitch').value);
        if (semi !== 0) Synth.ClipEditor.pitchShift(semi);
        document.getElementById('ed-pitch').value = 0;
        showVal('ed-pitch-val', '0');
    });
    $$('.ed-pitch-btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            Synth.ClipEditor.pitchShift(parseInt(btn.getAttribute('data-semi')));
        });
    });

    // Speed / Time stretch
    document.getElementById('ed-speed').addEventListener('input', function(e) {
        showVal('ed-speed-val', e.target.value + '%');
    });
    document.getElementById('ed-speed-apply').addEventListener('click', function() {
        var pct = parseInt(document.getElementById('ed-speed').value);
        if (pct !== 100) Synth.ClipEditor.timeStretch(100 / pct); // invert: 200% speed = 0.5x stretch
        document.getElementById('ed-speed').value = 100;
        showVal('ed-speed-val', '100%');
    });
    $$('.ed-speed-btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            var pct = parseInt(btn.getAttribute('data-speed'));
            Synth.ClipEditor.timeStretch(100 / pct);
        });
    });

    // Insert silence
    document.getElementById('ed-silence-dur').addEventListener('input', function(e) {
        var ms = parseInt(e.target.value);
        showVal('ed-silence-dur-val', ms >= 1000 ? (ms / 1000).toFixed(1) + 's' : ms + 'ms');
    });
    document.getElementById('ed-insert-silence').addEventListener('click', function() {
        Synth.ClipEditor.insertSilence(parseInt(document.getElementById('ed-silence-dur').value));
    });

    // Generate
    var genWaveform = 'sine';
    document.getElementById('ed-gen-freq').addEventListener('input', function(e) {
        showVal('ed-gen-freq-val', e.target.value + 'Hz');
    });
    document.getElementById('ed-gen-dur').addEventListener('input', function(e) {
        var ms = parseInt(e.target.value);
        showVal('ed-gen-dur-val', ms >= 1000 ? (ms / 1000).toFixed(1) + 's' : ms + 'ms');
    });
    $$('#ed-gen-wave-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#ed-gen-wave-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            genWaveform = btn.getAttribute('data-wave');
        });
    });
    document.getElementById('ed-generate').addEventListener('click', function() {
        Synth.ClipEditor.generateTone(
            parseInt(document.getElementById('ed-gen-freq').value),
            parseInt(document.getElementById('ed-gen-dur').value),
            genWaveform
        );
    });
    document.getElementById('ed-gen-noise').addEventListener('click', function() {
        Synth.ClipEditor.generateNoise(parseInt(document.getElementById('ed-gen-dur').value));
    });

    // Synth integration
    document.getElementById('ed-use-clip').addEventListener('click', function() {
        Synth.ClipEditor.useAsInstrument();
        document.getElementById('ed-use-clip').classList.add('active');
    });
    document.getElementById('ed-clear-clip').addEventListener('click', function() {
        Synth.ClipEditor.clearInstrument();
        document.getElementById('ed-use-clip').classList.remove('active');
    });

    // Keyboard shortcuts for editor view
    document.documentElement.addEventListener('keydown', function(e) {
        if (currentView === 'editor') {
            if (Synth.ClipEditor.handleKey(e)) {
                e.preventDefault();
            }
        }
    });
})();
