(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var stackEl = null;
    var rows = [];         // { type, layerIndex, busId, color, name, el, canvas, ctx, collapsed, peakBuf, peakIdx }
    var combinedRow = null;
    var waveData = null;
    var micWaveData = null;
    var frameCount = 0;
    var lastFpsTime = performance.now();
    var fps = 0;
    var micPitchCounter = 0;
    var paused = false;

    var PEAK_BUF_LEN = 200;

    // Cached DOM element references — avoid querySelectorAll / getElementById per frame
    var cachedMeters = null;
    var cachedMicLevelBar = null;
    var cachedMicNoteEl = null;
    var cachedMicFreqEl = null;

    // Reusable buffers for polyline batching — avoids per-frame allocations
    var polyBuf = null;
    var envUpperPts = new Float32Array(PEAK_BUF_LEN * 2);
    var envLowerPts = new Float32Array(PEAK_BUF_LEN * 2);
    var envFillPts = new Float32Array((1 + PEAK_BUF_LEN * 2) * 2);

    Synth.Visualizer = {
        init: function(container) {
            stackEl = container;
        },

        rebuild: function() {
            if (!stackEl) return;

            // Determine desired rows: one per layer + mic (if enabled) + combined
            var desired = [];
            var count = Synth.Layers.count();
            for (var i = 0; i < count; i++) {
                var layer = Synth.Layers.get(i);
                desired.push({ type: 'layer', layerIndex: i, busId: layer.busId,
                               color: layer.color, name: layer.name });
            }

            var mic = Synth.Layers.getMicSignal ? Synth.Layers.getMicSignal() : null;
            if (mic && Synth.isMicEnabled && Synth.isMicEnabled()) {
                desired.push({ type: 'mic', layerIndex: -1, busId: mic.busId,
                               color: '#ff4444', name: 'Mic' });
            }

            desired.push({ type: 'combined', layerIndex: -1, busId: -1,
                           color: '#00e5ff', name: 'Combined' });

            // Rebuild DOM — keep it simple: clear and recreate
            // Preserve collapsed state by type+layerIndex key
            var collapsedState = {};
            for (var i = 0; i < rows.length; i++) {
                var key = rows[i].type + ':' + rows[i].layerIndex;
                collapsedState[key] = rows[i].collapsed;
            }

            stackEl.innerHTML = '';
            rows = [];
            combinedRow = null;
            cachedMeters = null;
            cachedMicLevelBar = null;
            cachedMicNoteEl = null;
            cachedMicFreqEl = null;

            for (var i = 0; i < desired.length; i++) {
                var d = desired[i];
                var row = createRow(d);
                var key = d.type + ':' + d.layerIndex;
                if (collapsedState[key]) {
                    row.collapsed = true;
                    row.el.classList.add('collapsed');
                }
                stackEl.appendChild(row.el);
                rows.push(row);
                if (d.type === 'combined') combinedRow = row;
            }
        },

        draw: function() {
            if (paused) return;
            requestAnimationFrame(Synth.Visualizer.draw);

            frameCount++;
            var now = performance.now();
            if (now - lastFpsTime >= 1000) {
                fps = frameCount;
                frameCount = 0;
                lastFpsTime = now;
                var fpsEl = document.getElementById('fps-display');
                if (fpsEl) fpsEl.textContent = fps + ' fps';
            }

            for (var i = 0; i < rows.length; i++) {
                var row = rows[i];
                if (row.collapsed) continue;

                var canvas = row.canvas;
                var ctx = row.ctx;
                if (!ctx) continue;

                // Sync bitmap size to display size
                var cw = canvas.clientWidth;
                var ch = canvas.clientHeight;
                if (cw <= 0 || ch <= 0) continue;
                if (canvas.width !== cw) canvas.width = cw;
                if (canvas.height !== ch) canvas.height = ch;

                var W = cw;
                var H = ch;

                ctx.clearRect(0, 0, W, H);
                ctx.fillStyle = '#0a0a0f';
                ctx.fillRect(0, 0, W, H);

                if (row.type === 'layer') {
                    drawLayerEnvelope(ctx, W, H, row);
                } else if (row.type === 'mic') {
                    drawMicWaveform(ctx, W, H, row);
                } else if (row.type === 'combined') {
                    drawCombinedWaveform(ctx, W, H);
                }
            }

            updateMicInfo();
            if (frameCount % 10 === 0) updateBusMeters();
        },

        pause: function() {
            paused = true;
            for (var i = 0; i < rows.length; i++) {
                if (rows[i].ctx) {
                    var W = rows[i].ctx.canvasWidth;
                    var H = rows[i].ctx.canvasHeight;
                    if (W > 0 && H > 0) rows[i].ctx.clearRect(0, 0, W, H);
                }
            }
        },

        resume: function() {
            if (paused) { paused = false; Synth.Visualizer.draw(); }
        }
    };

    function createRow(d) {
        var el = document.createElement('div');
        el.className = 'viz-row' + (d.type === 'combined' ? ' combined' : '');

        var label = document.createElement('div');
        label.className = 'viz-row-label';

        var colorDot = document.createElement('span');
        colorDot.className = 'viz-row-label-color';
        colorDot.style.background = d.color;
        label.appendChild(colorDot);

        var nameSpan = document.createElement('span');
        nameSpan.textContent = d.name;
        label.appendChild(nameSpan);

        el.appendChild(label);

        var canvas = document.createElement('canvas');
        canvas.style.background = 'transparent';
        el.appendChild(canvas);

        var ctx = canvas.getContext('2d');

        var row = {
            type: d.type,
            layerIndex: d.layerIndex,
            busId: d.busId,
            color: d.color,
            name: d.name,
            el: el,
            canvas: canvas,
            ctx: ctx,
            collapsed: false,
            peakBuf: new Float32Array(PEAK_BUF_LEN),
            peakIdx: 0
        };

        // Collapsible click (not combined)
        if (d.type !== 'combined') {
            label.addEventListener('click', function() {
                row.collapsed = !row.collapsed;
                el.classList.toggle('collapsed', row.collapsed);
            });
        }

        return row;
    }

    function drawLayerEnvelope(ctx, W, H, row) {
        var SC = Synth.SignalChain;
        if (!SC) return;

        // Sample current peak with slow decay for visual smoothness
        var peakL = SC.getBusPeakL(row.busId);
        var peakR = SC.getBusPeakR(row.busId);
        var peak = Math.max(peakL, peakR);

        // Apply gain and clamp — linear peaks are often very small (0.01-0.1 range)
        peak = Math.min(1.0, peak * 12.0);

        // Smooth decay: hold previous value if current is lower
        var prev = row.peakBuf[(row.peakIdx + PEAK_BUF_LEN - 1) % PEAK_BUF_LEN];
        if (peak < prev) peak = prev * 0.92;  // slow decay

        row.peakBuf[row.peakIdx] = peak;
        row.peakIdx = (row.peakIdx + 1) % PEAK_BUF_LEN;

        var midY = H / 2;
        var color = row.color || '#00e5ff';

        // Center line
        ctx.strokeStyle = '#141420';
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(0, midY); ctx.lineTo(W, midY); ctx.stroke();

        // Build point arrays for upper and lower edges (reuse cached buffers)
        var scale95 = midY * 0.95;
        for (var i = 0; i < PEAK_BUF_LEN; i++) {
            var idx = (row.peakIdx + i) % PEAK_BUF_LEN;
            var x = (i / PEAK_BUF_LEN) * W;
            var amp = row.peakBuf[idx] * scale95;
            envUpperPts[i * 2]     = x;
            envUpperPts[i * 2 + 1] = midY - amp;
            envLowerPts[i * 2]     = x;
            envLowerPts[i * 2 + 1] = midY + amp;
        }

        // Fill envelope: [startPt, upper..., lowerReversed...]
        envFillPts[0] = 0; envFillPts[1] = midY;
        envFillPts.set(envUpperPts, 2);
        var off = (1 + PEAK_BUF_LEN) * 2;
        for (var i = 0; i < PEAK_BUF_LEN; i++) {
            var src = PEAK_BUF_LEN - 1 - i;
            envFillPts[off + i * 2]     = envLowerPts[src * 2];
            envFillPts[off + i * 2 + 1] = envLowerPts[src * 2 + 1];
        }
        ctx.fillStyle = color.slice(0, 7) + '20';
        ctx.beginPath();
        ctx.polyline(envFillPts);
        ctx.closePath();
        ctx.fill();

        // Stroke upper edge
        ctx.strokeStyle = color;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.polyline(envUpperPts);
        ctx.stroke();

        // Stroke lower edge
        ctx.beginPath();
        ctx.polyline(envLowerPts);
        ctx.stroke();
    }

    function drawMicWaveform(ctx, W, H, row) {
        var micAnalyser = Synth.getMicAnalyser();
        if (!micAnalyser) return;

        if (!micWaveData || micWaveData.length !== micAnalyser.fftSize) {
            micWaveData = new Float32Array(micAnalyser.fftSize);
        }
        micAnalyser.getFloatTimeDomainData(micWaveData);
        drawWaveform(ctx, W, H, micWaveData, row.color);
    }

    function drawCombinedWaveform(ctx, W, H) {
        var analyser = Synth.getAnalyser();
        if (!analyser) return;

        if (!waveData || waveData.length !== analyser.fftSize) {
            waveData = new Float32Array(analyser.fftSize);
        }
        analyser.getFloatTimeDomainData(waveData);
        drawWaveform(ctx, W, H, waveData, '#00e5ff');
    }

    function buildWavePoints(buf, drawLen, W, midY, data, triggerOffset, scale) {
        for (var i = 0; i < drawLen; i++) {
            buf[i * 2]     = (i / drawLen) * W;
            buf[i * 2 + 1] = midY + data[triggerOffset + i] * scale;
        }
    }

    function drawWaveform(ctx, W, H, data, color) {
        var bufLen = data.length;
        var midY = H / 2;

        // Center line + quarter lines
        ctx.strokeStyle = '#141420';
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(0, midY); ctx.lineTo(W, midY); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(0, midY * 0.5); ctx.lineTo(W, midY * 0.5); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(0, midY * 1.5); ctx.lineTo(W, midY * 1.5); ctx.stroke();

        // Find peak amplitude and RMS
        var peak = 0, rms = 0;
        for (var i = 0; i < bufLen; i++) {
            var abs = Math.abs(data[i]);
            if (abs > peak) peak = abs;
            rms += data[i] * data[i];
        }
        rms = Math.sqrt(rms / bufLen);
        var intensity = Math.min(1.0, rms * 4);

        // Auto-scale: normalize to fill ~80% of canvas height
        // Use a minimum floor so silent signals don't get amplified to noise
        var normPeak = Math.max(peak, 0.01);
        var scale = (midY * 0.8) / normPeak;

        // Zero-crossing trigger for stable display
        var triggerOffset = 0;
        var searchEnd = Math.floor(bufLen / 4);
        for (var i = 1; i < searchEnd; i++) {
            if (data[i - 1] <= 0 && data[i] > 0) { triggerOffset = i; break; }
        }
        // Cap to canvas pixel width — more points than pixels is wasted rendering
        var drawLen = Math.min(bufLen - triggerOffset, Math.floor(bufLen * 0.5), Math.ceil(W));

        // Reuse a single Float32Array for polyline batching
        var need = drawLen * 2;
        if (!polyBuf || polyBuf.length < need) polyBuf = new Float32Array(need);
        var pts = polyBuf;
        buildWavePoints(pts, drawLen, W, midY, data, triggerOffset, scale);
        var view = drawLen * 2 < pts.length ? pts.subarray(0, drawLen * 2) : pts;

        // Glow
        if (intensity > 0.01) {
            ctx.strokeStyle = color.slice(0, 7) + '26';
            ctx.lineWidth = 6;
            ctx.beginPath();
            ctx.polyline(view);
            ctx.stroke();
        }

        // Main stroke
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.polyline(view);
        ctx.stroke();
    }

    function updateBusMeters() {
        var SC = Synth.SignalChain;
        if (!SC) return;
        if (!cachedMeters) cachedMeters = document.querySelectorAll('.seq-layer-meter');
        var meters = cachedMeters;
        for (var i = 0; i < meters.length; i++) {
            var busId = parseInt(meters[i].getAttribute('data-bus'), 10);
            if (isNaN(busId) || busId < 0) continue;
            var peakL = SC.getBusPeakL(busId);
            var peakR = SC.getBusPeakR(busId);
            // Round to integers and skip DOM mutation if unchanged —
            // each style.width change triggers full document relayout (~3ms)
            var pctL = Math.min(100, Math.round(peakL * 100));
            var pctR = Math.min(100, Math.round(peakR * 100));
            var bars = meters[i].children;
            if (bars[0] && bars[0]._lastPct !== pctL) {
                bars[0]._lastPct = pctL;
                bars[0].style.width = pctL + '%';
            }
            if (bars[1] && bars[1]._lastPct !== pctR) {
                bars[1]._lastPct = pctR;
                bars[1].style.width = pctR + '%';
            }
        }
    }

    function updateMicInfo() {
        micPitchCounter++;
        if (micPitchCounter % 6 !== 0) return;

        // Cache element lookups
        if (!cachedMicNoteEl) cachedMicNoteEl = document.getElementById('mic-note');
        if (!cachedMicFreqEl) cachedMicFreqEl = document.getElementById('mic-freq');

        // Skip all analysis when mic is disabled — avoids FFT computation
        if (!Synth.isMicEnabled || !Synth.isMicEnabled()) {
            if (cachedMicNoteEl) cachedMicNoteEl.textContent = 'Mic: --';
            if (cachedMicFreqEl) cachedMicFreqEl.textContent = '-- Hz';
            return;
        }

        var micAnalyser = Synth.getMicAnalyser();
        var micLevelBuf = Synth.getMicLevelBuf();
        if (!micAnalyser || !micLevelBuf) return;

        micAnalyser.getByteFrequencyData(micLevelBuf);
        var sum = 0, len = micLevelBuf.length;
        for (var i = 0; i < len; i += 4) sum += micLevelBuf[i];
        var avgLevel = sum / (len / 4) / 255;

        if (!cachedMicLevelBar) cachedMicLevelBar = document.getElementById('mic-level-bar');
        if (cachedMicLevelBar) {
            cachedMicLevelBar.style.height = Math.min(100, avgLevel * 300) + '%';
            if (avgLevel > 0.6) cachedMicLevelBar.style.background = '#cc3333';
            else if (avgLevel > 0.3) cachedMicLevelBar.style.background = '#cccc33';
            else cachedMicLevelBar.style.background = '#33cc33';
        }

        if (!cachedMicNoteEl || !cachedMicFreqEl) return;

        var freq = Synth.detectPitch();
        if (freq && freq > 50 && freq < 2000) {
            var info = Synth.freqToNoteName(freq);
            var c = info.cents >= 0 ? '+' + info.cents : '' + info.cents;
            cachedMicNoteEl.textContent = 'Mic: ' + info.name + ' (' + c + 'c)';
            cachedMicFreqEl.textContent = freq.toFixed(1) + ' Hz';
        } else {
            cachedMicNoteEl.textContent = 'Mic: --';
            cachedMicFreqEl.textContent = '-- Hz';
        }
    }
})();
