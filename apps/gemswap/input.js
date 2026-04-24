// input.js — named action setup for gemswap.
'use strict';
var G = G || {};

G.Controls = (function () {
    var ACTIONS = [
        { name: 'cursor_up',    label: 'Cursor Up',    defaults: ['w', 'ArrowUp']    },
        { name: 'cursor_down',  label: 'Cursor Down',  defaults: ['s', 'ArrowDown']  },
        { name: 'cursor_left',  label: 'Cursor Left',  defaults: ['a', 'ArrowLeft']  },
        { name: 'cursor_right', label: 'Cursor Right', defaults: ['d', 'ArrowRight'] },
        { name: 'pick',         label: 'Pick / Swap',  defaults: [' ', 'Enter']      },
        { name: 'pause_game',   label: 'Pause',        defaults: ['Escape', 'p']     },
        { name: 'hint',         label: 'Hint',         defaults: ['h']               },
        { name: 'shuffle',      label: 'Shuffle',      defaults: ['r']               },
    ];

    function init() {
        Input.init(ACTIONS, { storageKey: 'gemswap_controls' });
        Input.attach(window);
    }

    return { ACTIONS: ACTIONS, init: init };
})();
