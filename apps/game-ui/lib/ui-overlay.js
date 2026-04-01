// ui-overlay — Dialog and Toast components.

(function() {
'use strict';

var emit = GameUI.emit;
var FocusManager = GameUI.FocusManager;

// <ui-dialog> — Modal confirmation dialog
class UIDialog extends HTMLElement {
    constructor() {
        super();
        this._fm = new FocusManager(this, 'horizontal');
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-dialog'));
    }

    connectedCallback() {
        var self = this;
        this.addEventListener('keydown', function(e) {
            if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); self.close('cancel'); return; }
            if (self._fm.handleKey(e)) return;
            if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); e.stopPropagation(); self._fm.activate(e); }
        });
        this.addEventListener('ui-action', function(e) {
            self.close(e.detail.action || 'ok');
        });
    }

    open(title, message) {
        var t = this.shadowRoot.querySelector('.title');
        var m = this.shadowRoot.querySelector('.message');
        if (t) t.textContent = title || this.getAttribute('title') || '';
        if (m) m.textContent = message || this.getAttribute('message') || '';
        this.setAttribute('open', '');
        this._fm.focusFirst();
    }

    close(result) {
        this.removeAttribute('open');
        this._fm.reset();
        emit(this, 'ui-dialog-close', { result: result || 'cancel' });
    }
}

// <ui-toast> — Transient notification
class UIToast extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-toast'));
    }

    show(message) {
        var msg = message || this.getAttribute('message') || '';
        var el = this.shadowRoot.querySelector('.toast');
        if (el) el.textContent = msg;
        this.setAttribute('visible', '');

        var duration = parseInt(this.getAttribute('duration') || '3000', 10);
        var self = this;
        setTimeout(function() {
            self.removeAttribute('visible');
            setTimeout(function() {
                if (self.parentNode) self.parentNode.removeChild(self);
            }, 300);
        }, duration);
    }
}

customElements.define('ui-dialog', UIDialog);
customElements.define('ui-toast',  UIToast);

})();
