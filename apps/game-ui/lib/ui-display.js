// ui-display — Progress bar and Badge components.

(function() {
'use strict';

// <ui-progress> — Progress bar
class UIProgress extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-progress'));
    }

    connectedCallback() { this._update(); }

    _update() {
        var v = parseFloat(this.getAttribute('value') || '0');
        var max = parseFloat(this.getAttribute('max') || '100');
        var pct = Math.min(100, Math.max(0, (v / max) * 100));
        var fill = this.shadowRoot.querySelector('.fill');
        if (fill) fill.style.width = pct + '%';
        var pctEl = this.shadowRoot.querySelector('.pct');
        if (pctEl) pctEl.textContent = Math.round(pct) + '%';
        var label = this.shadowRoot.querySelector('.label');
        if (label) label.textContent = this.getAttribute('label') || '';
    }

    static get observedAttributes() { return ['value', 'max', 'label']; }
    attributeChangedCallback() { this._update(); }
}

// <ui-badge> — Small colored label
class UIBadge extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-badge'));
    }
}

customElements.define('ui-progress', UIProgress);
customElements.define('ui-badge',    UIBadge);

})();
