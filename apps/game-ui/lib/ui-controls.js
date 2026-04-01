// ui-controls — Slider, Toggle, Select, Keybind, TextEntry, Button components.

(function() {
'use strict';

var emit = GameUI.emit;

// ---- <ui-button> ----

class UIButton extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-button'));

        var self = this;
        this._uiFocus = function() {
            self.shadowRoot.querySelector('.btn').classList.add('focused');
        };
        this._uiBlur = function() {
            self.shadowRoot.querySelector('.btn').classList.remove('focused');
        };
        this._uiActivate = function() {
            if (!self.hasAttribute('disabled'))
                emit(self, 'ui-action', { action: self.getAttribute('action') || '', item: self });
        };
        this.addEventListener('click', function() {
            if (!self.hasAttribute('disabled')) self._uiActivate();
        });
    }
}

// ---- <ui-slider> ----

class UISlider extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-slider'));

        var self = this;
        this._uiFocus = function() { self.shadowRoot.querySelector('.row').classList.add('focused'); };
        this._uiBlur  = function() { self.shadowRoot.querySelector('.row').classList.remove('focused'); };
        this._uiActivate = function() {};
    }

    connectedCallback() {
        this._updateLabel();
        this._updateDisplay();
        var self = this;
        this.addEventListener('keydown', function(e) {
            if (e.key === 'ArrowRight') { e.preventDefault(); e.stopPropagation(); self._adjust(self._step()); }
            else if (e.key === 'ArrowLeft') { e.preventDefault(); e.stopPropagation(); self._adjust(-self._step()); }
        });
    }

    _step() { return parseFloat(this.getAttribute('step') || '1'); }
    _min()  { return parseFloat(this.getAttribute('min') || '0'); }
    _max()  { return parseFloat(this.getAttribute('max') || '100'); }
    _val()  { return parseFloat(this.getAttribute('value') || '50'); }

    _adjust(delta) {
        var v = this._val() + delta;
        var min = this._min(), max = this._max(), step = this._step();
        if (v < min) v = min;
        if (v > max) v = max;
        v = Math.round(v / step) * step;
        this.setAttribute('value', String(v));
        this._updateDisplay();
        emit(this, 'ui-change', { name: this.getAttribute('name') || '', value: v, item: this });
    }

    _updateLabel() {
        var el = this.shadowRoot.querySelector('.label');
        if (el) el.textContent = this.getAttribute('label') || '';
    }

    _updateDisplay() {
        var v = this._val(), min = this._min(), max = this._max();
        var pct = ((v - min) / (max - min)) * 100;
        var fill = this.shadowRoot.querySelector('.fill');
        if (fill) fill.style.width = pct + '%';
        var val = this.shadowRoot.querySelector('.val');
        if (val) val.textContent = v + (this.getAttribute('suffix') || '');
    }

    static get observedAttributes() { return ['value', 'label', 'min', 'max']; }
    attributeChangedCallback(name) {
        if (name === 'label') this._updateLabel();
        else this._updateDisplay();
    }
}

// ---- <ui-toggle> ----

class UIToggle extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-toggle'));

        var self = this;
        this._uiFocus = function() { self.shadowRoot.querySelector('.row').classList.add('focused'); };
        this._uiBlur  = function() { self.shadowRoot.querySelector('.row').classList.remove('focused'); };
        this._uiActivate = function() { self.toggle(); };
        this.addEventListener('click', function() { self.toggle(); });
    }

    get checked() { return this.hasAttribute('checked'); }
    set checked(v) {
        if (v) this.setAttribute('checked', '');
        else this.removeAttribute('checked');
        this._updateDisplay();
    }

    toggle() {
        this.checked = !this.checked;
        emit(this, 'ui-change', { name: this.getAttribute('name') || '', value: this.checked, item: this });
    }

    connectedCallback() {
        this._updateLabel();
        this._updateDisplay();
        var self = this;
        this.addEventListener('keydown', function(e) {
            if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); e.stopPropagation(); self.toggle(); }
        });
    }

    _updateLabel() {
        var el = this.shadowRoot.querySelector('.label');
        if (el) el.textContent = this.getAttribute('label') || '';
    }

    _updateDisplay() {
        var sw = this.shadowRoot.querySelector('.switch');
        var st = this.shadowRoot.querySelector('.state');
        if (this.checked) {
            if (sw) sw.classList.add('on');
            if (st) st.textContent = this.getAttribute('on-label') || 'ON';
        } else {
            if (sw) sw.classList.remove('on');
            if (st) st.textContent = this.getAttribute('off-label') || 'OFF';
        }
    }

    static get observedAttributes() { return ['checked', 'label']; }
    attributeChangedCallback(name) {
        if (name === 'label') this._updateLabel();
        else this._updateDisplay();
    }
}

// ---- <ui-select> ----

class UISelect extends HTMLElement {
    constructor() {
        super();
        this._options = [];
        this._index = 0;
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-select'));

        var self = this;
        this._uiFocus = function() { self.shadowRoot.querySelector('.row').classList.add('focused'); };
        this._uiBlur  = function() { self.shadowRoot.querySelector('.row').classList.remove('focused'); };
        this._uiActivate = function() {};
    }

    connectedCallback() {
        this._parseOptions();
        this._updateLabel();
        this._updateDisplay();
        var self = this;
        this.addEventListener('keydown', function(e) {
            if (e.key === 'ArrowRight') { e.preventDefault(); e.stopPropagation(); self.next(); }
            else if (e.key === 'ArrowLeft') { e.preventDefault(); e.stopPropagation(); self.prev(); }
        });
    }

    _parseOptions() {
        var opts = this.getAttribute('options');
        if (opts) {
            this._options = opts.split(',');
            for (var i = 0; i < this._options.length; i++) this._options[i] = this._options[i].trim();
        }
        var val = this.getAttribute('value');
        if (val) {
            for (var i = 0; i < this._options.length; i++) {
                if (this._options[i] === val) { this._index = i; break; }
            }
        }
    }

    get value() { return this._options[this._index] || ''; }
    set value(v) {
        for (var i = 0; i < this._options.length; i++) {
            if (this._options[i] === v) { this._index = i; this._updateDisplay(); return; }
        }
    }

    next() {
        if (this._options.length === 0) return;
        this._index = (this._index + 1) % this._options.length;
        this._updateDisplay();
        this._emitChange();
    }

    prev() {
        if (this._options.length === 0) return;
        this._index = (this._index - 1 + this._options.length) % this._options.length;
        this._updateDisplay();
        this._emitChange();
    }

    _emitChange() {
        this.setAttribute('value', this.value);
        emit(this, 'ui-change', { name: this.getAttribute('name') || '', value: this.value, item: this });
    }

    _updateLabel() {
        var el = this.shadowRoot.querySelector('.label');
        if (el) el.textContent = this.getAttribute('label') || '';
    }

    _updateDisplay() {
        var el = this.shadowRoot.querySelector('.val');
        if (el) el.textContent = this.value;
    }

    static get observedAttributes() { return ['label', 'options', 'value']; }
    attributeChangedCallback(name) {
        if (name === 'label') this._updateLabel();
        if (name === 'options' || name === 'value') { this._parseOptions(); this._updateDisplay(); }
    }
}

// ---- <ui-keybind> ----

class UIKeybind extends HTMLElement {
    constructor() {
        super();
        this._listening = false;
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-keybind'));

        var self = this;
        this._uiFocus = function() { self.shadowRoot.querySelector('.row').classList.add('focused'); };
        this._uiBlur  = function() { self.shadowRoot.querySelector('.row').classList.remove('focused'); self._stopListening(); };
        this._uiActivate = function() { self._startListening(); };
    }

    connectedCallback() {
        this._updateLabel();
        this._updateDisplay();
        var self = this;
        this.addEventListener('keydown', function(e) {
            if (self._listening) {
                e.preventDefault();
                e.stopPropagation();
                if (e.key === 'Escape') { self._stopListening(); }
                else {
                    self.setAttribute('key', e.key);
                    self._stopListening();
                    self._updateDisplay();
                    emit(self, 'ui-change', { name: self.getAttribute('name') || '', value: e.key, item: self });
                }
            } else if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                e.stopPropagation();
                self._startListening();
            }
        });
    }

    _startListening() {
        this._listening = true;
        var k = this.shadowRoot.querySelector('.key');
        if (k) { k.classList.add('listening'); k.textContent = '...press a key...'; }
    }

    _stopListening() {
        this._listening = false;
        var k = this.shadowRoot.querySelector('.key');
        if (k) k.classList.remove('listening');
        this._updateDisplay();
    }

    _formatKey(key) {
        if (!key) return '---';
        var names = {
            'ArrowUp': 'Up', 'ArrowDown': 'Down', 'ArrowLeft': 'Left', 'ArrowRight': 'Right',
            ' ': 'Space', 'Control': 'Ctrl', 'Shift': 'Shift', 'Alt': 'Alt',
        };
        return names[key] || key.toUpperCase();
    }

    _updateLabel() {
        var el = this.shadowRoot.querySelector('.label');
        if (el) el.textContent = this.getAttribute('label') || '';
    }

    _updateDisplay() {
        if (!this._listening) {
            var el = this.shadowRoot.querySelector('.key');
            if (el) el.textContent = this._formatKey(this.getAttribute('key'));
        }
    }

    static get observedAttributes() { return ['label', 'key']; }
    attributeChangedCallback(name) {
        if (name === 'label') this._updateLabel();
        if (name === 'key') this._updateDisplay();
    }
}

// ---- <ui-text-entry> ----

class UITextEntry extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-text-entry'));

        var self = this;
        this._uiFocus = function() {
            self.shadowRoot.querySelector('.row').classList.add('focused');
            self.shadowRoot.querySelector('input').focus();
        };
        this._uiBlur = function() { self.shadowRoot.querySelector('.row').classList.remove('focused'); };
        this._uiActivate = function() { self.shadowRoot.querySelector('input').focus(); };
    }

    get value() { return this.shadowRoot.querySelector('input').value; }
    set value(v) { this.shadowRoot.querySelector('input').value = v; }

    connectedCallback() {
        this._updateLabel();
        var input = this.shadowRoot.querySelector('input');
        var ph = this.getAttribute('placeholder');
        if (ph) input.setAttribute('placeholder', ph);
        var val = this.getAttribute('value');
        if (val) input.value = val;
        var maxlen = this.getAttribute('maxlength');
        if (maxlen) input.setAttribute('maxlength', maxlen);

        var self = this;
        input.addEventListener('input', function() {
            emit(self, 'ui-change', { name: self.getAttribute('name') || '', value: input.value, item: self });
        });
        input.addEventListener('keydown', function(e) {
            if (e.key === 'ArrowLeft' || e.key === 'ArrowRight' ||
                e.key === 'ArrowUp' || e.key === 'ArrowDown') {
                e.stopPropagation();
            }
        });
    }

    _updateLabel() {
        var el = this.shadowRoot.querySelector('.label');
        if (el) el.textContent = this.getAttribute('label') || '';
    }

    static get observedAttributes() { return ['label']; }
    attributeChangedCallback(name) {
        if (name === 'label') this._updateLabel();
    }
}

customElements.define('ui-button',     UIButton);
customElements.define('ui-slider',     UISlider);
customElements.define('ui-toggle',     UIToggle);
customElements.define('ui-select',     UISelect);
customElements.define('ui-keybind',    UIKeybind);
customElements.define('ui-text-entry', UITextEntry);

})();
