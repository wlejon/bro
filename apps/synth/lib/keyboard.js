(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    // Home row layout: white keys on A-;, sharps on W-P
    var KEY_MAP = {
        'a': 0, 'w': 1, 's': 2, 'e': 3, 'd': 4, 'f': 5, 't': 6,
        'g': 7, 'y': 8, 'h': 9, 'u': 10, 'j': 11,
        'k': 12, 'o': 13, 'l': 14, 'p': 15, ';': 16
    };

    var VIEW_NOTES = 17;
    var viewOffset = 24; // C3
    var MIN_VIEW = 0;
    var MAX_VIEW;

    var keyboard = null;
    var octaveDisplay = null;
    var keysDown = new Set();
    var mouseDown = false;
    var mouseNoteIdx = -1;

    function $$(sel) { return Array.from(document.querySelectorAll(sel)); }

    function buildKeyboard() {
        var notes = Synth.notes;
        MAX_VIEW = notes.length - VIEW_NOTES;

        keyboard.innerHTML = '';
        for (var i = 0; i < notes.length; i++) notes[i].element = null;

        var viewNotes = notes.slice(viewOffset, viewOffset + VIEW_NOTES);
        var visWhite = viewNotes.filter(function(n) { return !n.isBlack; });
        var visBlack = viewNotes.filter(function(n) { return n.isBlack; });

        visWhite.forEach(function(note) {
            var el = document.createElement('div');
            el.className = 'white-key';
            var noteIdx = notes.indexOf(note);
            var relIdx = noteIdx - viewOffset;
            el.setAttribute('data-note-idx', noteIdx.toString());

            if (Synth.getActiveNotes().has(noteIdx)) el.classList.add('pressed');

            var binding = Object.entries(KEY_MAP).find(function(e) { return e[1] === relIdx; });
            var label = document.createElement('div');
            label.className = 'key-label';
            label.textContent = binding ? binding[0].toUpperCase() : '';
            el.appendChild(label);

            var nameLabel = document.createElement('div');
            nameLabel.className = 'key-note-label';
            nameLabel.textContent = note.name;
            el.appendChild(nameLabel);

            keyboard.appendChild(el);
            note.element = el;
        });

        visBlack.forEach(function(note) {
            var noteIdx = notes.indexOf(note);
            var relIdx = noteIdx - viewOffset;
            var whiteIdx = visWhite.filter(function(w) { return notes.indexOf(w) < noteIdx; }).length;
            var whiteKeyWidth = 100 / visWhite.length;
            var leftPos = whiteIdx * whiteKeyWidth - whiteKeyWidth * 0.3;

            var el = document.createElement('div');
            el.className = 'black-key';
            el.setAttribute('data-note-idx', noteIdx.toString());
            el.style.left = leftPos + '%';
            el.style.width = (whiteKeyWidth * 0.6) + '%';

            if (Synth.getActiveNotes().has(noteIdx)) el.classList.add('pressed');

            var binding = Object.entries(KEY_MAP).find(function(e) { return e[1] === relIdx; });
            var label = document.createElement('div');
            label.className = 'key-label';
            label.textContent = binding ? binding[0].toUpperCase() : '';
            el.appendChild(label);

            keyboard.appendChild(el);
            note.element = el;
        });

        $$('.white-key').forEach(attachMouseHandlers);
        $$('.black-key').forEach(attachMouseHandlers);

        octaveDisplay.textContent = Synth.notes[viewOffset].name;
    }

    function shiftView(semitones) {
        keysDown.forEach(function(key) {
            if (key in KEY_MAP) Synth.noteOff(KEY_MAP[key] + viewOffset);
        });
        keysDown.clear();

        var newOffset = Math.max(MIN_VIEW, Math.min(MAX_VIEW, viewOffset + semitones));
        if (newOffset !== viewOffset) {
            viewOffset = newOffset;
            buildKeyboard();
        }
    }

    function attachMouseHandlers(el) {
        el.addEventListener('mousedown', function(e) {
            mouseDown = true;
            var idx = el.getAttribute('data-note-idx');
            if (idx !== null) {
                mouseNoteIdx = parseInt(idx);
                Synth.noteOn(mouseNoteIdx);
            }
        });
        el.addEventListener('mousemove', function() {
            if (!mouseDown) return;
            var idx = el.getAttribute('data-note-idx');
            if (idx !== null) {
                var newIdx = parseInt(idx);
                if (newIdx !== mouseNoteIdx) {
                    if (mouseNoteIdx >= 0) Synth.noteOff(mouseNoteIdx);
                    mouseNoteIdx = newIdx;
                    Synth.noteOn(mouseNoteIdx);
                }
            }
        });
    }

    Synth.Keyboard = {
        init: function(containerEl, octaveEl) {
            keyboard = containerEl;
            octaveDisplay = octaveEl;
            buildKeyboard();

            document.documentElement.addEventListener('keydown', function(e) {
                if (e.repeat) return;
                var key = e.key.toLowerCase();

                if (e.key === 'Tab') {
                    e.preventDefault();
                    // Blur focused element so Tab isn't consumed by focus cycling
                    if (document.activeElement && document.activeElement.blur) {
                        document.activeElement.blur();
                    }
                    shiftView(e.shiftKey ? -12 : 12);
                    return;
                }

                if (key in KEY_MAP && !keysDown.has(key)) {
                    keysDown.add(key);
                    Synth.noteOn(KEY_MAP[key] + viewOffset);
                }
            });

            document.documentElement.addEventListener('keyup', function(e) {
                var key = e.key.toLowerCase();
                if (key in KEY_MAP) {
                    keysDown.delete(key);
                    Synth.noteOff(KEY_MAP[key] + viewOffset);
                }
            });

            document.documentElement.addEventListener('mouseup', function() {
                if (mouseDown) {
                    mouseDown = false;
                    if (mouseNoteIdx >= 0) {
                        Synth.noteOff(mouseNoteIdx);
                        mouseNoteIdx = -1;
                    }
                }
            });
        },
        getViewOffset: function() { return viewOffset; }
    };
})();
