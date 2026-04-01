// ui-root — Theme provider, screen stack manager, global key handling.

(function() {
'use strict';

var emit = GameUI.emit;

class UIRoot extends HTMLElement {
    constructor() {
        super();
        this._screenStack = [];
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-root'));
    }

    connectedCallback() {
        this.applyTheme(this.getAttribute('theme') || 'dark');
        var self = this;
        this.addEventListener('keydown', function(e) {
            if (e.key === 'Escape') {
                e.preventDefault();
                e.stopPropagation();
                self.back();
            }
        });
    }

    applyTheme(name) {
        var theme = GameUI.THEMES[name] || GameUI.THEMES.dark;
        var keys = Object.keys(theme);
        for (var i = 0; i < keys.length; i++) {
            this.style.setProperty(keys[i], theme[keys[i]]);
        }
        this.setAttribute('theme', name);
    }

    get activeScreen() {
        var s = this._screenStack;
        return s.length > 0 ? s[s.length - 1] : null;
    }

    pushScreen(id) {
        var current = this.activeScreen;
        if (current) current.removeAttribute('active');

        var screen = this.querySelector('#' + id);
        if (!screen) return;

        this._screenStack.push(screen);
        screen.setAttribute('active', '');
        if (screen.activate) screen.activate();
        emit(this, 'ui-screen', { action: 'push', screen: id });
    }

    popScreen() {
        if (this._screenStack.length <= 1) return;
        var current = this._screenStack.pop();
        current.removeAttribute('active');
        if (current.deactivate) current.deactivate();

        var prev = this.activeScreen;
        if (prev) {
            prev.setAttribute('active', '');
            if (prev.activate) prev.activate();
        }
        emit(this, 'ui-screen', { action: 'pop', screen: current.id });
    }

    replaceScreen(id) {
        var current = this.activeScreen;
        if (current) {
            current.removeAttribute('active');
            if (current.deactivate) current.deactivate();
            this._screenStack.pop();
        }
        var screen = this.querySelector('#' + id);
        if (!screen) return;
        this._screenStack.push(screen);
        screen.setAttribute('active', '');
        if (screen.activate) screen.activate();
        emit(this, 'ui-screen', { action: 'replace', screen: id });
    }

    back() {
        var active = this.activeScreen;
        if (active) {
            var dialog = active.querySelector('ui-dialog[open]');
            if (dialog) { dialog.close('cancel'); return; }
        }
        if (this._screenStack.length > 1) this.popScreen();
        else emit(this, 'ui-back', {});
    }

    toast(message, type, duration) {
        var t = document.createElement('ui-toast');
        t.setAttribute('message', message || '');
        if (type) t.setAttribute('type', type);
        if (duration) t.setAttribute('duration', String(duration));
        this.appendChild(t);
        t.show();
    }
}

customElements.define('ui-root', UIRoot);

})();
