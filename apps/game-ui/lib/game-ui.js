// ============================================================================
// game-ui.js — Game UI Component Library (core)
// ============================================================================
//
// Provides: theme system, focus manager, component registration helper.
// Components are defined in separate files, templates live in HTML.
//

(function() {
'use strict';

// ============================================================================
// Themes
// ============================================================================

var THEMES = {
    dark: {
        '--ui-bg':            '#0a0a1a',
        '--ui-surface':       '#141428',
        '--ui-surface-alt':   '#1c1c38',
        '--ui-surface-hover': '#222250',
        '--ui-border':        '#2a2a4a',
        '--ui-border-focus':  '#4a6aff',
        '--ui-text':          '#d0d0e0',
        '--ui-text-dim':      '#7878a0',
        '--ui-text-bright':   '#ffffff',
        '--ui-accent':        '#4a6aff',
        '--ui-accent-dim':    '#3a4a99',
        '--ui-success':       '#44dd88',
        '--ui-warning':       '#ffaa33',
        '--ui-danger':        '#ff4466',
        '--ui-disabled':      '#3a3a50',
    },
    midnight: {
        '--ui-bg':            '#060614',
        '--ui-surface':       '#0c0c24',
        '--ui-surface-alt':   '#141436',
        '--ui-surface-hover': '#1c1c4a',
        '--ui-border':        '#20204a',
        '--ui-border-focus':  '#6644ff',
        '--ui-text':          '#c8c8e8',
        '--ui-text-dim':      '#6868a0',
        '--ui-text-bright':   '#eeeeff',
        '--ui-accent':        '#6644ff',
        '--ui-accent-dim':    '#4433aa',
        '--ui-success':       '#33ccaa',
        '--ui-warning':       '#ff9944',
        '--ui-danger':        '#ff3366',
        '--ui-disabled':      '#2a2a44',
    },
    ember: {
        '--ui-bg':            '#140a0a',
        '--ui-surface':       '#1e1010',
        '--ui-surface-alt':   '#2a1818',
        '--ui-surface-hover': '#3a2020',
        '--ui-border':        '#4a2a2a',
        '--ui-border-focus':  '#ff6633',
        '--ui-text':          '#e8d0c8',
        '--ui-text-dim':      '#a07868',
        '--ui-text-bright':   '#fff0e8',
        '--ui-accent':        '#ff6633',
        '--ui-accent-dim':    '#aa4422',
        '--ui-success':       '#88cc44',
        '--ui-warning':       '#ffcc22',
        '--ui-danger':        '#ff3344',
        '--ui-disabled':      '#3a2828',
    },
    forest: {
        '--ui-bg':            '#060e0a',
        '--ui-surface':       '#0c1a12',
        '--ui-surface-alt':   '#14241a',
        '--ui-surface-hover': '#1c3024',
        '--ui-border':        '#2a4a34',
        '--ui-border-focus':  '#44bb66',
        '--ui-text':          '#c8e0d0',
        '--ui-text-dim':      '#689878',
        '--ui-text-bright':   '#e8fff0',
        '--ui-accent':        '#44bb66',
        '--ui-accent-dim':    '#338844',
        '--ui-success':       '#66ee88',
        '--ui-warning':       '#ddaa33',
        '--ui-danger':        '#ee4444',
        '--ui-disabled':      '#283830',
    },
    ice: {
        '--ui-bg':            '#0a0e14',
        '--ui-surface':       '#101824',
        '--ui-surface-alt':   '#182030',
        '--ui-surface-hover': '#1e2c40',
        '--ui-border':        '#2a4060',
        '--ui-border-focus':  '#44aaee',
        '--ui-text':          '#c8dce8',
        '--ui-text-dim':      '#6890aa',
        '--ui-text-bright':   '#e8f4ff',
        '--ui-accent':        '#44aaee',
        '--ui-accent-dim':    '#2278aa',
        '--ui-success':       '#44ddaa',
        '--ui-warning':       '#eeaa33',
        '--ui-danger':        '#ee4466',
        '--ui-disabled':      '#283848',
    },
};

// ============================================================================
// Utility
// ============================================================================

function emit(el, name, detail) {
    el.dispatchEvent(new CustomEvent(name, { bubbles: true, detail: detail || {} }));
}

function tmpl(id) {
    var t = document.getElementById(id);
    if (!t) {
        console.log('[game-ui] template not found: ' + id);
        return null;
    }
    return t.content.cloneNode(true);
}

// ============================================================================
// Focus Manager — arrow-key navigation within a container
// ============================================================================

function FocusManager(host, direction) {
    this.host = host;
    this.direction = direction || 'vertical';
    this.index = -1;
}

FocusManager.prototype.getItems = function() {
    var nodes = this.host.children || [];
    var result = [];
    for (var i = 0; i < nodes.length; i++) {
        var n = nodes[i];
        if (n.hasAttribute && !n.hasAttribute('disabled') && !n.hasAttribute('hidden')
            && n.tagName !== 'UI-SEPARATOR') {
            result.push(n);
        }
    }
    return result;
};

FocusManager.prototype.focusIndex = function(idx) {
    var list = this.getItems();
    if (list.length === 0) return;
    if (idx < 0) idx = 0;
    if (idx >= list.length) idx = list.length - 1;

    if (this.index >= 0 && this.index < list.length) {
        var prev = list[this.index];
        if (prev && prev._uiBlur) prev._uiBlur();
    }

    this.index = idx;
    var item = list[idx];
    if (item && item._uiFocus) item._uiFocus();
    emit(this.host, 'ui-navigate', { index: idx, item: item });
};

FocusManager.prototype.focusFirst = function() { this.focusIndex(0); };
FocusManager.prototype.focusLast = function() { this.focusIndex(this.getItems().length - 1); };

FocusManager.prototype.moveNext = function() {
    var len = this.getItems().length;
    if (len === 0) return;
    this.focusIndex((this.index + 1) % len);
};

FocusManager.prototype.movePrev = function() {
    var len = this.getItems().length;
    if (len === 0) return;
    this.focusIndex((this.index - 1 + len) % len);
};

FocusManager.prototype.handleKey = function(e) {
    var nextKey = this.direction === 'vertical' ? 'ArrowDown' : 'ArrowRight';
    var prevKey = this.direction === 'vertical' ? 'ArrowUp'   : 'ArrowLeft';

    if (e.key === nextKey) { e.preventDefault(); e.stopPropagation(); this.moveNext(); return true; }
    if (e.key === prevKey) { e.preventDefault(); e.stopPropagation(); this.movePrev(); return true; }
    if (e.key === 'Home') { e.preventDefault(); this.focusFirst(); return true; }
    if (e.key === 'End')  { e.preventDefault(); this.focusLast(); return true; }
    return false;
};

FocusManager.prototype.activate = function(e) {
    var list = this.getItems();
    if (this.index >= 0 && this.index < list.length) {
        var item = list[this.index];
        if (item && item._uiActivate) { item._uiActivate(e); return true; }
    }
    return false;
};

FocusManager.prototype.reset = function() {
    var list = this.getItems();
    for (var i = 0; i < list.length; i++) {
        if (list[i]._uiBlur) list[i]._uiBlur();
    }
    this.index = -1;
};

// ============================================================================
// Public API
// ============================================================================

window.GameUI = {
    THEMES: THEMES,
    emit: emit,
    tmpl: tmpl,
    FocusManager: FocusManager,
};

})();
