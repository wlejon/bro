// ui-menu — Menu and MenuItem components.

(function() {
'use strict';

var emit = GameUI.emit;
var FocusManager = GameUI.FocusManager;

// <ui-menu> — Vertical navigable list
class UIMenu extends HTMLElement {
    constructor() {
        super();
        this._fm = new FocusManager(this, 'vertical');
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-menu'));
    }

    connectedCallback() {
        var self = this;
        this.addEventListener('keydown', function(e) {
            if (self._fm.handleKey(e)) return;
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                e.stopPropagation();
                self._fm.activate(e);
            }
        });
        this.addEventListener('click', function(e) {
            var target = e.target;
            while (target && target !== self) {
                if (target._uiFocus) {
                    var items = self._fm.getItems();
                    for (var i = 0; i < items.length; i++) {
                        if (items[i] === target) {
                            self._fm.focusIndex(i);
                            self._fm.activate(e);
                            break;
                        }
                    }
                    return;
                }
                target = target.parentElement;
            }
        });
        this.addEventListener('mouseenter', function(e) {
            var target = e.target;
            while (target && target !== self) {
                if (target._uiFocus) {
                    var items = self._fm.getItems();
                    for (var i = 0; i < items.length; i++) {
                        if (items[i] === target) {
                            self._fm.focusIndex(i);
                            break;
                        }
                    }
                    return;
                }
                target = target.parentElement;
            }
        }, true);
    }

    activate() { this._fm.focusFirst(); }
    deactivate() { this._fm.reset(); }
}

// <ui-menu-item> — Selectable item in a menu
class UIMenuItem extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-menu-item'));

        var self = this;
        this._uiFocus = function() {
            self.shadowRoot.querySelector('.item').classList.add('focused');
        };
        this._uiBlur = function() {
            self.shadowRoot.querySelector('.item').classList.remove('focused');
        };
        this._uiActivate = function() {
            if (self.hasAttribute('disabled')) return;
            emit(self, 'ui-action', {
                action: self.getAttribute('action') || '',
                item: self,
            });
        };
    }

    connectedCallback() {
        this._updateValue();
        this._updateArrow();
    }

    _updateValue() {
        var el = this.shadowRoot.querySelector('.value');
        if (el) el.textContent = this.getAttribute('value') || '';
    }

    _updateArrow() {
        var el = this.shadowRoot.querySelector('.arrow');
        if (el) el.textContent = this.hasAttribute('nav') ? '>' : '';
    }

    static get observedAttributes() { return ['value', 'nav']; }

    attributeChangedCallback(name) {
        if (name === 'value') this._updateValue();
        if (name === 'nav') this._updateArrow();
    }
}

customElements.define('ui-menu',      UIMenu);
customElements.define('ui-menu-item', UIMenuItem);

})();
