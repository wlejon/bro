// ---------------------------------------------------------------------------
// Synth App — wires all modules together
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    function $$(sel) { return Array.from(document.querySelectorAll(sel)); }

    // Init audio engine
    Synth.init();

    // Init keyboard
    Synth.Keyboard.init(
        document.getElementById('keyboard'),
        document.getElementById('octave-display')
    );

    // Init visualizer
    Synth.Visualizer.init(document.getElementById('viz-stack'));
    Synth.Visualizer.rebuild();
    Synth.Visualizer.draw();

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    function showVal(id, text) {
        var el = document.getElementById(id);
        if (el) el.textContent = text;
    }

    function updateToggle(btn, active) {
        btn.classList.toggle('active', active);
        btn.textContent = active ? 'On' : 'Off';
    }

    function formatMs(ms) {
        return ms >= 1000 ? (ms / 1000).toFixed(1) + 's' : Math.round(ms) + 'ms';
    }

    function formatFreq(freq) {
        return freq >= 1000 ? (freq / 1000).toFixed(1) + 'kHz' : Math.round(freq) + 'Hz';
    }

    function formatLfoRate(hz) {
        return hz < 1 ? hz.toFixed(2) + 'Hz' : hz.toFixed(1) + 'Hz';
    }

    // Exponential cutoff mapping: 0-100 slider -> 20Hz-20kHz
    function cutoffSliderToFreq(v) { return 20 * Math.pow(1000, v / 100); }
    function freqToCutoffSlider(f) { return Math.log(f / 20) / Math.log(1000) * 100; }

    // Exponential LFO rate mapping: 0-100 slider -> 0.1Hz-10Hz
    function lfoSliderToHz(v) { return 0.1 * Math.pow(100, v / 100); }
    function hzToLfoSlider(hz) { return Math.log(hz / 0.1) / Math.log(100) * 100; }

    // -----------------------------------------------------------------------
    // Signal param helpers — write to the active signal (layer or mic)
    // -----------------------------------------------------------------------
    function signalParam(path, value) {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var parts = path.split('.');
        var obj = signal;
        for (var i = 0; i < parts.length - 1; i++) obj = obj[parts[i]];
        obj[parts[parts.length - 1]] = value;
    }

    function activeBusId() {
        return Synth.Layers.getActiveBusId();
    }

    // -----------------------------------------------------------------------
    // Collapsible panels — click header to toggle, auto-expand on enable
    // -----------------------------------------------------------------------
    $$('.panel-collapsible').forEach(function(panel) {
        var header = panel.querySelector('.panel-header');
        if (!header) return;
        header.addEventListener('click', function(e) {
            // Don't toggle when clicking the On/Off button itself
            var tgt = e.target;
            if (tgt && tgt.classList && tgt.classList.contains('btn-toggle')) return;
            panel.classList.toggle('collapsed');
        });
    });

    // Map effect toggle buttons to their panels for auto-expand
    var togglePanelMap = {
        'filter-toggle': 'panel-filter',
        'delay-toggle': 'panel-delay',
        'reverb-toggle': 'panel-reverb',
        'chorus-toggle': 'panel-chorus',
        'comp-toggle': 'panel-compressor',
        'eq-toggle': 'panel-eq',
        'dist-toggle': 'panel-distortion',
        'lfo-toggle': 'panel-lfo'
    };

    function syncPanelCollapse(signal) {
        if (!signal) return;
        var pairs = [
            ['panel-filter', signal.filter && signal.filter.enabled],
            ['panel-delay', signal.delay && signal.delay.enabled],
            ['panel-reverb', signal.reverb && signal.reverb.enabled],
            ['panel-chorus', signal.chorus && signal.chorus.enabled],
            ['panel-compressor', signal.compressor && signal.compressor.enabled],
            ['panel-eq', signal.eq && signal.eq.enabled],
            ['panel-distortion', signal.distortion && signal.distortion.enabled],
            ['panel-lfo', signal.lfo && signal.lfo.enabled]
        ];
        for (var i = 0; i < pairs.length; i++) {
            var panel = document.getElementById(pairs[i][0]);
            if (panel) panel.classList.toggle('collapsed', !pairs[i][1]);
        }
    }

    // -----------------------------------------------------------------------
    // Init layers — create first layer, then apply preset to it
    // -----------------------------------------------------------------------
    Synth.Layers.init();
    Synth.Presets.load('Init');

    // -----------------------------------------------------------------------
    // Fancy Sliders — sidebar synth params
    // -----------------------------------------------------------------------
    var sliders = {};

    sliders.adsrA = Synth.Slider(document.getElementById('adsr-a-slider'), {
        min: 1, max: 2000, value: 10, step: 1, defaultValue: 10,
        format: formatMs,
        onChange: function(ms) {
            signalParam('adsr.attack', ms / 1000);
            Synth.Layers.updateActiveAllocator();
        }
    });

    sliders.adsrD = Synth.Slider(document.getElementById('adsr-d-slider'), {
        min: 1, max: 2000, value: 100, step: 1, defaultValue: 100,
        format: formatMs,
        onChange: function(ms) {
            signalParam('adsr.decay', ms / 1000);
            Synth.Layers.updateActiveAllocator();
        }
    });

    sliders.adsrS = Synth.Slider(document.getElementById('adsr-s-slider'), {
        min: 0, max: 100, value: 100, step: 1, defaultValue: 100,
        format: function(v) { return v + '%'; },
        onChange: function(pct) {
            signalParam('adsr.sustain', pct / 100);
            Synth.Layers.updateActiveAllocator();
        }
    });

    sliders.adsrR = Synth.Slider(document.getElementById('adsr-r-slider'), {
        min: 1, max: 3000, value: 80, step: 1, defaultValue: 80,
        format: formatMs,
        onChange: function(ms) {
            signalParam('adsr.release', ms / 1000);
            Synth.Layers.updateActiveAllocator();
        }
    });

    // -----------------------------------------------------------------------
    // Unison sliders
    // -----------------------------------------------------------------------
    sliders.unisonCount = Synth.Slider(document.getElementById('unison-count-slider'), {
        min: 1, max: 8, value: 1, step: 1, defaultValue: 1,
        format: function(v) { return v === 1 ? 'Off' : v + 'x'; },
        onChange: function(v) {
            signalParam('unison.count', v);
            Synth.Layers.updateActiveAllocator();
        }
    });

    sliders.unisonDetune = Synth.Slider(document.getElementById('unison-detune-slider'), {
        min: 0, max: 200, value: 15, step: 1, defaultValue: 15,
        format: function(v) { return (v / 100).toFixed(2) + ' st'; },
        onChange: function(v) {
            signalParam('unison.detune', v / 100);
            Synth.Layers.updateActiveAllocator();
        }
    });

    sliders.unisonWidth = Synth.Slider(document.getElementById('unison-width-slider'), {
        min: 0, max: 100, value: 70, step: 1, defaultValue: 70,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('unison.stereoWidth', v / 100);
            Synth.Layers.updateActiveAllocator();
        }
    });

    sliders.filterCutoff = Synth.Slider(document.getElementById('filter-cutoff-slider'), {
        min: 0, max: 100, value: 50, step: 0.5, defaultValue: 50,
        format: function(v) { return formatFreq(cutoffSliderToFreq(v)); },
        onChange: function(v) {
            var freq = cutoffSliderToFreq(v);
            signalParam('filter.frequency', freq);
            Synth.SignalChain.setFilterFrequency(activeBusId(), freq);
        }
    });

    sliders.filterQ = Synth.Slider(document.getElementById('filter-q-slider'), {
        min: 1, max: 200, value: 10, step: 1, defaultValue: 10,
        format: function(v) { return (v / 10).toFixed(1); },
        onChange: function(v) {
            signalParam('filter.Q', v / 10);
            Synth.SignalChain.setFilterQ(activeBusId(), v / 10);
        }
    });

    sliders.delayTime = Synth.Slider(document.getElementById('delay-time-slider'), {
        min: 10, max: 1500, value: 300, step: 1, defaultValue: 300,
        format: formatMs,
        onChange: function(ms) {
            signalParam('delay.time', ms / 1000);
            Synth.SignalChain.setDelayTime(activeBusId(), ms / 1000);
        }
    });

    sliders.delayFb = Synth.Slider(document.getElementById('delay-fb-slider'), {
        min: 0, max: 90, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('delay.feedback', v / 100);
            Synth.SignalChain.setDelayFeedback(activeBusId(), v / 100);
        }
    });

    sliders.delayMix = Synth.Slider(document.getElementById('delay-mix-slider'), {
        min: 0, max: 100, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('delay.mix', v / 100);
            Synth.SignalChain.setDelayMix(activeBusId(), v / 100);
        }
    });

    // -----------------------------------------------------------------------
    // Reverb sliders
    // -----------------------------------------------------------------------
    sliders.reverbRoom = Synth.Slider(document.getElementById('reverb-room-slider'), {
        min: 0, max: 100, value: 50, step: 1, defaultValue: 50,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('reverb.roomSize', v / 100);
            Synth.SignalChain.setReverbRoomSize(activeBusId(), v / 100);
        }
    });

    sliders.reverbDamp = Synth.Slider(document.getElementById('reverb-damp-slider'), {
        min: 0, max: 100, value: 50, step: 1, defaultValue: 50,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('reverb.damping', v / 100);
            Synth.SignalChain.setReverbDamping(activeBusId(), v / 100);
        }
    });

    sliders.reverbMix = Synth.Slider(document.getElementById('reverb-mix-slider'), {
        min: 0, max: 100, value: 20, step: 1, defaultValue: 20,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('reverb.mix', v / 100);
            Synth.SignalChain.setReverbMix(activeBusId(), v / 100);
        }
    });

    // -----------------------------------------------------------------------
    // Chorus sliders
    // -----------------------------------------------------------------------
    sliders.chorusRate = Synth.Slider(document.getElementById('chorus-rate-slider'), {
        min: 1, max: 100, value: 20, step: 1, defaultValue: 20,
        format: function(v) { return (v / 10).toFixed(1) + 'Hz'; },
        onChange: function(v) {
            signalParam('chorus.rate', v / 10);
            Synth.SignalChain.setChorusRate(activeBusId(), v / 10);
        }
    });

    sliders.chorusDepth = Synth.Slider(document.getElementById('chorus-depth-slider'), {
        min: 0, max: 100, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('chorus.depth', v / 10000);
            Synth.SignalChain.setChorusDepth(activeBusId(), v / 10000);
        }
    });

    sliders.chorusMix = Synth.Slider(document.getElementById('chorus-mix-slider'), {
        min: 0, max: 100, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('chorus.mix', v / 100);
            Synth.SignalChain.setChorusMix(activeBusId(), v / 100);
        }
    });

    // -----------------------------------------------------------------------
    // Compressor sliders
    // -----------------------------------------------------------------------
    sliders.compThresh = Synth.Slider(document.getElementById('comp-thresh-slider'), {
        min: -60, max: 0, value: -12, step: 1, defaultValue: -12,
        format: function(v) { return v + 'dB'; },
        onChange: function(v) {
            signalParam('compressor.threshold', v);
            Synth.SignalChain.setCompressorThreshold(activeBusId(), v);
        }
    });

    sliders.compRatio = Synth.Slider(document.getElementById('comp-ratio-slider'), {
        min: 10, max: 200, value: 40, step: 1, defaultValue: 40,
        format: function(v) { return (v / 10).toFixed(1) + ':1'; },
        onChange: function(v) {
            signalParam('compressor.ratio', v / 10);
            Synth.SignalChain.setCompressorRatio(activeBusId(), v / 10);
        }
    });

    // -----------------------------------------------------------------------
    // Equalizer sliders (7 bands + master)
    // -----------------------------------------------------------------------
    var EQ_BAND_LABELS = ['60Hz', '170Hz', '350Hz', '1kHz', '3.5kHz', '10kHz', '16kHz'];
    sliders.eqBands = [];
    for (var bi = 0; bi < 7; bi++) {
        (function(band) {
            sliders.eqBands[band] = Synth.Slider(document.getElementById('eq-band' + band + '-slider'), {
                min: -12, max: 12, value: 0, step: 0.5, defaultValue: 0,
                format: function(v) { return (v > 0 ? '+' : '') + v.toFixed(1) + 'dB'; },
                onChange: function(v) {
                    var signal = Synth.Layers.getActiveSignal();
                    if (signal && signal.eq) signal.eq.bands[band] = v;
                    Synth.SignalChain.setEqBandGain(activeBusId(), band, v);
                }
            });
        })(bi);
    }

    sliders.eqMaster = Synth.Slider(document.getElementById('eq-master-slider'), {
        min: -12, max: 12, value: 0, step: 0.5, defaultValue: 0,
        format: function(v) { return (v > 0 ? '+' : '') + v.toFixed(1) + 'dB'; },
        onChange: function(v) {
            signalParam('eq.masterGain', v);
            Synth.SignalChain.setEqMasterGain(activeBusId(), v);
        }
    });

    // -----------------------------------------------------------------------
    // Distortion sliders
    // -----------------------------------------------------------------------
    sliders.distDrive = Synth.Slider(document.getElementById('dist-drive-slider'), {
        min: 10, max: 500, value: 25, step: 1, defaultValue: 25,
        format: function(v) { return (v / 10).toFixed(1) + 'x'; },
        onChange: function(v) {
            signalParam('distortion.drive', v / 10);
            Synth.SignalChain.setDistortionDrive(activeBusId(), v / 10);
        }
    });

    sliders.distMix = Synth.Slider(document.getElementById('dist-mix-slider'), {
        min: 0, max: 100, value: 100, step: 1, defaultValue: 100,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('distortion.mix', v / 100);
            Synth.SignalChain.setDistortionMix(activeBusId(), v / 100);
        }
    });

    sliders.distOutput = Synth.Slider(document.getElementById('dist-output-slider'), {
        min: 0, max: 200, value: 70, step: 1, defaultValue: 70,
        format: function(v) { return (v / 100).toFixed(2) + 'x'; },
        onChange: function(v) {
            signalParam('distortion.outputGain', v / 100);
            Synth.SignalChain.setDistortionOutputGain(activeBusId(), v / 100);
        }
    });

    sliders.distBits = Synth.Slider(document.getElementById('dist-bits-slider'), {
        min: 1, max: 16, value: 8, step: 1, defaultValue: 8,
        format: function(v) { return v + ' bit'; },
        onChange: function(v) {
            signalParam('distortion.crushBits', v);
            Synth.SignalChain.setDistortionCrushBits(activeBusId(), v);
        }
    });

    sliders.distRate = Synth.Slider(document.getElementById('dist-rate-slider'), {
        min: 1, max: 100, value: 50, step: 1, defaultValue: 50,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('distortion.crushRate', v / 100);
            Synth.SignalChain.setDistortionCrushRate(activeBusId(), v / 100);
        }
    });

    // -----------------------------------------------------------------------
    // Sidechain select
    // -----------------------------------------------------------------------
    var sidechainSelect = document.getElementById('comp-sidechain-select');
    sidechainSelect.addEventListener('change', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var scBusId = parseInt(sidechainSelect.value, 10);
        signal.compressor.sidechainBusId = scBusId;
        Synth.SignalChain.setCompressorSidechain(activeBusId(), scBusId);
    });

    function rebuildSidechainOptions() {
        var current = sidechainSelect.value;
        sidechainSelect.innerHTML = '<option value="-1">Self</option>';
        var count = Synth.Layers.count();
        var activeIdx = Synth.Layers.getActiveIndex();
        for (var i = 0; i < count; i++) {
            if (i === activeIdx && !Synth.Layers.isEditingMic()) continue;
            var layer = Synth.Layers.get(i);
            var opt = document.createElement('option');
            opt.value = layer.busId;
            opt.textContent = layer.name;
            sidechainSelect.appendChild(opt);
        }
        sidechainSelect.value = current;
    }

    sliders.lfoRate = Synth.Slider(document.getElementById('lfo-rate-slider'), {
        min: 0, max: 100, value: 30, step: 0.5, defaultValue: 30,
        format: function(v) { return formatLfoRate(lfoSliderToHz(v)); },
        onChange: function(v) {
            var hz = lfoSliderToHz(v);
            signalParam('lfo.rate', hz);
            Synth.LFO.setRate(hz);
        }
    });

    sliders.lfoDepth = Synth.Slider(document.getElementById('lfo-depth-slider'), {
        min: 0, max: 100, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            signalParam('lfo.depth', v / 100);
            Synth.LFO.setDepth(v / 100);
        }
    });

    // -----------------------------------------------------------------------
    // Waveform buttons (per-layer)
    // -----------------------------------------------------------------------
    $$('#wave-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#wave-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var wf = btn.getAttribute('data-wave');
            signalParam('waveform', wf);
            Synth.Layers.updateActiveAllocator();
        });
    });

    // -----------------------------------------------------------------------
    // Pan slider (per-layer)
    // -----------------------------------------------------------------------
    sliders.pan = Synth.Slider(document.getElementById('pan-slider'), {
        min: -100, max: 100, value: 0, step: 1, defaultValue: 0,
        format: function(v) {
            if (v === 0) return 'C';
            return v < 0 ? 'L' + Math.abs(v) : 'R' + v;
        },
        onChange: function(v) {
            var pan = v / 100;
            signalParam('pan', pan);
            Synth.Layers.updateActiveAllocator();
        }
    });

    // -----------------------------------------------------------------------
    // Volume (global, not per-layer)
    // -----------------------------------------------------------------------
    document.getElementById('volume').addEventListener('input', function(e) {
        Synth.setVolume(parseInt(e.target.value) / 100);
    });

    // -----------------------------------------------------------------------
    // Mic — toggle + signal selection
    // -----------------------------------------------------------------------
    document.getElementById('mic-toggle').addEventListener('click', async function() {
        if (!Synth.hasMic()) {
            await Synth.initMic();
            if (!Synth.hasMic()) return;
            // Create mic bus on first enable
            Synth.Layers.initMicBus();
        }
        var enabled = !Synth.isMicEnabled();
        Synth.setMicEnabled(enabled);
        var btn = document.getElementById('mic-toggle');
        btn.classList.toggle('mic-on', enabled);
        btn.classList.toggle('mic-off', !enabled);

        // Double-click on active mic button selects mic for sidebar editing
        if (enabled && Synth.Layers.getMicSignal()) {
            Synth.Layers.selectMic();
        }
    });

    document.getElementById('mic-volume').addEventListener('input', function(e) {
        Synth.setMicVolume(parseInt(e.target.value) / 100);
    });

    // -----------------------------------------------------------------------
    // Filter toggle & type
    // -----------------------------------------------------------------------
    var filterToggle = document.getElementById('filter-toggle');
    filterToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var enabled = !signal.filter.enabled;
        signal.filter.enabled = enabled;
        Synth.SignalChain.setFilterEnabled(activeBusId(), enabled);
        updateToggle(filterToggle, enabled);
        if (enabled) document.getElementById('panel-filter').classList.remove('collapsed');
    });

    $$('#filter-type-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#filter-type-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var type = btn.getAttribute('data-type');
            signalParam('filter.type', type);
            Synth.SignalChain.setFilterType(activeBusId(), type);
        });
    });

    // -----------------------------------------------------------------------
    // Delay toggle
    // -----------------------------------------------------------------------
    var delayToggle = document.getElementById('delay-toggle');
    delayToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var enabled = !signal.delay.enabled;
        signal.delay.enabled = enabled;
        Synth.SignalChain.setDelayEnabled(activeBusId(), enabled);
        updateToggle(delayToggle, enabled);
        if (enabled) document.getElementById('panel-delay').classList.remove('collapsed');
    });

    // -----------------------------------------------------------------------
    // Reverb toggle
    // -----------------------------------------------------------------------
    var reverbToggle = document.getElementById('reverb-toggle');
    reverbToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var enabled = !signal.reverb.enabled;
        signal.reverb.enabled = enabled;
        Synth.SignalChain.setReverbEnabled(activeBusId(), enabled);
        updateToggle(reverbToggle, enabled);
        if (enabled) document.getElementById('panel-reverb').classList.remove('collapsed');
    });

    // -----------------------------------------------------------------------
    // Chorus toggle
    // -----------------------------------------------------------------------
    var chorusToggle = document.getElementById('chorus-toggle');
    chorusToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var enabled = !signal.chorus.enabled;
        signal.chorus.enabled = enabled;
        Synth.SignalChain.setChorusEnabled(activeBusId(), enabled);
        updateToggle(chorusToggle, enabled);
        if (enabled) document.getElementById('panel-chorus').classList.remove('collapsed');
    });

    // -----------------------------------------------------------------------
    // Compressor toggle
    // -----------------------------------------------------------------------
    var compToggle = document.getElementById('comp-toggle');
    compToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var enabled = !signal.compressor.enabled;
        signal.compressor.enabled = enabled;
        Synth.SignalChain.setCompressorEnabled(activeBusId(), enabled);
        updateToggle(compToggle, enabled);
        if (enabled) document.getElementById('panel-compressor').classList.remove('collapsed');
    });

    // -----------------------------------------------------------------------
    // EQ toggle
    // -----------------------------------------------------------------------
    var eqToggle = document.getElementById('eq-toggle');
    eqToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var enabled = !signal.eq.enabled;
        signal.eq.enabled = enabled;
        Synth.SignalChain.setEqEnabled(activeBusId(), enabled);
        updateToggle(eqToggle, enabled);
        if (enabled) document.getElementById('panel-eq').classList.remove('collapsed');
    });

    // -----------------------------------------------------------------------
    // Distortion toggle + mode
    // -----------------------------------------------------------------------
    var distToggle = document.getElementById('dist-toggle');
    distToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal || !signal.distortion) return;
        var enabled = !signal.distortion.enabled;
        signal.distortion.enabled = enabled;
        Synth.SignalChain.setDistortionEnabled(activeBusId(), enabled);
        updateToggle(distToggle, enabled);
        if (enabled) document.getElementById('panel-distortion').classList.remove('collapsed');
    });

    $$('#dist-mode-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#dist-mode-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var mode = btn.getAttribute('data-mode');
            signalParam('distortion.mode', mode);
            Synth.SignalChain.setDistortionMode(activeBusId(), mode);
        });
    });

    // -----------------------------------------------------------------------
    // LFO toggle & target
    // -----------------------------------------------------------------------
    var lfoToggle = document.getElementById('lfo-toggle');
    lfoToggle.addEventListener('click', function() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;
        var enabled = !signal.lfo.enabled;
        signal.lfo.enabled = enabled;
        Synth.LFO.setEnabled(enabled);
        updateToggle(lfoToggle, enabled);
        if (enabled) document.getElementById('panel-lfo').classList.remove('collapsed');
    });

    $$('#lfo-target-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#lfo-target-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var t = btn.getAttribute('data-target');
            Synth.LFO.setTarget(t);
            signalParam('lfo.target', t);
        });
    });

    // -----------------------------------------------------------------------
    // Effect Order — draggable reorder list
    // -----------------------------------------------------------------------
    var EFFECT_NAMES = ['filter', 'delay', 'compressor', 'chorus', 'reverb', 'equalizer', 'distortion'];
    var EFFECT_LABELS = { filter: 'Filter', delay: 'Delay', compressor: 'Compressor', chorus: 'Chorus', reverb: 'Reverb', equalizer: 'Equalizer', distortion: 'Distortion' };

    function buildEffectOrderList() {
        var container = document.getElementById('effect-order-list');
        container.innerHTML = '';
        var signal = Synth.Layers.getActiveSignal();
        var order = (signal && signal.effectOrder) ? signal.effectOrder : EFFECT_NAMES.slice();

        for (var i = 0; i < order.length; i++) {
            (function(idx) {
                var item = document.createElement('div');
                item.className = 'effect-order-item';
                item.setAttribute('draggable', 'true');
                item.setAttribute('data-idx', idx.toString());

                var num = document.createElement('span');
                num.className = 'effect-order-num';
                num.textContent = (idx + 1) + '.';
                item.appendChild(num);

                var label = document.createElement('span');
                label.textContent = EFFECT_LABELS[order[idx]] || order[idx];
                item.appendChild(label);

                var arrows = document.createElement('span');
                arrows.className = 'effect-order-arrows';
                if (idx > 0) {
                    var up = document.createElement('button');
                    up.className = 'btn effect-order-btn';
                    up.textContent = '\u25B2';
                    up.addEventListener('click', function(e) {
                        e.stopPropagation();
                        swapEffectOrder(idx, idx - 1);
                    });
                    arrows.appendChild(up);
                }
                if (idx < order.length - 1) {
                    var dn = document.createElement('button');
                    dn.className = 'btn effect-order-btn';
                    dn.textContent = '\u25BC';
                    dn.addEventListener('click', function(e) {
                        e.stopPropagation();
                        swapEffectOrder(idx, idx + 1);
                    });
                    arrows.appendChild(dn);
                }
                item.appendChild(arrows);

                container.appendChild(item);
            })(i);
        }
    }

    function swapEffectOrder(fromIdx, toIdx) {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal || !signal.effectOrder) return;
        var order = signal.effectOrder;
        var tmp = order[fromIdx];
        order[fromIdx] = order[toIdx];
        order[toIdx] = tmp;
        Synth.SignalChain.setEffectOrder(Synth.Layers.getActiveBusId(), order);
        buildEffectOrderList();
    }

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
        // Re-init layer first, then load preset into it
        Synth.Layers.init();
        Synth.Presets.load(this.value);
        syncUIToSignal();
        buildLayerRows();
    });

    document.getElementById('preset-save').addEventListener('click', function() {
        var name = presetSelect.value;
        if (Synth.Presets.isFactory(name)) name = 'My ' + name;
        Synth.Presets.save(name);
        populatePresets();
        presetSelect.value = name;
    });

    // -----------------------------------------------------------------------
    // Sync UI to active signal (layer or mic)
    // -----------------------------------------------------------------------
    function syncUIToSignal() {
        var signal = Synth.Layers.getActiveSignal();
        if (!signal) return;

        var isMic = Synth.Layers.isEditingMic();

        // Layer indicator
        document.getElementById('layer-indicator-color').style.background = signal.color || '#00e5ff';
        document.getElementById('layer-indicator-name').textContent = signal.name || 'Layer 1';

        // Waveform (layers only)
        if (!isMic) {
            $$('#wave-btns .btn').forEach(function(b) {
                b.classList.toggle('active', b.getAttribute('data-wave') === signal.waveform);
            });
        }

        // Volume (global)
        document.getElementById('volume').value = Math.round(Synth.getVolume() * 100);

        // Pan (layers only)
        if (!isMic && signal.pan !== undefined) {
            sliders.pan.setValue(Math.round(signal.pan * 100), true);
        }

        // ADSR (layers only)
        if (!isMic && signal.adsr) {
            sliders.adsrA.setValue(Math.round(signal.adsr.attack * 1000), true);
            sliders.adsrD.setValue(Math.round(signal.adsr.decay * 1000), true);
            sliders.adsrS.setValue(Math.round(signal.adsr.sustain * 100), true);
            sliders.adsrR.setValue(Math.round(signal.adsr.release * 1000), true);
        }

        // Unison (layers only)
        if (!isMic && signal.unison) {
            sliders.unisonCount.setValue(signal.unison.count, true);
            sliders.unisonDetune.setValue(Math.round(signal.unison.detune * 100), true);
            sliders.unisonWidth.setValue(Math.round(signal.unison.stereoWidth * 100), true);
        }

        // Effect order
        buildEffectOrderList();

        // Collapse/expand panels based on enabled state
        syncPanelCollapse(signal);

        // Filter
        updateToggle(filterToggle, signal.filter.enabled);
        $$('#filter-type-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-type') === signal.filter.type);
        });
        sliders.filterCutoff.setValue(Math.round(freqToCutoffSlider(signal.filter.frequency)), true);
        sliders.filterQ.setValue(Math.round(signal.filter.Q * 10), true);

        // Delay
        updateToggle(delayToggle, signal.delay.enabled);
        sliders.delayTime.setValue(Math.round(signal.delay.time * 1000), true);
        sliders.delayFb.setValue(Math.round(signal.delay.feedback * 100), true);
        sliders.delayMix.setValue(Math.round(signal.delay.mix * 100), true);

        // Reverb
        updateToggle(reverbToggle, signal.reverb.enabled);
        sliders.reverbRoom.setValue(Math.round(signal.reverb.roomSize * 100), true);
        sliders.reverbDamp.setValue(Math.round(signal.reverb.damping * 100), true);
        sliders.reverbMix.setValue(Math.round(signal.reverb.mix * 100), true);

        // Chorus
        updateToggle(chorusToggle, signal.chorus.enabled);
        sliders.chorusRate.setValue(Math.round(signal.chorus.rate * 10), true);
        sliders.chorusDepth.setValue(Math.round(signal.chorus.depth * 10000), true);
        sliders.chorusMix.setValue(Math.round(signal.chorus.mix * 100), true);

        // Compressor
        updateToggle(compToggle, signal.compressor.enabled);
        sliders.compThresh.setValue(Math.round(signal.compressor.threshold), true);
        sliders.compRatio.setValue(Math.round(signal.compressor.ratio * 10), true);
        rebuildSidechainOptions();
        sidechainSelect.value = signal.compressor.sidechainBusId !== undefined ? signal.compressor.sidechainBusId : -1;

        // Equalizer
        updateToggle(eqToggle, signal.eq.enabled);
        for (var bi = 0; bi < 7; bi++) {
            sliders.eqBands[bi].setValue(signal.eq.bands[bi], true);
        }
        sliders.eqMaster.setValue(signal.eq.masterGain, true);

        // Distortion
        if (signal.distortion) {
            updateToggle(distToggle, signal.distortion.enabled);
            $$('#dist-mode-btns .btn').forEach(function(b) {
                b.classList.toggle('active', b.getAttribute('data-mode') === signal.distortion.mode);
            });
            sliders.distDrive.setValue(Math.round(signal.distortion.drive * 10), true);
            sliders.distMix.setValue(Math.round(signal.distortion.mix * 100), true);
            sliders.distOutput.setValue(Math.round(signal.distortion.outputGain * 100), true);
            sliders.distBits.setValue(Math.round(signal.distortion.crushBits), true);
            sliders.distRate.setValue(Math.round(signal.distortion.crushRate * 100), true);
        }

        // LFO
        updateToggle(lfoToggle, signal.lfo.enabled);
        sliders.lfoRate.setValue(Math.round(hzToLfoSlider(signal.lfo.rate)), true);
        sliders.lfoDepth.setValue(Math.round(signal.lfo.depth * 100), true);
        $$('#lfo-target-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-target') === signal.lfo.target);
        });

        // Seq/Arp mode (per-layer, not mic)
        if (!isMic) {
            $$('#seq-mode-btns .btn').forEach(function(b) {
                b.classList.toggle('active', b.getAttribute('data-mode') === signal.mode);
            });
            $$('#arp-pattern-btns .btn').forEach(function(b) {
                b.classList.toggle('active', b.getAttribute('data-pattern') === signal.arpPattern);
            });
        }

        // Highlight selected layer row
        $$('.seq-layer-row').forEach(function(row, i) {
            row.classList.toggle('selected', !isMic && i === Synth.Layers.getActiveIndex());
        });
    }

    // Respond to layer selection changes
    Synth.Layers.onSelect(function() { syncUIToSignal(); });

    // -----------------------------------------------------------------------
    // Sequencer — multi-layer grid
    // -----------------------------------------------------------------------
    var seqLayersEl = document.getElementById('seq-layers');

    function buildLayerRows() {
        seqLayersEl.innerHTML = '';
        var count = Synth.Layers.count();

        for (var li = 0; li < count; li++) {
            (function(layerIdx) {
                var layer = Synth.Layers.get(layerIdx);
                var row = document.createElement('div');
                row.className = 'seq-layer-row';
                if (!Synth.Layers.isEditingMic() && layerIdx === Synth.Layers.getActiveIndex()) row.classList.add('selected');

                // Label (click to select)
                var label = document.createElement('div');
                label.className = 'seq-layer-label';
                label.style.borderLeftColor = layer.color;
                label.textContent = layer.name;
                label.addEventListener('click', function() {
                    Synth.Layers.select(layerIdx);
                    buildLayerRows();
                });
                row.appendChild(label);

                // Mode toggle (S = sequencer, A = arpeggiator)
                var modeBtn = document.createElement('div');
                modeBtn.className = 'seq-layer-mode' + (layer.mode === 'arpeggiator' ? ' arp' : '');
                modeBtn.textContent = layer.mode === 'arpeggiator' ? 'A' : 'S';
                modeBtn.title = 'Toggle Seq/Arp';
                modeBtn.addEventListener('click', function(e) {
                    e.stopPropagation();
                    layer.mode = layer.mode === 'arpeggiator' ? 'sequencer' : 'arpeggiator';
                    layer.arpIndex = 0;
                    modeBtn.textContent = layer.mode === 'arpeggiator' ? 'A' : 'S';
                    modeBtn.classList.toggle('arp', layer.mode === 'arpeggiator');
                    // Sync sidebar buttons if this is the active layer
                    if (layerIdx === Synth.Layers.getActiveIndex()) {
                        $$('#seq-mode-btns .btn').forEach(function(b) {
                            b.classList.toggle('active', b.getAttribute('data-mode') === layer.mode);
                        });
                    }
                });
                row.appendChild(modeBtn);

                // Mute button
                var muteBtn = document.createElement('div');
                muteBtn.className = 'seq-layer-mute' + (layer.muted ? ' muted' : '');
                muteBtn.textContent = 'M';
                muteBtn.title = 'Mute';
                muteBtn.addEventListener('click', function(e) {
                    e.stopPropagation();
                    layer.muted = !layer.muted;
                    muteBtn.classList.toggle('muted', layer.muted);
                    if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                });
                row.appendChild(muteBtn);

                // Duplicate button
                var dupBtn = document.createElement('div');
                dupBtn.className = 'seq-layer-dup';
                dupBtn.textContent = 'D';
                dupBtn.title = 'Duplicate';
                dupBtn.addEventListener('click', function(e) {
                    e.stopPropagation();
                    if (Synth.Layers.duplicate(layerIdx)) {
                        buildLayerRows();
                    }
                });
                row.appendChild(dupBtn);

                // Delete button (only if >1 layer)
                if (count > 1) {
                    var delBtn = document.createElement('div');
                    delBtn.className = 'seq-layer-del';
                    delBtn.textContent = 'X';
                    delBtn.title = 'Delete';
                    delBtn.addEventListener('click', function(e) {
                        e.stopPropagation();
                        Synth.Layers.remove(layerIdx);
                        buildLayerRows();
                    });
                    row.appendChild(delBtn);
                }

                // Level meter
                var meter = document.createElement('div');
                meter.className = 'seq-layer-meter';
                meter.setAttribute('data-bus', layer.busId.toString());
                var meterL = document.createElement('div');
                meterL.className = 'meter-bar meter-l';
                var meterR = document.createElement('div');
                meterR.className = 'meter-bar meter-r';
                meter.appendChild(meterL);
                meter.appendChild(meterR);
                row.appendChild(meter);

                // Step grid
                var grid = document.createElement('div');
                grid.className = 'seq-layer-grid';

                for (var si = 0; si < Synth.Sequencer.NUM_STEPS; si++) {
                    (function(stepIdx) {
                        var stepEl = document.createElement('div');
                        stepEl.className = 'seq-step';
                        stepEl.setAttribute('data-layer', layerIdx.toString());
                        stepEl.setAttribute('data-step', stepIdx.toString());

                        var noteIdx = layer.steps[stepIdx];
                        if (noteIdx !== null && noteIdx !== undefined) {
                            stepEl.classList.add('active');
                            stepEl.style.background = layer.color + '25';
                            stepEl.style.borderColor = layer.color + '60';
                            stepEl.style.color = layer.color;
                            stepEl.textContent = Synth.notes[noteIdx] ? Synth.notes[noteIdx].name : '';
                        }

                        stepEl.addEventListener('click', function() {
                            var current = Synth.Layers.getStep(layerIdx, stepIdx);
                            if (current !== null) {
                                Synth.Layers.clearStep(layerIdx, stepIdx);
                                stepEl.classList.remove('active');
                                stepEl.style.background = '';
                                stepEl.style.borderColor = '';
                                stepEl.style.color = '';
                                stepEl.textContent = '';
                            } else {
                                // Select this layer first
                                if (layerIdx !== Synth.Layers.getActiveIndex()) {
                                    Synth.Layers.select(layerIdx);
                                    buildLayerRows();
                                    return; // rebuild will re-render; let user click again
                                }
                                var ni = Synth.getLastPlayedNote();
                                Synth.Layers.setStep(layerIdx, stepIdx, ni);
                                stepEl.classList.add('active');
                                stepEl.style.background = layer.color + '25';
                                stepEl.style.borderColor = layer.color + '60';
                                stepEl.style.color = layer.color;
                                stepEl.textContent = Synth.notes[ni] ? Synth.notes[ni].name : '';
                            }
                            // Update running sequences with new step data
                            if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                        });

                        grid.appendChild(stepEl);
                    })(si);
                }

                row.appendChild(grid);
                seqLayersEl.appendChild(row);

                // --- Automation row ---
                var autoRow = document.createElement('div');
                autoRow.className = 'seq-auto-row';

                // Toggle button to show/hide automation
                var autoToggle = document.createElement('div');
                autoToggle.className = 'seq-auto-toggle' + (layer.automation.length > 0 ? ' active' : '');
                autoToggle.textContent = 'A';
                autoToggle.title = 'Toggle automation';
                autoToggle.addEventListener('click', function() {
                    var l = Synth.Layers.get(layerIdx);
                    if (l.automation.length === 0) {
                        // Add a default automation lane (filter freq)
                        var tgt = 'filter-freq';
                        var def = Synth.Layers.AUTOMATION_TARGETS[tgt];
                        l.automation.push({
                            target: tgt,
                            interpMode: 'linear',
                            points: []
                        });
                    } else {
                        l.automation = [];
                    }
                    buildLayerRows();
                    if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                });
                autoRow.appendChild(autoToggle);

                if (layer.automation.length > 0) {
                    var autoData = layer.automation[0]; // one lane per layer for now
                    var targets = Synth.Layers.AUTOMATION_TARGETS;

                    // Target selector
                    var targetSel = document.createElement('select');
                    targetSel.className = 'seq-auto-target';
                    var targetKeys = Object.keys(targets);
                    for (var ti = 0; ti < targetKeys.length; ti++) {
                        var opt = document.createElement('option');
                        opt.value = targetKeys[ti];
                        opt.textContent = targets[targetKeys[ti]].label;
                        if (targetKeys[ti] === autoData.target) opt.selected = true;
                        targetSel.appendChild(opt);
                    }
                    targetSel.addEventListener('change', function() {
                        var l = Synth.Layers.get(layerIdx);
                        l.automation[0].target = this.value;
                        l.automation[0].points = [];
                        buildLayerRows();
                        if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                    });
                    autoRow.appendChild(targetSel);

                    // Interp mode button
                    var interpBtn = document.createElement('div');
                    interpBtn.className = 'seq-auto-interp';
                    var modes = ['linear', 'smooth', 'step'];
                    interpBtn.textContent = autoData.interpMode === 'smooth' ? 'S' : autoData.interpMode === 'step' ? 'H' : 'L';
                    interpBtn.title = 'Interpolation: ' + autoData.interpMode;
                    interpBtn.addEventListener('click', function() {
                        var l = Synth.Layers.get(layerIdx);
                        var cur = modes.indexOf(l.automation[0].interpMode);
                        l.automation[0].interpMode = modes[(cur + 1) % modes.length];
                        buildLayerRows();
                        if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                    });
                    autoRow.appendChild(interpBtn);

                    // Canvas for drawing automation
                    var canvas = document.createElement('canvas');
                    canvas.className = 'seq-auto-canvas';
                    autoRow.appendChild(canvas);

                    // Draw and interact with automation points
                    (function(canvas, layerIdx) {
                        var TARGETS = Synth.Layers.AUTOMATION_TARGETS;
                        var POINT_RADIUS = 4;
                        var dragging = -1;

                        function getAutoData() {
                            var l = Synth.Layers.get(layerIdx);
                            return l && l.automation.length > 0 ? l.automation[0] : null;
                        }

                        function getTarget() {
                            var ad = getAutoData();
                            return ad ? TARGETS[ad.target] : null;
                        }

                        // Map value to Y position (0 = top = max, h = bottom = min)
                        function valToY(val, h, tgt) {
                            if (tgt.log) {
                                var logMin = Math.log(tgt.min), logMax = Math.log(tgt.max);
                                return h - (Math.log(Math.max(val, tgt.min)) - logMin) / (logMax - logMin) * h;
                            }
                            return h - (val - tgt.min) / (tgt.max - tgt.min) * h;
                        }

                        function yToVal(y, h, tgt) {
                            var norm = 1 - y / h;
                            norm = Math.max(0, Math.min(1, norm));
                            if (tgt.log) {
                                var logMin = Math.log(tgt.min), logMax = Math.log(tgt.max);
                                return Math.exp(logMin + norm * (logMax - logMin));
                            }
                            return tgt.min + norm * (tgt.max - tgt.min);
                        }

                        function beatToX(beat, w) { return beat / 4 * w; }
                        function xToBeat(x, w) { return Math.max(0, Math.min(4, x / w * 4)); }

                        function draw() {
                            var w = canvas.clientWidth;
                            var h = canvas.clientHeight;
                            if (w <= 0 || h <= 0) return;
                            // Sync bitmap size to display size
                            if (canvas.width !== w) canvas.width = w;
                            if (canvas.height !== h) canvas.height = h;
                            var ctx = canvas.getContext('2d');
                            // Draw background (CSS bg is transparent for compositing)
                            ctx.fillStyle = '#12121a';
                            ctx.fillRect(0, 0, w, h);

                            var ad = getAutoData();
                            var tgt = getTarget();
                            if (!ad || !tgt) return;

                            var color = Synth.Layers.get(layerIdx).color || '#00e5ff';

                            // Draw grid lines at each step
                            ctx.strokeStyle = '#ffffff10';
                            ctx.lineWidth = 1;
                            for (var s = 0; s <= 16; s++) {
                                var gx = s / 16 * w;
                                ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, h); ctx.stroke();
                            }

                            if (ad.points.length === 0) return;

                            // Sort points for drawing
                            var pts = ad.points.slice().sort(function(a, b) { return a.beat - b.beat; });

                            // Draw interpolated curve
                            ctx.strokeStyle = color;
                            ctx.lineWidth = 1.5;
                            ctx.beginPath();
                            for (var px = 0; px < w; px++) {
                                var beat = xToBeat(px, w);
                                var val = evaluateAutomation(pts, beat, ad.interpMode, tgt);
                                var py = valToY(val, h, tgt);
                                if (px === 0) ctx.moveTo(px, py);
                                else ctx.lineTo(px, py);
                            }
                            ctx.stroke();

                            // Draw points
                            for (var i = 0; i < pts.length; i++) {
                                var bx = beatToX(pts[i].beat, w);
                                var by = valToY(pts[i].value, h, tgt);
                                ctx.beginPath();
                                ctx.arc(bx, by, POINT_RADIUS, 0, Math.PI * 2);
                                ctx.fillStyle = color;
                                ctx.fill();
                                ctx.strokeStyle = '#fff';
                                ctx.lineWidth = 1;
                                ctx.stroke();
                            }
                        }

                        // Simple JS-side evaluation for drawing curves
                        function evaluateAutomation(pts, beat, mode, tgt) {
                            if (pts.length === 0) return tgt.default;
                            if (pts.length === 1) return pts[0].value;
                            if (beat <= pts[0].beat) return pts[0].value;
                            if (beat >= pts[pts.length - 1].beat) return pts[pts.length - 1].value;

                            var p1, p2;
                            for (var i = 0; i < pts.length - 1; i++) {
                                if (beat >= pts[i].beat && beat < pts[i + 1].beat) {
                                    p1 = pts[i]; p2 = pts[i + 1]; break;
                                }
                            }
                            if (!p1) return pts[pts.length - 1].value;

                            if (mode === 'step') return p1.value;
                            var t = (beat - p1.beat) / (p2.beat - p1.beat);
                            if (mode === 'smooth') t = (1 - Math.cos(t * Math.PI)) * 0.5;
                            return p1.value + t * (p2.value - p1.value);
                        }

                        function canvasSize() {
                            return { w: canvas.clientWidth, h: canvas.clientHeight };
                        }

                        function findPoint(x, y, w, h, pts, tgt) {
                            for (var i = 0; i < pts.length; i++) {
                                var bx = beatToX(pts[i].beat, w);
                                var by = valToY(pts[i].value, h, tgt);
                                if (Math.abs(x - bx) < 8 && Math.abs(y - by) < 8) return i;
                            }
                            return -1;
                        }

                        canvas.addEventListener('mousedown', function(e) {
                            var ad = getAutoData();
                            var tgt = getTarget();
                            if (!ad || !tgt) return;
                            var rect = canvas.getBoundingClientRect();
                            var x = e.clientX - rect.left;
                            var y = e.clientY - rect.top;
                            var sz = canvasSize();

                            var idx = findPoint(x, y, sz.w, sz.h, ad.points, tgt);
                            if (e.button === 2 || (e.button === 0 && e.ctrlKey)) {
                                // Right-click or ctrl+click: delete point
                                if (idx >= 0) {
                                    ad.points.splice(idx, 1);
                                    draw();
                                    if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                                }
                                e.preventDefault();
                                return;
                            }
                            if (idx >= 0) {
                                dragging = idx;
                            } else {
                                // Add new point
                                var beat = xToBeat(x, sz.w);
                                var val = yToVal(y, sz.h, tgt);
                                ad.points.push({ beat: beat, value: val });
                                ad.points.sort(function(a, b) { return a.beat - b.beat; });
                                dragging = findPoint(x, y, sz.w, sz.h, ad.points, tgt);
                                draw();
                                if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                            }
                        });

                        canvas.addEventListener('mousemove', function(e) {
                            if (dragging < 0) return;
                            var ad = getAutoData();
                            var tgt = getTarget();
                            if (!ad || !tgt) return;
                            var rect = canvas.getBoundingClientRect();
                            var x = e.clientX - rect.left;
                            var y = e.clientY - rect.top;
                            var sz = canvasSize();

                            ad.points[dragging].beat = xToBeat(x, sz.w);
                            ad.points[dragging].value = yToVal(y, sz.h, tgt);
                            draw();
                        });

                        canvas.addEventListener('mouseup', function() {
                            if (dragging >= 0) {
                                dragging = -1;
                                var ad = getAutoData();
                                if (ad) ad.points.sort(function(a, b) { return a.beat - b.beat; });
                                if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                            }
                        });

                        canvas.addEventListener('mouseleave', function() {
                            if (dragging >= 0) {
                                dragging = -1;
                                var ad = getAutoData();
                                if (ad) ad.points.sort(function(a, b) { return a.beat - b.beat; });
                                if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
                            }
                        });

                        canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });

                        // Initial draw after layout
                        setTimeout(draw, 0);
                    })(canvas, layerIdx);
                }

                seqLayersEl.appendChild(autoRow);
            })(li);
        }

        // Rebuild visualizer rows to match current layers
        if (Synth.Visualizer && Synth.Visualizer.rebuild) {
            Synth.Visualizer.rebuild();
        }
    }

    // Step highlight callback
    Synth.Sequencer.onStep(function(step) {
        $$('.seq-step').forEach(function(el) {
            el.classList.toggle('playing', parseInt(el.getAttribute('data-step')) === step);
        });
    });

    // Add layer button
    document.getElementById('layer-add').addEventListener('click', function() {
        var newLayer = Synth.Layers.add();
        if (newLayer) {
            Synth.Layers.select(Synth.Layers.count() - 1);
            buildLayerRows();
        }
    });

    // Build initial layer rows
    buildLayerRows();

    // -----------------------------------------------------------------------
    // Sequencer transport & controls
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Save Loop — offline-render one full sequencer/arp loop to WAV
    // -----------------------------------------------------------------------
    document.getElementById('seq-save-loop').addEventListener('click', function() {
        var audioCtx = Synth.getAudioContext();
        if (!audioCtx) return;

        var Layers = Synth.Layers;
        var NUM_STEPS = Synth.Sequencer.NUM_STEPS;
        var bpm = Synth.Sequencer.getBPM();
        var SAMPLE_RATE = audioCtx.sampleRate || 44100;
        var stepDur = 60 / bpm / 4;
        var loopDur = stepDur * NUM_STEPS;
        var layerCount = Layers.count();

        // Check there are notes to play
        var hasNotes = false;
        for (var i = 0; i < layerCount; i++) {
            var layer = Layers.get(i);
            if (!layer || layer.muted) continue;
            for (var s = 0; s < NUM_STEPS; s++) {
                if (layer.steps[s] !== null) { hasNotes = true; break; }
            }
            if (hasNotes) break;
        }
        if (!hasNotes) {
            console.warn('Nothing to save — add notes to the sequencer');
            return;
        }

        var path = showSaveFileDialog('WAV Files|wav', 'loop.wav');
        if (!path) return;
        if (path.indexOf('.wav') < 0 && path.indexOf('.WAV') < 0) path += '.wav';

        var btn = document.getElementById('seq-save-loop');
        btn.textContent = 'Saving...';
        btn.classList.add('active');

        function collectUnique(layer) {
            var held = [];
            for (var i = 0; i < NUM_STEPS; i++) {
                if (layer.steps[i] !== null && held.indexOf(layer.steps[i]) < 0) held.push(layer.steps[i]);
            }
            held.sort(function(a, b) { return a - b; });
            return held;
        }

        function pickArp(arpIdx, held, pattern) {
            if (held.length === 0) return { note: -1, next: arpIdx };
            var idx, next;
            switch (pattern) {
                case 'down':
                    idx = held[held.length - 1 - (arpIdx % held.length)]; next = arpIdx + 1; break;
                case 'updown':
                    if (held.length === 1) { idx = held[0]; next = arpIdx; }
                    else { var c = held.length * 2 - 2; var p = arpIdx % c;
                           idx = p < held.length ? held[p] : held[c - p]; next = arpIdx + 1; }
                    break;
                case 'random':
                    idx = held[Math.floor(Math.random() * held.length)]; next = arpIdx; break;
                default: idx = held[arpIdx % held.length]; next = arpIdx + 1;
            }
            return { note: idx, next: next };
        }

        // Find max release for tail
        var maxRelease = 0;
        for (var li = 0; li < layerCount; li++) {
            var l = Layers.get(li);
            if (l && !l.muted) maxRelease = Math.max(maxRelease, l.adsr.release);
        }

        var totalSamples = Math.ceil((loopDur + maxRelease) * SAMPLE_RATE);
        var mixedOutput = new Float32Array(totalSamples);

        // Render each layer: oscillator+ADSR in JS, then effects offline in C++
        for (var li = 0; li < layerCount; li++) {
            var layer = Layers.get(li);
            if (!layer || layer.muted) continue;

            var layerBuf = new Float32Array(totalSamples);
            var arpIdx = 0;

            for (var step = 0; step < NUM_STEPS; step++) {
                var noteIdx = -1;
                if (layer.mode === 'arpeggiator') {
                    if (layer.steps[step] !== null) {
                        var pick = pickArp(arpIdx, collectUnique(layer), layer.arpPattern);
                        noteIdx = pick.note; arpIdx = pick.next;
                    }
                } else {
                    var sn = layer.steps[step];
                    if (sn !== null && sn !== undefined && sn >= 0) noteIdx = sn;
                }
                if (noteIdx < 0) continue;
                var note = Synth.notes[noteIdx];
                if (!note) continue;

                var startSmp = Math.floor(step * stepDur * SAMPLE_RATE);
                var onSmp = Math.floor(stepDur * SAMPLE_RATE);
                var relSmp = Math.ceil(layer.adsr.release * SAMPLE_RATE);
                var noteSmp = onSmp + relSmp;

                for (var s = 0; s < noteSmp; s++) {
                    var outIdx = startSmp + s;
                    if (outIdx >= totalSamples) break;
                    var t = s / SAMPLE_RATE;
                    var phase = t * note.freq;
                    var val;
                    switch (layer.waveform) {
                        case 'square':     val = (phase % 1) < 0.5 ? 1 : -1; break;
                        case 'sawtooth':   val = 2 * (phase % 1) - 1; break;
                        case 'triangle':   var p = phase % 1; val = p < 0.5 ? 4 * p - 1 : 3 - 4 * p; break;
                        case 'whitenoise': val = Math.random() * 2 - 1; break;
                        case 'pinknoise':  val = (Math.random() + Math.random() + Math.random()) / 1.5 - 1; break;
                        default:           val = Math.sin(2 * Math.PI * phase);
                    }
                    var env;
                    if (t < layer.adsr.attack) {
                        env = layer.adsr.attack > 0 ? t / layer.adsr.attack : 1;
                    } else if (t < layer.adsr.attack + layer.adsr.decay) {
                        var dp = (t - layer.adsr.attack) / (layer.adsr.decay || 0.001);
                        env = 1.0 - dp * (1.0 - layer.adsr.sustain);
                    } else if (t < stepDur) {
                        env = layer.adsr.sustain;
                    } else {
                        var rt = t - stepDur;
                        env = rt < layer.adsr.release ? layer.adsr.sustain * (1 - rt / layer.adsr.release) : 0;
                    }
                    layerBuf[outIdx] += val * env * 0.3;
                }
            }

            // Process through bus effect chain (offline, non-realtime)
            var processed = audioCtx.processEffectsOffline(layer.busId, layerBuf);
            var src = processed || layerBuf;
            var len = Math.min(src.length, mixedOutput.length);
            for (var i = 0; i < len; i++) mixedOutput[i] += src[i];
            // If processed is longer (effect tail), extend output
            if (processed && processed.length > mixedOutput.length) {
                var extended = new Float32Array(processed.length);
                extended.set(mixedOutput);
                for (var i = mixedOutput.length; i < processed.length; i++) extended[i] = processed[i];
                mixedOutput = extended;
            }
        }

        // Trim trailing silence
        var end = mixedOutput.length - 1;
        while (end > 0 && Math.abs(mixedOutput[end]) < 0.0001) end--;
        var minSmp = Math.ceil(loopDur * SAMPLE_RATE);
        end = Math.max(end, minSmp - 1);
        var finalOutput = mixedOutput.subarray(0, end + 1);

        // Clamp
        for (var c = 0; c < finalOutput.length; c++) {
            if (finalOutput[c] > 1) finalOutput[c] = 1;
            else if (finalOutput[c] < -1) finalOutput[c] = -1;
        }

        try {
            audioCtx.saveWav(path, finalOutput, 1, SAMPLE_RATE);
            console.log('Loop saved:', path, '(' + finalOutput.length + ' samples)');
        } catch (e) {
            console.error('Save failed:', e.message);
        }

        btn.textContent = 'Save Loop';
        btn.classList.remove('active');
    });

    // -----------------------------------------------------------------------
    // Freeform Record — record live playing to WAV
    // -----------------------------------------------------------------------
    var seqRecording = false;

    document.getElementById('seq-record').addEventListener('click', function() {
        var btn = this;
        var audioCtx = Synth.getAudioContext();
        if (!audioCtx) return;

        if (!seqRecording) {
            // Start recording
            audioCtx.startRecording();
            seqRecording = true;
            btn.classList.add('recording');
            btn.textContent = 'Stop Rec';
        } else {
            // Stop recording and save
            var samples = audioCtx.stopRecording();
            seqRecording = false;
            btn.classList.remove('recording');
            btn.textContent = 'Record';

            if (!samples || samples.length === 0) {
                console.warn('No audio recorded');
                return;
            }

            var path = showSaveFileDialog('WAV Files|wav', 'recording.wav');
            if (path) {
                if (path.indexOf('.wav') < 0 && path.indexOf('.WAV') < 0) path += '.wav';
                try {
                    audioCtx.saveWav(path, samples, 1, audioCtx.sampleRate);
                    console.log('Recording saved:', path);
                } catch (e) {
                    console.error('Save failed:', e.message);
                }
            }
        }
    });

    $$('#seq-mode-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#seq-mode-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var layer = Synth.Layers.getActive();
            if (layer) {
                layer.mode = btn.getAttribute('data-mode');
                if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
            }
            buildLayerRows();
        });
    });

    $$('#arp-pattern-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#arp-pattern-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var layer = Synth.Layers.getActive();
            if (layer) {
                layer.arpPattern = btn.getAttribute('data-pattern');
                layer.arpIndex = 0;
                if (Synth.Sequencer.isPlaying()) Synth.Sequencer.rebuild();
            }
        });
    });

    // -----------------------------------------------------------------------
    // Sync initial UI
    // -----------------------------------------------------------------------
    syncUIToSignal();

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
                Synth.ClipEditor.clear();
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
        var files = showOpenFileDialog('Audio Files|wav;flac;mp3;ogg;opus');
        if (files && files.length > 0) {
            try { Synth.ClipEditor.loadFromFile(files[0]); }
            catch (e) { console.error('Load failed:', e.message); }
        }
    });

    document.getElementById('ed-save').addEventListener('click', function() {
        var path = showSaveFileDialog('WAV Files|wav', 'clip.wav');
        if (path) {
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
        if (pct !== 100) Synth.ClipEditor.timeStretch(100 / pct);
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
