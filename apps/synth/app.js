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
    // ADSR sliders
    // -----------------------------------------------------------------------
    function bindADSR(id, param, scale, offset) {
        var el = document.getElementById(id);
        el.addEventListener('input', function(e) {
            var val = parseInt(e.target.value) / scale + (offset || 0);
            var adsr = Synth.getADSR();
            adsr[param] = val;
            Synth.setADSR(adsr.attack, adsr.decay, adsr.sustain, adsr.release);
        });
    }
    // Attack: 1ms - 2s (exponential feel via /1000)
    bindADSR('adsr-a', 'attack', 1000);
    // Decay: 1ms - 2s
    bindADSR('adsr-d', 'decay', 1000);
    // Sustain: 0 - 1
    bindADSR('adsr-s', 'sustain', 100);
    // Release: 1ms - 3s
    bindADSR('adsr-r', 'release', 1000);

    // -----------------------------------------------------------------------
    // Filter controls
    // -----------------------------------------------------------------------
    document.getElementById('filter-toggle').addEventListener('click', function() {
        var enabled = !Synth.Filter.isEnabled();
        Synth.Filter.setEnabled(enabled);
        this.classList.toggle('active', enabled);
    });

    $$('#filter-type-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#filter-type-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            Synth.Filter.setType(btn.getAttribute('data-type'));
        });
    });

    document.getElementById('filter-cutoff').addEventListener('input', function(e) {
        // Exponential mapping: 20Hz - 20kHz
        var pct = parseInt(e.target.value) / 100;
        var freq = 20 * Math.pow(1000, pct);
        Synth.Filter.setCutoff(freq);
    });

    document.getElementById('filter-q').addEventListener('input', function(e) {
        var q = parseInt(e.target.value) / 10;
        Synth.Filter.setQ(q);
    });

    // -----------------------------------------------------------------------
    // Delay controls
    // -----------------------------------------------------------------------
    document.getElementById('delay-toggle').addEventListener('click', function() {
        var enabled = !Synth.Effects.isDelayEnabled();
        Synth.Effects.setDelayEnabled(enabled);
        this.classList.toggle('active', enabled);
    });

    document.getElementById('delay-time').addEventListener('input', function(e) {
        Synth.Effects.setDelayTime(parseInt(e.target.value) / 1000);
    });

    document.getElementById('delay-feedback').addEventListener('input', function(e) {
        Synth.Effects.setDelayFeedback(parseInt(e.target.value) / 100);
    });

    document.getElementById('delay-mix').addEventListener('input', function(e) {
        Synth.Effects.setDelayMix(parseInt(e.target.value) / 100);
    });

    // -----------------------------------------------------------------------
    // LFO controls
    // -----------------------------------------------------------------------
    document.getElementById('lfo-toggle').addEventListener('click', function() {
        var enabled = !Synth.LFO.isEnabled();
        Synth.LFO.setEnabled(enabled);
        this.classList.toggle('active', enabled);
    });

    document.getElementById('lfo-rate').addEventListener('input', function(e) {
        Synth.LFO.setRate(parseInt(e.target.value) / 10);
    });

    document.getElementById('lfo-depth').addEventListener('input', function(e) {
        Synth.LFO.setDepth(parseInt(e.target.value) / 100);
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
        if (Synth.Presets.isFactory(name)) {
            // Don't overwrite factory — prompt for new name
            name = 'My ' + name;
        }
        Synth.Presets.save(name);
        populatePresets();
        presetSelect.value = name;
    });

    // Sync UI controls to current synth state (after preset load)
    function syncUIToState() {
        // Waveform
        $$('#wave-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-wave') === Synth.getWaveform());
        });
        document.getElementById('volume').value = Math.round(Synth.getVolume() * 100);

        // ADSR
        var adsr = Synth.getADSR();
        document.getElementById('adsr-a').value = Math.round(adsr.attack * 1000);
        document.getElementById('adsr-d').value = Math.round(adsr.decay * 1000);
        document.getElementById('adsr-s').value = Math.round(adsr.sustain * 100);
        document.getElementById('adsr-r').value = Math.round(adsr.release * 1000);

        // Filter
        var fState = Synth.Filter.getState();
        document.getElementById('filter-toggle').classList.toggle('active', fState.enabled);
        $$('#filter-type-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-type') === fState.type);
        });
        // Reverse exponential mapping for cutoff slider
        var cutPct = Math.log(fState.frequency / 20) / Math.log(1000);
        document.getElementById('filter-cutoff').value = Math.round(cutPct * 100);
        document.getElementById('filter-q').value = Math.round(fState.Q * 10);

        // Delay
        var dState = Synth.Effects.getState();
        document.getElementById('delay-toggle').classList.toggle('active', dState.delayEnabled);
        document.getElementById('delay-time').value = Math.round(dState.delayTime * 1000);
        document.getElementById('delay-feedback').value = Math.round(dState.delayFeedback * 100);
        document.getElementById('delay-mix').value = Math.round(dState.delayMix * 100);

        // LFO
        var lState = Synth.LFO.getState();
        document.getElementById('lfo-toggle').classList.toggle('active', lState.enabled);
        document.getElementById('lfo-rate').value = Math.round(lState.rate * 10);
        document.getElementById('lfo-depth').value = Math.round(lState.depth * 100);
        $$('#lfo-target-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-target') === lState.target);
        });
    }

    // -----------------------------------------------------------------------
    // Sequencer
    // -----------------------------------------------------------------------
    var seqGrid = document.getElementById('seq-grid');
    var seqStepEls = [];

    // Build 16-step grid
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
                    // Default to C of current view octave
                    var noteIdx = Synth.Keyboard.getViewOffset();
                    Synth.Sequencer.setStep(idx, noteIdx);
                    this.classList.add('active');
                    this.textContent = Synth.notes[noteIdx].name;
                }
            });
        })(i);
    }

    // Highlight current step
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
