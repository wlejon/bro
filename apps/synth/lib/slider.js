// ---------------------------------------------------------------------------
// Custom Slider Widget — replaces <input type="range"> with fine-tuning UX
// ---------------------------------------------------------------------------
// Usage:
//   var s = Synth.Slider(container, { min, max, value, step, defaultValue,
//                                      format, onChange, fineScale, color });
//   s.setValue(50);  s.getValue();
//
// Interactions:
//   Click track    — jump to value
//   Drag           — adjust value
//   Shift+drag     — fine mode (10x precision)
//   Dbl-click val  — type exact number
//   Mouse wheel    — increment (shift = fine)
//   Right-click    — reset to default
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    function Slider(container, opts) {
        if (!(this instanceof Slider)) return new Slider(container, opts);

        var self = this;
        opts = opts || {};
        var min = opts.min !== undefined ? opts.min : 0;
        var max = opts.max !== undefined ? opts.max : 100;
        var step = opts.step || 1;
        var fineScale = opts.fineScale !== undefined ? opts.fineScale : 0.1;
        var defaultValue = opts.defaultValue !== undefined ? opts.defaultValue : min;
        var format = opts.format || function(v) { return String(v); };
        var onChange = opts.onChange || function() {};
        var color = opts.color || '#00e5ff';
        var value = clamp(opts.value !== undefined ? opts.value : defaultValue, min, max);

        // Build DOM: [track-area [track, fill, thumb]] [value]
        container.classList.add('fancy-slider');

        var trackArea = document.createElement('div');
        trackArea.className = 'fs-track-area';
        var track = document.createElement('div');
        track.className = 'fs-track';
        var fill = document.createElement('div');
        fill.className = 'fs-fill';
        fill.style.background = color;
        var thumb = document.createElement('div');
        thumb.className = 'fs-thumb';
        thumb.style.background = color;
        var valEl = document.createElement('div');
        valEl.className = 'fs-value';

        trackArea.appendChild(track);
        trackArea.appendChild(fill);
        trackArea.appendChild(thumb);
        container.appendChild(trackArea);
        container.appendChild(valEl);

        function pct() { return (max > min) ? (value - min) / (max - min) : 0; }

        function renderFast() {
            var p = pct();
            var trackW = trackArea.getBoundingClientRect().width;
            if (trackW <= 0) trackW = 80; // fallback before layout
            var px = Math.round(p * trackW);
            fill.style.width = px + 'px';
            thumb.style.left = px + 'px';
            valEl.textContent = format(value);
        }

        function setVal(v, silent) {
            v = Math.round(v / step) * step;
            v = clamp(v, min, max);
            v = Math.round(v * 1e6) / 1e6;
            if (v === value) return;
            value = v;
            renderFast();
            if (!silent) onChange(value);
        }

        // --- Drag interaction ---
        var dragging = false;
        var dragStartX = 0;
        var dragStartVal = 0;
        var fine = false;

        function posToValue(clientX) {
            var rect = trackArea.getBoundingClientRect();
            var p = clamp((clientX - rect.left) / rect.width, 0, 1);
            return min + p * (max - min);
        }

        trackArea.addEventListener('mousedown', function(e) {
            if (e.button === 2) return;
            e.preventDefault();

            dragging = true;
            fine = e.shiftKey;
            dragStartX = e.clientX;
            dragStartVal = value;

            if (!fine) {
                setVal(posToValue(e.clientX));
                dragStartVal = value;
                dragStartX = e.clientX;
            }

            container.classList.add('dragging');
            if (fine) container.classList.add('fine');
        });

        document.addEventListener('mousemove', function(e) {
            if (!dragging) return;
            fine = e.shiftKey;
            container.classList.toggle('fine', fine);

            if (fine) {
                var dx = e.clientX - dragStartX;
                var rect = trackArea.getBoundingClientRect();
                var range = max - min;
                var delta = (dx / rect.width) * range * fineScale;
                setVal(dragStartVal + delta);
            } else {
                setVal(posToValue(e.clientX));
            }
        });

        document.addEventListener('mouseup', function() {
            if (!dragging) return;
            dragging = false;
            container.classList.remove('dragging');
            container.classList.remove('fine');
        });

        // --- Right-click: reset to default ---
        container.addEventListener('contextmenu', function(e) {
            e.preventDefault();
            setVal(defaultValue);
        });

        // --- Mouse wheel ---
        container.addEventListener('wheel', function(e) {
            e.preventDefault();
            var mult = e.shiftKey ? fineScale : 1;
            var delta = e.deltaY < 0 ? step * mult : -step * mult;
            setVal(value + delta);
        });

        // --- Double-click value for text input ---
        valEl.addEventListener('dblclick', function(e) {
            e.stopPropagation();
            startTextEdit();
        });

        function startTextEdit() {
            var input = document.createElement('input');
            input.type = 'text';
            input.className = 'fs-input';
            input.value = String(value);
            valEl.textContent = '';
            valEl.appendChild(input);
            input.focus();
            input.select();

            function commit() {
                var parsed = parseFloat(input.value);
                if (!isNaN(parsed)) setVal(parsed);
                if (valEl.contains(input)) {
                    valEl.removeChild(input);
                }
                valEl.textContent = format(value);
            }

            input.addEventListener('keydown', function(e) {
                if (e.key === 'Enter') { e.preventDefault(); commit(); }
                if (e.key === 'Escape') {
                    if (valEl.contains(input)) valEl.removeChild(input);
                    valEl.textContent = format(value);
                }
                e.stopPropagation();
            });
            input.addEventListener('blur', commit);
        }

        // Public API
        self.setValue = function(v, silent) { setVal(v, silent); };
        self.getValue = function() { return value; };
        self.setColor = function(c) {
            color = c;
            fill.style.background = c;
            thumb.style.background = c;
        };
        self.render = renderFast;

        // Initial render
        renderFast();
    }

    Synth.Slider = Slider;
})();
