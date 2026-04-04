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
})();
