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
    // Init layers
    // -----------------------------------------------------------------------
    Synth.Presets.load('Init');
    Synth.Layers.init();

    // -----------------------------------------------------------------------
    // Fancy Sliders — sidebar synth params
    // -----------------------------------------------------------------------
    var sliders = {};

    function layerParam(path, value) {
        // Set a nested property on the active layer and apply to engine
        var layer = Synth.Layers.getActive();
        if (!layer) return;
        var parts = path.split('.');
        var obj = layer;
        for (var i = 0; i < parts.length - 1; i++) obj = obj[parts[i]];
        obj[parts[parts.length - 1]] = value;
    }

    sliders.adsrA = Synth.Slider(document.getElementById('adsr-a-slider'), {
        min: 1, max: 2000, value: 10, step: 1, defaultValue: 10,
        format: formatMs,
        onChange: function(ms) {
            layerParam('adsr.attack', ms / 1000);
            Synth.setADSR(ms / 1000, sliders.adsrD.getValue() / 1000,
                          sliders.adsrS.getValue() / 100, sliders.adsrR.getValue() / 1000);
        }
    });

    sliders.adsrD = Synth.Slider(document.getElementById('adsr-d-slider'), {
        min: 1, max: 2000, value: 100, step: 1, defaultValue: 100,
        format: formatMs,
        onChange: function(ms) {
            layerParam('adsr.decay', ms / 1000);
            Synth.setADSR(sliders.adsrA.getValue() / 1000, ms / 1000,
                          sliders.adsrS.getValue() / 100, sliders.adsrR.getValue() / 1000);
        }
    });

    sliders.adsrS = Synth.Slider(document.getElementById('adsr-s-slider'), {
        min: 0, max: 100, value: 100, step: 1, defaultValue: 100,
        format: function(v) { return v + '%'; },
        onChange: function(pct) {
            layerParam('adsr.sustain', pct / 100);
            Synth.setADSR(sliders.adsrA.getValue() / 1000, sliders.adsrD.getValue() / 1000,
                          pct / 100, sliders.adsrR.getValue() / 1000);
        }
    });

    sliders.adsrR = Synth.Slider(document.getElementById('adsr-r-slider'), {
        min: 1, max: 3000, value: 80, step: 1, defaultValue: 80,
        format: formatMs,
        onChange: function(ms) {
            layerParam('adsr.release', ms / 1000);
            Synth.setADSR(sliders.adsrA.getValue() / 1000, sliders.adsrD.getValue() / 1000,
                          sliders.adsrS.getValue() / 100, ms / 1000);
        }
    });

    sliders.filterCutoff = Synth.Slider(document.getElementById('filter-cutoff-slider'), {
        min: 0, max: 100, value: 50, step: 0.5, defaultValue: 50,
        format: function(v) { return formatFreq(cutoffSliderToFreq(v)); },
        onChange: function(v) {
            var freq = cutoffSliderToFreq(v);
            layerParam('filter.frequency', freq);
            Synth.Filter.setCutoff(freq);
        }
    });

    sliders.filterQ = Synth.Slider(document.getElementById('filter-q-slider'), {
        min: 1, max: 200, value: 10, step: 1, defaultValue: 10,
        format: function(v) { return (v / 10).toFixed(1); },
        onChange: function(v) {
            layerParam('filter.Q', v / 10);
            Synth.Filter.setQ(v / 10);
        }
    });

    sliders.delayTime = Synth.Slider(document.getElementById('delay-time-slider'), {
        min: 10, max: 1500, value: 300, step: 1, defaultValue: 300,
        format: formatMs,
        onChange: function(ms) {
            layerParam('delay.time', ms / 1000);
            Synth.Effects.setDelayTime(ms / 1000);
        }
    });

    sliders.delayFb = Synth.Slider(document.getElementById('delay-fb-slider'), {
        min: 0, max: 90, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            layerParam('delay.feedback', v / 100);
            Synth.Effects.setDelayFeedback(v / 100);
        }
    });

    sliders.delayMix = Synth.Slider(document.getElementById('delay-mix-slider'), {
        min: 0, max: 100, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            layerParam('delay.mix', v / 100);
            Synth.Effects.setDelayMix(v / 100);
        }
    });

    sliders.lfoRate = Synth.Slider(document.getElementById('lfo-rate-slider'), {
        min: 0, max: 100, value: 30, step: 0.5, defaultValue: 30,
        format: function(v) { return formatLfoRate(lfoSliderToHz(v)); },
        onChange: function(v) {
            var hz = lfoSliderToHz(v);
            layerParam('lfo.rate', hz);
            Synth.LFO.setRate(hz);
        }
    });

    sliders.lfoDepth = Synth.Slider(document.getElementById('lfo-depth-slider'), {
        min: 0, max: 100, value: 30, step: 1, defaultValue: 30,
        format: function(v) { return v + '%'; },
        onChange: function(v) {
            layerParam('lfo.depth', v / 100);
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
            Synth.setWaveform(wf);
            layerParam('waveform', wf);
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
            layerParam('pan', pan);
            Synth.setPan(pan);
        }
    });

    // -----------------------------------------------------------------------
    // Volume (global, not per-layer)
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
    // Filter toggle & type (per-layer)
    // -----------------------------------------------------------------------
    var filterToggle = document.getElementById('filter-toggle');
    filterToggle.addEventListener('click', function() {
        var enabled = !Synth.Filter.isEnabled();
        Synth.Filter.setEnabled(enabled);
        layerParam('filter.enabled', enabled);
        updateToggle(filterToggle, enabled);
    });

    $$('#filter-type-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#filter-type-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var type = btn.getAttribute('data-type');
            Synth.Filter.setType(type);
            layerParam('filter.type', type);
        });
    });

    // -----------------------------------------------------------------------
    // Delay toggle (per-layer)
    // -----------------------------------------------------------------------
    var delayToggle = document.getElementById('delay-toggle');
    delayToggle.addEventListener('click', function() {
        var enabled = !Synth.Effects.isDelayEnabled();
        Synth.Effects.setDelayEnabled(enabled);
        layerParam('delay.enabled', enabled);
        updateToggle(delayToggle, enabled);
    });

    // -----------------------------------------------------------------------
    // LFO toggle & target (per-layer)
    // -----------------------------------------------------------------------
    var lfoToggle = document.getElementById('lfo-toggle');
    lfoToggle.addEventListener('click', function() {
        var enabled = !Synth.LFO.isEnabled();
        Synth.LFO.setEnabled(enabled);
        layerParam('lfo.enabled', enabled);
        updateToggle(lfoToggle, enabled);
    });

    $$('#lfo-target-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#lfo-target-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var t = btn.getAttribute('data-target');
            Synth.LFO.setTarget(t);
            layerParam('lfo.target', t);
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
        // Re-capture into layer 1 after preset load
        Synth.Layers.init();
        syncUIToLayer();
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
    // Sync UI to active layer
    // -----------------------------------------------------------------------
    function syncUIToLayer() {
        var layer = Synth.Layers.getActive();
        if (!layer) return;

        // Layer indicator
        document.getElementById('layer-indicator-color').style.background = layer.color;
        document.getElementById('layer-indicator-name').textContent = layer.name;

        // Waveform
        $$('#wave-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-wave') === layer.waveform);
        });

        // Volume (global)
        document.getElementById('volume').value = Math.round(Synth.getVolume() * 100);

        // Pan
        sliders.pan.setValue(Math.round(layer.pan * 100), true);

        // ADSR
        sliders.adsrA.setValue(Math.round(layer.adsr.attack * 1000), true);
        sliders.adsrD.setValue(Math.round(layer.adsr.decay * 1000), true);
        sliders.adsrS.setValue(Math.round(layer.adsr.sustain * 100), true);
        sliders.adsrR.setValue(Math.round(layer.adsr.release * 1000), true);

        // Filter
        updateToggle(filterToggle, layer.filter.enabled);
        $$('#filter-type-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-type') === layer.filter.type);
        });
        sliders.filterCutoff.setValue(Math.round(freqToCutoffSlider(layer.filter.frequency)), true);
        sliders.filterQ.setValue(Math.round(layer.filter.Q * 10), true);

        // Delay
        updateToggle(delayToggle, layer.delay.enabled);
        sliders.delayTime.setValue(Math.round(layer.delay.time * 1000), true);
        sliders.delayFb.setValue(Math.round(layer.delay.feedback * 100), true);
        sliders.delayMix.setValue(Math.round(layer.delay.mix * 100), true);

        // LFO
        updateToggle(lfoToggle, layer.lfo.enabled);
        sliders.lfoRate.setValue(Math.round(hzToLfoSlider(layer.lfo.rate)), true);
        sliders.lfoDepth.setValue(Math.round(layer.lfo.depth * 100), true);
        $$('#lfo-target-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-target') === layer.lfo.target);
        });

        // Seq/Arp mode (per-layer)
        $$('#seq-mode-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-mode') === layer.mode);
        });

        // Arp pattern (per-layer)
        $$('#arp-pattern-btns .btn').forEach(function(b) {
            b.classList.toggle('active', b.getAttribute('data-pattern') === layer.arpPattern);
        });

        // Highlight selected layer row
        $$('.seq-layer-row').forEach(function(row, i) {
            row.classList.toggle('selected', i === Synth.Layers.getActiveIndex());
        });
    }

    // Respond to layer selection changes
    Synth.Layers.onSelect(function() { syncUIToLayer(); });

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
                if (layerIdx === Synth.Layers.getActiveIndex()) row.classList.add('selected');

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
                        });

                        grid.appendChild(stepEl);
                    })(si);
                }

                row.appendChild(grid);
                seqLayersEl.appendChild(row);
            })(li);
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
        var result = Synth.Sequencer.renderOffline();
        if (!result || result.samples.length === 0) {
            console.warn('Nothing to save — add notes to the sequencer');
            return;
        }

        var path = showSaveFileDialog('WAV Files|wav', 'loop.wav');
        if (path) {
            if (path.indexOf('.wav') < 0 && path.indexOf('.WAV') < 0) path += '.wav';
            try {
                var wav = Synth.WAV.encode(result.samples, result.sampleRate);
                require('fs').writeFileSync(path, new Uint8Array(wav));
                console.log('Loop saved:', path);
            } catch (e) {
                console.error('Save failed:', e.message);
            }
        }
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
                    var wav = Synth.WAV.encode(samples, audioCtx.sampleRate);
                    require('fs').writeFileSync(path, new Uint8Array(wav));
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
            if (layer) layer.mode = btn.getAttribute('data-mode');
        });
    });

    $$('#arp-pattern-btns .btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            $$('#arp-pattern-btns .btn').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            var layer = Synth.Layers.getActive();
            if (layer) { layer.arpPattern = btn.getAttribute('data-pattern'); layer.arpIndex = 0; }
        });
    });

    // -----------------------------------------------------------------------
    // Sync initial UI
    // -----------------------------------------------------------------------
    syncUIToLayer();

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
        var files = showOpenFileDialog('Audio Files|wav');
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
