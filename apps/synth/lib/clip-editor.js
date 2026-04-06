// ---------------------------------------------------------------------------
// Clip Editor — comprehensive audio clip editing
// ---------------------------------------------------------------------------
(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var Editor = Synth.ClipEditor = {};

    // State
    var audioCtx = null;
    var canvas = null;
    var samples = null;          // Float32Array — the working buffer
    var clipId = -1;             // engine clip ID (for playback)
    var playbackId = -1;         // active playback instance
    var sampleRate = 44100;

    // Selection (in samples)
    var selStart = 0;
    var selEnd = 0;
    var hasSelection = false;

    // View (in samples)
    var viewStart = 0;
    var viewEnd = 0;

    // Clipboard
    var clipboard = null;

    // Playback
    var isPlaying = false;
    var playFromSample = 0;
    var looping = false;
    var animFrame = 0;

    // Cursor / playhead (in samples) — always visible, independent of selection
    var cursorPos = 0;

    // Interaction state
    var dragging = false;
    var dragStartX = 0;
    var dragStartSample = 0;
    var hasDragged = false;
    var cachedW = 0, cachedH = 0; // updated each draw

    // Shared constant
    var RULER_H = 18;

    // Undo
    var undoStack = [];
    var redoStack = [];
    var MAX_UNDO = 30;

    // UI references
    var statusEl = null;
    var timeDisplay = null;
    var zoomDisplay = null;

    Editor.init = function(canvasEl) {
        canvas = canvasEl;
        audioCtx = Synth.getAudioContext();
        sampleRate = audioCtx ? audioCtx.sampleRate : 44100;

        canvas.addEventListener('mousedown', onMouseDown);
        canvas.addEventListener('mousemove', onMouseMove);
        canvas.addEventListener('mouseup', onMouseUp);
        canvas.addEventListener('dblclick', onDblClick);
        canvas.addEventListener('wheel', onWheel);

        statusEl = document.getElementById('clip-status');
        timeDisplay = document.getElementById('clip-time');
        zoomDisplay = document.getElementById('clip-zoom');
    };

    // -----------------------------------------------------------------------
    // Load / Create clip data
    // -----------------------------------------------------------------------

    Editor.loadSamples = function(float32Array) {
        Editor.stop();
        destroyClip();
        samples = new Float32Array(float32Array);
        viewStart = 0;
        viewEnd = samples.length;
        selStart = 0;
        selEnd = 0;
        hasSelection = false;
        cursorPos = 0;
        undoStack = [];
        redoStack = [];
        createClip();
        Editor.draw();
        updateStatus();
    };

    Editor.hasSamples = function() { return samples !== null && samples.length > 0; };
    Editor.getSamples = function() { return samples; };
    Editor.getSampleRate = function() { return sampleRate; };

    Editor.loadFromFile = function(path) {
        if (!audioCtx) return;
        var result = audioCtx.decodeAudioFile(path);
        if (!result) {
            setStatus('Failed to load: ' + path.split('/').pop().split('\\').pop());
            return;
        }
        Editor.loadSamples(result.samples);
        setStatus('Loaded: ' + path.split('/').pop().split('\\').pop());
    };

    Editor.saveToFile = function(path) {
        if (!samples || !audioCtx) return;
        var src = hasSelection ? samples.slice(selStart, selEnd) : samples;
        audioCtx.saveWav(path, src, 1, sampleRate);
        setStatus('Saved: ' + path.split('/').pop().split('\\').pop());
    };

    Editor.record = function() {
        if (!audioCtx) return;
        audioCtx.startRecording();
        setStatus('Recording...');
    };

    Editor.stopRecording = function() {
        if (!audioCtx) return;
        var recorded = audioCtx.stopRecording();
        if (recorded && recorded.length > 0) {
            Editor.loadSamples(recorded);
            setStatus('Recorded ' + formatTime(recorded.length / sampleRate));
        }
    };

    // -----------------------------------------------------------------------
    // Engine clip management
    // -----------------------------------------------------------------------

    function createClip() {
        destroyClip();
        if (audioCtx && samples && samples.length > 0) {
            clipId = audioCtx.createClip(samples);
        }
    }

    function destroyClip() {
        if (clipId >= 0 && audioCtx) {
            Editor.stop();
            audioCtx.deleteClip(clipId);
            clipId = -1;
        }
    }

    function recreateClip() {
        var wasPlaying = isPlaying;
        Editor.stop();
        if (clipId >= 0 && audioCtx) audioCtx.deleteClip(clipId);
        clipId = -1;
        if (samples && samples.length > 0 && audioCtx) {
            clipId = audioCtx.createClip(samples);
        }
        if (wasPlaying) Editor.play();
    }

    // -----------------------------------------------------------------------
    // Transport
    // -----------------------------------------------------------------------

    Editor.play = function() {
        if (!samples || clipId < 0) return;
        Editor.stop();

        var start, end;
        if (hasSelection) {
            start = selStart;
            end = selEnd;
        } else {
            start = cursorPos;
            end = samples.length;
        }
        if (start >= end) { start = 0; end = samples.length; }

        playbackId = audioCtx.playClip(clipId, 1.0, looping);
        if (playbackId >= 0) {
            audioCtx.setPlaybackRegion(playbackId, start, end);
            playFromSample = start;
            isPlaying = true;
            playbackStarted = false;
            animatePlayhead();
        }
    };

    Editor.stop = function() {
        if (playbackId >= 0 && audioCtx) {
            audioCtx.stopPlayback(playbackId);
        }
        playbackId = -1;
        isPlaying = false;
        if (animFrame) {
            cancelAnimationFrame(animFrame);
            animFrame = 0;
        }
        Editor.draw();
    };

    Editor.toggleLoop = function() {
        looping = !looping;
        if (playbackId >= 0 && audioCtx) {
            audioCtx.setPlaybackLoop(playbackId, looping);
        }
        return looping;
    };

    Editor.isPlaying = function() { return isPlaying; };
    Editor.isLooping = function() { return looping; };

    var playbackStarted = false; // tracks whether pos has moved above 0

    function animatePlayhead() {
        if (!isPlaying) return;
        if (playbackId >= 0 && audioCtx) {
            var pos = audioCtx.getPlaybackPosition(playbackId);
            var start = playFromSample;
            var end = hasSelection ? selEnd : samples.length;
            var regionLen = end - start;
            var currentSample = start + Math.floor(pos * regionLen);

            // Track that playback has actually progressed (pos > 0 at least once)
            if (pos > 0) playbackStarted = true;

            cursorPos = currentSample;
            Editor.draw();
            updateTimeDisplay(currentSample);

            // Detect playback finished: pos returns to 0 after having progressed
            if (!looping && playbackStarted && pos <= 0) {
                cursorPos = start;
                isPlaying = false;
                playbackStarted = false;
                Editor.draw();
                return;
            }
        }
        animFrame = requestAnimationFrame(animatePlayhead);
    }

    // -----------------------------------------------------------------------
    // Undo / Redo
    // -----------------------------------------------------------------------

    function pushUndo() {
        undoStack.push({
            samples: new Float32Array(samples),
            selStart: selStart, selEnd: selEnd, hasSelection: hasSelection,
            viewStart: viewStart, viewEnd: viewEnd
        });
        if (undoStack.length > MAX_UNDO) undoStack.shift();
        redoStack = [];
    }

    Editor.undo = function() {
        if (undoStack.length === 0) return;
        redoStack.push({
            samples: samples, selStart: selStart, selEnd: selEnd,
            hasSelection: hasSelection, viewStart: viewStart, viewEnd: viewEnd
        });
        var state = undoStack.pop();
        samples = state.samples;
        selStart = state.selStart;
        selEnd = state.selEnd;
        hasSelection = state.hasSelection;
        viewStart = state.viewStart;
        viewEnd = state.viewEnd;
        recreateClip();
        Editor.draw();
        updateStatus();
    };

    Editor.redo = function() {
        if (redoStack.length === 0) return;
        undoStack.push({
            samples: samples, selStart: selStart, selEnd: selEnd,
            hasSelection: hasSelection, viewStart: viewStart, viewEnd: viewEnd
        });
        var state = redoStack.pop();
        samples = state.samples;
        selStart = state.selStart;
        selEnd = state.selEnd;
        hasSelection = state.hasSelection;
        viewStart = state.viewStart;
        viewEnd = state.viewEnd;
        recreateClip();
        Editor.draw();
        updateStatus();
    };

    // -----------------------------------------------------------------------
    // Editing operations
    // -----------------------------------------------------------------------

    Editor.trimToSelection = function() {
        if (!samples || !hasSelection) return;
        pushUndo();
        samples = samples.slice(selStart, selEnd);
        viewStart = 0;
        viewEnd = samples.length;
        selStart = 0;
        selEnd = 0;
        hasSelection = false;
        recreateClip();
        Editor.draw();
        updateStatus();
        setStatus('Trimmed to selection');
    };

    Editor.deleteSelection = function() {
        if (!samples || !hasSelection) return;
        pushUndo();
        var before = samples.slice(0, selStart);
        var after = samples.slice(selEnd);
        samples = concatArrays(before, after);
        clampView();
        hasSelection = false;
        recreateClip();
        Editor.draw();
        updateStatus();
        setStatus('Deleted selection');
    };

    Editor.cut = function() {
        if (!samples || !hasSelection) return;
        clipboard = samples.slice(selStart, selEnd);
        Editor.deleteSelection();
        setStatus('Cut ' + formatTime(clipboard.length / sampleRate));
    };

    Editor.copy = function() {
        if (!samples || !hasSelection) return;
        clipboard = samples.slice(selStart, selEnd);
        setStatus('Copied ' + formatTime(clipboard.length / sampleRate));
    };

    Editor.paste = function() {
        if (!samples || !clipboard) return;
        pushUndo();
        var insertAt = hasSelection ? selStart : samples.length;
        var before = samples.slice(0, insertAt);
        var after = hasSelection ? samples.slice(selEnd) : new Float32Array(0);
        samples = concatArrays(concatArrays(before, clipboard), after);
        selStart = insertAt;
        selEnd = insertAt + clipboard.length;
        hasSelection = true;
        clampView();
        recreateClip();
        Editor.draw();
        updateStatus();
        setStatus('Pasted ' + formatTime(clipboard.length / sampleRate));
    };

    Editor.silenceSelection = function() {
        if (!samples || !hasSelection) return;
        pushUndo();
        for (var i = selStart; i < selEnd; i++) samples[i] = 0;
        recreateClip();
        Editor.draw();
        setStatus('Silenced selection');
    };

    Editor.reverse = function() {
        if (!samples) return;
        pushUndo();
        var start = hasSelection ? selStart : 0;
        var end = hasSelection ? selEnd : samples.length;
        var left = start, right = end - 1;
        while (left < right) {
            var tmp = samples[left];
            samples[left] = samples[right];
            samples[right] = tmp;
            left++;
            right--;
        }
        recreateClip();
        Editor.draw();
        setStatus('Reversed' + (hasSelection ? ' selection' : ''));
    };

    Editor.normalize = function() {
        if (!samples) return;
        pushUndo();
        var start = hasSelection ? selStart : 0;
        var end = hasSelection ? selEnd : samples.length;
        var peak = 0;
        for (var i = start; i < end; i++) {
            var abs = Math.abs(samples[i]);
            if (abs > peak) peak = abs;
        }
        if (peak > 0 && peak !== 1) {
            var scale = 1.0 / peak;
            for (var i = start; i < end; i++) samples[i] *= scale;
        }
        recreateClip();
        Editor.draw();
        setStatus('Normalized to 0dB (peak was ' + (20 * Math.log10(peak)).toFixed(1) + 'dB)');
    };

    Editor.adjustGain = function(dB) {
        if (!samples) return;
        pushUndo();
        var start = hasSelection ? selStart : 0;
        var end = hasSelection ? selEnd : samples.length;
        var scale = Math.pow(10, dB / 20);
        for (var i = start; i < end; i++) samples[i] *= scale;
        recreateClip();
        Editor.draw();
        setStatus('Gain ' + (dB >= 0 ? '+' : '') + dB + 'dB');
    };

    Editor.fadeIn = function() {
        if (!samples || !hasSelection) return;
        pushUndo();
        var len = selEnd - selStart;
        for (var i = 0; i < len; i++) {
            samples[selStart + i] *= i / len;
        }
        recreateClip();
        Editor.draw();
        setStatus('Fade in applied');
    };

    Editor.fadeOut = function() {
        if (!samples || !hasSelection) return;
        pushUndo();
        var len = selEnd - selStart;
        for (var i = 0; i < len; i++) {
            samples[selStart + i] *= 1 - (i / len);
        }
        recreateClip();
        Editor.draw();
        setStatus('Fade out applied');
    };

    Editor.pitchShift = function(semitones) {
        if (!samples) return;
        pushUndo();
        var start = hasSelection ? selStart : 0;
        var end = hasSelection ? selEnd : samples.length;
        var src = samples.slice(start, end);
        var rate = Math.pow(2, semitones / 12);
        var newLen = Math.round(src.length / rate);
        var resampled = new Float32Array(newLen);
        for (var i = 0; i < newLen; i++) {
            var srcPos = i * rate;
            var idx = Math.floor(srcPos);
            var frac = srcPos - idx;
            var s0 = idx < src.length ? src[idx] : 0;
            var s1 = idx + 1 < src.length ? src[idx + 1] : s0;
            resampled[i] = s0 + frac * (s1 - s0);
        }
        // Replace region with resampled data
        var before = samples.slice(0, start);
        var after = samples.slice(end);
        samples = concatArrays(concatArrays(before, resampled), after);
        selEnd = start + newLen;
        clampView();
        recreateClip();
        Editor.draw();
        updateStatus();
        setStatus('Pitch shifted ' + (semitones >= 0 ? '+' : '') + semitones + ' semitones');
    };

    Editor.timeStretch = function(factor) {
        // Simple time stretch by resampling (changes pitch too — basic approach)
        // factor > 1 = slower/longer, < 1 = faster/shorter
        if (!samples || factor <= 0) return;
        pushUndo();
        var start = hasSelection ? selStart : 0;
        var end = hasSelection ? selEnd : samples.length;
        var src = samples.slice(start, end);
        var newLen = Math.round(src.length * factor);
        var stretched = new Float32Array(newLen);
        for (var i = 0; i < newLen; i++) {
            var srcPos = i / factor;
            var idx = Math.floor(srcPos);
            var frac = srcPos - idx;
            var s0 = idx < src.length ? src[idx] : 0;
            var s1 = idx + 1 < src.length ? src[idx + 1] : s0;
            stretched[i] = s0 + frac * (s1 - s0);
        }
        var before = samples.slice(0, start);
        var after = samples.slice(end);
        samples = concatArrays(concatArrays(before, stretched), after);
        selEnd = start + newLen;
        clampView();
        recreateClip();
        Editor.draw();
        updateStatus();
        setStatus('Time stretched x' + factor.toFixed(2));
    };

    Editor.insertSilence = function(durationMs) {
        if (!samples) return;
        pushUndo();
        var insertAt = hasSelection ? selStart : samples.length;
        var numSamples = Math.round(durationMs / 1000 * sampleRate);
        var silence = new Float32Array(numSamples);
        var before = samples.slice(0, insertAt);
        var after = samples.slice(insertAt);
        samples = concatArrays(concatArrays(before, silence), after);
        selStart = insertAt;
        selEnd = insertAt + numSamples;
        hasSelection = true;
        clampView();
        recreateClip();
        Editor.draw();
        updateStatus();
        setStatus('Inserted ' + durationMs + 'ms silence');
    };

    // -----------------------------------------------------------------------
    // Selection
    // -----------------------------------------------------------------------

    Editor.selectAll = function() {
        if (!samples) return;
        selStart = 0;
        selEnd = samples.length;
        hasSelection = true;
        Editor.draw();
        updateStatus();
    };

    Editor.clearSelection = function() {
        hasSelection = false;
        selStart = selEnd = 0;
        Editor.draw();
        updateStatus();
    };

    Editor.getSelection = function() {
        return hasSelection ? { start: selStart, end: selEnd } : null;
    };

    // -----------------------------------------------------------------------
    // Zoom
    // -----------------------------------------------------------------------

    Editor.zoomIn = function() {
        if (!samples) return;
        var center = (viewStart + viewEnd) / 2;
        var halfLen = (viewEnd - viewStart) / 4; // halve visible range
        halfLen = Math.max(halfLen, 100); // minimum visible samples
        viewStart = Math.max(0, Math.floor(center - halfLen));
        viewEnd = Math.min(samples.length, Math.ceil(center + halfLen));
        Editor.draw();
        updateZoom();
    };

    Editor.zoomOut = function() {
        if (!samples) return;
        var center = (viewStart + viewEnd) / 2;
        var halfLen = (viewEnd - viewStart); // double visible range
        viewStart = Math.max(0, Math.floor(center - halfLen));
        viewEnd = Math.min(samples.length, Math.ceil(center + halfLen));
        Editor.draw();
        updateZoom();
    };

    Editor.zoomToFit = function() {
        if (!samples) return;
        viewStart = 0;
        viewEnd = samples.length;
        Editor.draw();
        updateZoom();
    };

    Editor.zoomToSelection = function() {
        if (!samples || !hasSelection) return;
        var pad = Math.max(1, Math.floor((selEnd - selStart) * 0.05));
        viewStart = Math.max(0, selStart - pad);
        viewEnd = Math.min(samples.length, selEnd + pad);
        Editor.draw();
        updateZoom();
    };

    // -----------------------------------------------------------------------
    // Canvas drawing
    // -----------------------------------------------------------------------

    // Clear the canvas scene completely so nothing renders
    Editor.clear = function() {
        if (!canvas) return;
        var ctx = canvas.getContext('2d');
        ctx.reset();
    };

    Editor.draw = function() {
        if (!canvas) return;
        var ctx = canvas.getContext('2d');
        var w = ctx.canvasWidth;
        var h = ctx.canvasHeight;
        if (w <= 0 || h <= 0) return; // not laid out yet
        cachedW = w;
        cachedH = h;

        ctx.clearRect(0, 0, w, h);

        // --- Background ---
        ctx.fillStyle = '#121218';
        ctx.fillRect(0, 0, w, h);
        // Ruler background
        ctx.fillStyle = '#0e0e14';
        ctx.fillRect(0, 0, w, RULER_H);

        if (!samples || samples.length === 0) {
            ctx.fillStyle = '#444';
            ctx.font = '13px system-ui';
            ctx.fillText('No audio loaded', w / 2 - 50, h / 2 - 10);
            ctx.fillStyle = '#333';
            ctx.font = '11px system-ui';
            ctx.fillText('Load a file, Record, or Generate a tone.', w / 2 - 110, h / 2 + 10);
            return;
        }

        var visibleLen = viewEnd - viewStart;
        if (visibleLen <= 0) return;

        var waveTop = RULER_H;
        var waveH = h - RULER_H;
        var midY = waveTop + waveH / 2;

        // --- Compute min/max per pixel bin ---
        var numBins = Math.min(Math.floor(w), visibleLen);
        if (numBins <= 0) return;
        var samplesPerBin = visibleLen / numBins;

        var mins = new Float32Array(numBins);
        var maxs = new Float32Array(numBins);
        var rms = new Float32Array(numBins);
        var peakAbs = 0;

        for (var i = 0; i < numBins; i++) {
            var bStart = viewStart + Math.floor(i * samplesPerBin);
            var bEnd = viewStart + Math.floor((i + 1) * samplesPerBin);
            if (bEnd > samples.length) bEnd = samples.length;
            var lo = 1, hi = -1, sumSq = 0, count = 0;
            for (var j = bStart; j < bEnd; j++) {
                var s = samples[j];
                if (s < lo) lo = s;
                if (s > hi) hi = s;
                sumSq += s * s;
                count++;
            }
            mins[i] = lo;
            maxs[i] = hi;
            rms[i] = count > 0 ? Math.sqrt(sumSq / count) : 0;
            var absMax = Math.max(Math.abs(lo), Math.abs(hi));
            if (absMax > peakAbs) peakAbs = absMax;
        }

        // Auto-scale: fit peak to 85% of waveform area
        var scale = peakAbs > 0.001 ? 0.85 / peakAbs : 1.0;
        var halfH = waveH / 2;

        // --- Selection highlight ---
        if (hasSelection) {
            var sx1 = sampleToX(selStart, w);
            var sx2 = sampleToX(selEnd, w);
            // Dim the unselected regions
            ctx.fillStyle = 'rgba(0, 0, 0, 0.35)';
            ctx.fillRect(0, waveTop, sx1, waveH);
            ctx.fillRect(sx2, waveTop, w - sx2, waveH);
            // Selected region tint
            ctx.fillStyle = 'rgba(90, 180, 255, 0.06)';
            ctx.fillRect(sx1, waveTop, sx2 - sx1, waveH);
        }

        // --- Grid lines ---
        // Center line (zero crossing)
        ctx.strokeStyle = '#252530';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, midY);
        ctx.lineTo(w, midY);
        ctx.stroke();
        // ±0.5 lines
        ctx.strokeStyle = '#1a1a24';
        ctx.beginPath();
        ctx.moveTo(0, midY - halfH * 0.5);
        ctx.lineTo(w, midY - halfH * 0.5);
        ctx.moveTo(0, midY + halfH * 0.5);
        ctx.lineTo(w, midY + halfH * 0.5);
        ctx.stroke();

        // --- Waveform: per-column vertical bars (DAW style) ---
        var binW = w / numBins;
        for (var i = 0; i < numBins; i++) {
            var x = i * binW;
            var yTop = midY - maxs[i] * scale * halfH;
            var yBot = midY - mins[i] * scale * halfH;
            var barH = yBot - yTop;
            if (barH < 1) barH = 1;

            // Outer waveform (peak): solid fill
            ctx.fillStyle = '#1a4a5a';
            ctx.fillRect(x, yTop, binW < 1 ? 1 : binW, barH);

            // Inner RMS: brighter core
            var rmsH = rms[i] * scale * halfH;
            if (rmsH > 0.5) {
                ctx.fillStyle = '#2a90a8';
                ctx.fillRect(x, midY - rmsH, binW < 1 ? 1 : binW, rmsH * 2);
            }
        }

        // --- Waveform top/bottom edge highlight ---
        ctx.strokeStyle = '#40c0d8';
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (var i = 0; i < numBins; i++) {
            var x = i * binW + binW * 0.5;
            var yTop = midY - maxs[i] * scale * halfH;
            if (i === 0) ctx.moveTo(x, yTop);
            else ctx.lineTo(x, yTop);
        }
        ctx.stroke();
        ctx.beginPath();
        for (var i = 0; i < numBins; i++) {
            var x = i * binW + binW * 0.5;
            var yBot = midY - mins[i] * scale * halfH;
            if (i === 0) ctx.moveTo(x, yBot);
            else ctx.lineTo(x, yBot);
        }
        ctx.stroke();

        // --- Selection edges ---
        if (hasSelection) {
            var sx1 = sampleToX(selStart, w);
            var sx2 = sampleToX(selEnd, w);
            ctx.fillStyle = 'rgba(90, 180, 255, 0.7)';
            ctx.fillRect(sx1, waveTop, 1, waveH);
            ctx.fillRect(sx2 - 1, waveTop, 1, waveH);
        }

        // --- Time ruler ---
        drawTimeRuler(ctx, w, RULER_H);

        // --- Playhead / Cursor ---
        var cpx = sampleToX(cursorPos, w);
        if (cpx >= 0 && cpx <= w) {
            if (isPlaying) {
                // Playing: bright red line
                ctx.fillStyle = '#ff4444';
                ctx.fillRect(cpx - 1, 0, 2, h);
                // Small triangle at top
                ctx.beginPath();
                ctx.moveTo(cpx - 4, 0);
                ctx.lineTo(cpx + 4, 0);
                ctx.lineTo(cpx, 6);
                ctx.closePath();
                ctx.fill();
            } else {
                // Stopped: white cursor line
                ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
                ctx.fillRect(cpx, RULER_H, 1, waveH);
                // Small triangle at top
                ctx.fillStyle = 'rgba(255, 255, 255, 0.8)';
                ctx.beginPath();
                ctx.moveTo(cpx - 3, RULER_H);
                ctx.lineTo(cpx + 3, RULER_H);
                ctx.lineTo(cpx, RULER_H + 5);
                ctx.closePath();
                ctx.fill();
            }
        }

        // --- Info labels ---
        ctx.fillStyle = '#444';
        ctx.font = '9px monospace';
        var peakDb = peakAbs > 0 ? (20 * Math.log10(peakAbs)).toFixed(1) + 'dB' : '-inf';
        ctx.fillText(peakDb, w - 40, RULER_H + 12);
    };

    function drawTimeRuler(ctx, w, rulerH) {
        var visibleDur = (viewEnd - viewStart) / sampleRate;
        // Choose tick interval for major ticks
        var intervals = [0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 30, 60];
        var targetTicks = w / 90;
        var interval = intervals[0];
        for (var i = 0; i < intervals.length; i++) {
            if (visibleDur / intervals[i] <= targetTicks) { interval = intervals[i]; break; }
        }

        var startTime = viewStart / sampleRate;
        var firstTick = Math.ceil(startTime / interval) * interval;

        // Minor ticks (subdivisions)
        var minorInterval = interval / 4;
        var firstMinor = Math.ceil(startTime / minorInterval) * minorInterval;

        // Draw minor ticks
        ctx.strokeStyle = '#1e1e28';
        ctx.lineWidth = 1;
        for (var t = firstMinor; t * sampleRate < viewEnd; t += minorInterval) {
            var x = sampleToX(Math.round(t * sampleRate), w);
            ctx.beginPath();
            ctx.moveTo(x, rulerH - 3);
            ctx.lineTo(x, rulerH);
            ctx.stroke();
        }

        // Draw major ticks with labels
        ctx.fillStyle = '#666';
        ctx.font = '9px monospace';
        ctx.strokeStyle = '#2a2a38';
        ctx.lineWidth = 1;

        for (var t = firstTick; t * sampleRate < viewEnd; t += interval) {
            var x = sampleToX(Math.round(t * sampleRate), w);
            // Tick mark
            ctx.beginPath();
            ctx.moveTo(x, rulerH - 8);
            ctx.lineTo(x, rulerH);
            ctx.stroke();
            // Label
            ctx.fillText(formatTime(t), x + 3, rulerH - 3);
        }

        // Bottom border
        ctx.strokeStyle = '#2a2a3a';
        ctx.beginPath();
        ctx.moveTo(0, rulerH);
        ctx.lineTo(w, rulerH);
        ctx.stroke();
    }

    // -----------------------------------------------------------------------
    // Mouse interaction — DAW-style controls
    //   Click:         place cursor (playhead)
    //   Drag:          select region
    //   Double-click:  select all
    //   Scroll:        zoom in/out toward cursor
    //   Shift+Scroll:  horizontal scroll (pan)
    //   Ruler click:   seek playhead (even during playback)
    // -----------------------------------------------------------------------

    function sampleToX(sample, w) {
        return ((sample - viewStart) / (viewEnd - viewStart)) * w;
    }

    function xToSample(x, w) {
        var s = Math.round(viewStart + (x / w) * (viewEnd - viewStart));
        return Math.max(0, Math.min(samples ? samples.length : 0, s));
    }

    function canvasXY(e) {
        return { x: e.offsetX, y: e.offsetY };
    }

    function onMouseDown(e) {
        if (!samples) return;
        var w = cachedW;
        var pos = canvasXY(e);
        var x = pos.x, y = pos.y;

        dragging = true;
        hasDragged = false;
        dragStartX = x;
        dragStartSample = xToSample(x, w);

        // Click in ruler area: seek only (don't start selection)
        if (y < RULER_H) {
            cursorPos = dragStartSample;
            if (isPlaying) seekPlayback(cursorPos);
            hasSelection = false;
            selStart = selEnd = 0;
            Editor.draw();
            updateTimeDisplay(cursorPos);
            dragging = false;
            return;
        }

        // Place cursor immediately
        cursorPos = dragStartSample;
        hasSelection = false;
        selStart = selEnd = 0;
        Editor.draw();
        updateTimeDisplay(cursorPos);
    }

    function onMouseMove(e) {
        if (!samples) return;
        var w = cachedW;
        var x = canvasXY(e).x;

        if (!dragging) {
            // Hover: update time display
            var hoverSample = xToSample(x, w);
            updateTimeDisplay(hoverSample);
            return;
        }

        var curSample = xToSample(x, w);

        // Threshold before starting selection (3px)
        if (!hasDragged && Math.abs(x - dragStartX) > 3) {
            hasDragged = true;
        }

        if (hasDragged) {
            if (curSample < dragStartSample) {
                selStart = curSample;
                selEnd = dragStartSample;
            } else {
                selStart = dragStartSample;
                selEnd = curSample;
            }
            hasSelection = (selEnd - selStart) > 10; // need at least ~10 samples
            // Don't move cursor during drag — keep it at the click origin
        }
        Editor.draw();
        updateStatus();
    }

    function onMouseUp(e) {
        if (!samples) { dragging = false; return; }
        if (dragging) {
            if (!hasDragged) {
                // Single click: place cursor, clear selection
                var w = cachedW;
                var x = canvasXY(e).x;
                cursorPos = xToSample(x, w);
                hasSelection = false;
                selStart = selEnd = 0;
                if (isPlaying) seekPlayback(cursorPos);
            }
            Editor.draw();
            updateStatus();
        }
        dragging = false;
    }

    function onDblClick(e) {
        if (!samples) return;
        // Double-click: select all visible
        Editor.selectAll();
    }

    function onWheel(e) {
        if (!samples) return;
        var w = cachedW;
        var x = canvasXY(e).x;
        var visLen = viewEnd - viewStart;

        if (e.shiftKey) {
            // Shift+scroll: horizontal pan
            var scrollAmount = Math.round(visLen * 0.15);
            if (e.deltaY > 0 || e.deltaX > 0) {
                // Scroll right
                viewStart = Math.min(samples.length - visLen, viewStart + scrollAmount);
                viewEnd = viewStart + visLen;
            } else {
                // Scroll left
                viewStart = Math.max(0, viewStart - scrollAmount);
                viewEnd = viewStart + visLen;
            }
        } else {
            // Scroll: zoom in/out toward mouse position
            var mousePos = xToSample(x, w);
            var ratio = (mousePos - viewStart) / visLen;

            if (e.deltaY < 0) {
                // Zoom in
                var newLen = Math.max(200, Math.round(visLen * 0.7));
                viewStart = Math.max(0, Math.round(mousePos - newLen * ratio));
                viewEnd = Math.min(samples.length, viewStart + newLen);
            } else {
                // Zoom out
                var newLen = Math.min(samples.length, Math.round(visLen * 1.4));
                viewStart = Math.max(0, Math.round(mousePos - newLen * ratio));
                viewEnd = Math.min(samples.length, viewStart + newLen);
            }
        }
        Editor.draw();
        updateZoom();
    }

    // Seek playback to a new position while playing
    function seekPlayback(sample) {
        if (!isPlaying || playbackId < 0) return;
        var end = hasSelection ? selEnd : samples.length;
        if (sample >= end) sample = end - 1;
        audioCtx.stopPlayback(playbackId);
        playbackId = audioCtx.playClip(clipId, 1.0, looping);
        if (playbackId >= 0) {
            audioCtx.setPlaybackRegion(playbackId, sample, end);
            playFromSample = sample;
        }
    }

    // -----------------------------------------------------------------------
    // Synth integration — use clip as instrument
    // -----------------------------------------------------------------------

    Editor.useAsInstrument = function() {
        if (!samples || clipId < 0) return;
        // Store reference for synth keyboard to use
        Synth.customClipId = clipId;
        Synth.customClipSamples = samples;
        Synth.useClipMode = true;
        setStatus('Clip set as instrument — play keyboard to hear it!');
    };

    Editor.clearInstrument = function() {
        Synth.useClipMode = false;
        Synth.customClipId = -1;
        Synth.customClipSamples = null;
        setStatus('Instrument reset to oscillator');
    };

    // -----------------------------------------------------------------------
    // Generators
    // -----------------------------------------------------------------------

    Editor.generateTone = function(freq, durationMs, waveType) {
        var numSamples = Math.round(durationMs / 1000 * sampleRate);
        var buf = new Float32Array(numSamples);
        var phase = 0;
        var inc = freq / sampleRate;

        for (var i = 0; i < numSamples; i++) {
            switch (waveType) {
                case 'square':
                    buf[i] = phase < 0.5 ? 0.8 : -0.8;
                    break;
                case 'sawtooth':
                    buf[i] = (2 * phase - 1) * 0.8;
                    break;
                case 'triangle':
                    buf[i] = (4 * Math.abs(phase - 0.5) - 1) * 0.8;
                    break;
                default: // sine
                    buf[i] = Math.sin(2 * Math.PI * phase) * 0.8;
            }
            phase += inc;
            if (phase >= 1) phase -= 1;
        }
        Editor.loadSamples(buf);
        setStatus('Generated ' + freq + 'Hz ' + (waveType || 'sine') + ' (' + durationMs + 'ms)');
    };

    Editor.generateNoise = function(durationMs) {
        var numSamples = Math.round(durationMs / 1000 * sampleRate);
        var buf = new Float32Array(numSamples);
        for (var i = 0; i < numSamples; i++) {
            buf[i] = (Math.random() * 2 - 1) * 0.8;
        }
        Editor.loadSamples(buf);
        setStatus('Generated white noise (' + durationMs + 'ms)');
    };

    // -----------------------------------------------------------------------
    // Utilities
    // -----------------------------------------------------------------------

    function concatArrays(a, b) {
        var result = new Float32Array(a.length + b.length);
        result.set(a, 0);
        result.set(b, a.length);
        return result;
    }

    function clampView() {
        if (!samples) return;
        if (viewEnd > samples.length) viewEnd = samples.length;
        if (viewStart >= viewEnd) viewStart = Math.max(0, viewEnd - 1000);
        if (selStart > samples.length) selStart = samples.length;
        if (selEnd > samples.length) selEnd = samples.length;
        if (selStart >= selEnd) hasSelection = false;
    }

    function formatTime(seconds) {
        if (seconds < 0) seconds = 0;
        var mins = Math.floor(seconds / 60);
        var secs = seconds - mins * 60;
        if (mins > 0) return mins + ':' + (secs < 10 ? '0' : '') + secs.toFixed(2);
        if (seconds >= 1) return secs.toFixed(2) + 's';
        return (seconds * 1000).toFixed(1) + 'ms';
    }

    function setStatus(text) {
        if (statusEl) statusEl.textContent = text;
    }

    function updateStatus() {
        if (!samples) {
            setStatus('No clip loaded');
            return;
        }
        var dur = formatTime(samples.length / sampleRate);
        var info = samples.length + ' samples | ' + dur + ' | ' + sampleRate + 'Hz';
        if (hasSelection) {
            var selDur = formatTime((selEnd - selStart) / sampleRate);
            info += ' | Sel: ' + selDur + ' (' + (selEnd - selStart) + ' smp)';
        }
        setStatus(info);
    }

    function updateTimeDisplay(sample) {
        if (timeDisplay) {
            timeDisplay.textContent = formatTime((sample || 0) / sampleRate);
        }
    }

    function updateZoom() {
        if (!zoomDisplay || !samples) return;
        var pct = Math.round(samples.length / (viewEnd - viewStart) * 100);
        zoomDisplay.textContent = pct + '%';
    }

    // Keyboard shortcuts for clip editor
    Editor.handleKey = function(e) {
        if (!samples) return false;
        var key = e.key;
        var ctrl = e.ctrlKey;

        // Edit
        if (ctrl && key === 'z') { Editor.undo(); return true; }
        if (ctrl && key === 'y') { Editor.redo(); return true; }
        if (ctrl && key === 'x') { Editor.cut(); return true; }
        if (ctrl && key === 'c') { Editor.copy(); return true; }
        if (ctrl && key === 'v') { Editor.paste(); return true; }
        if (ctrl && key === 'a') { Editor.selectAll(); return true; }
        if (key === 'Delete' || key === 'Backspace') { Editor.deleteSelection(); return true; }

        // Transport
        if (key === ' ') { isPlaying ? Editor.stop() : Editor.play(); return true; }

        // Navigation — Home/End to jump cursor
        if (key === 'Home') { cursorPos = 0; Editor.draw(); updateTimeDisplay(0); return true; }
        if (key === 'End') { cursorPos = samples.length; Editor.draw(); updateTimeDisplay(cursorPos); return true; }

        // Arrow keys: nudge cursor / selection
        if (key === 'ArrowLeft' || key === 'ArrowRight') {
            var step = Math.max(1, Math.round((viewEnd - viewStart) / 100));
            if (ctrl) step *= 10;
            if (key === 'ArrowLeft') cursorPos = Math.max(0, cursorPos - step);
            else cursorPos = Math.min(samples.length, cursorPos + step);
            // Shift+arrow: extend selection
            if (e.shiftKey) {
                if (!hasSelection) { selStart = dragStartSample || cursorPos; selEnd = cursorPos; }
                if (cursorPos < selStart) selStart = cursorPos;
                else selEnd = cursorPos;
                hasSelection = (selEnd - selStart) > 1;
            }
            // Auto-scroll if cursor goes out of view
            if (cursorPos < viewStart || cursorPos > viewEnd) {
                var visLen = viewEnd - viewStart;
                viewStart = Math.max(0, cursorPos - Math.round(visLen * 0.1));
                viewEnd = viewStart + visLen;
                if (viewEnd > samples.length) { viewEnd = samples.length; viewStart = viewEnd - visLen; }
            }
            Editor.draw();
            updateTimeDisplay(cursorPos);
            return true;
        }

        // Zoom
        if (key === '+' || key === '=') { Editor.zoomIn(); return true; }
        if (key === '-') { Editor.zoomOut(); return true; }
        if (key === '0') { Editor.zoomToFit(); return true; }

        // Loop toggle
        if (key === 'l' || key === 'L') {
            var on = Editor.toggleLoop();
            document.getElementById('ed-loop').classList.toggle('active', on);
            return true;
        }

        // Escape: clear selection
        if (key === 'Escape') { Editor.clearSelection(); return true; }

        return false;
    };
})();
