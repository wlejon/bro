// ui-layout — Screen, Panel, Tabs, Tab, Separator components.

(function() {
'use strict';

// <ui-screen> — Full-screen container
class UIScreen extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-screen'));
    }

    activate() {
        var nav = this.querySelector('ui-menu, ui-tabs');
        if (nav && nav.activate) nav.activate();
    }

    deactivate() {
        var nav = this.querySelector('ui-menu, ui-tabs');
        if (nav && nav.deactivate) nav.deactivate();
    }
}

// <ui-panel> — Styled container with optional header
class UIPanel extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-panel'));
    }

    connectedCallback() {
        var h = this.getAttribute('header');
        if (h) this.shadowRoot.querySelector('.header').textContent = h;
    }

    static get observedAttributes() { return ['header']; }

    attributeChangedCallback(name, oldVal, newVal) {
        if (name === 'header') {
            var el = this.shadowRoot.querySelector('.header');
            if (el) el.textContent = newVal || '';
        }
    }
}

// <ui-tabs> — Tabbed container
class UITabs extends HTMLElement {
    constructor() {
        super();
        this._tabIndex = 0;
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-tabs'));
    }

    connectedCallback() {
        var self = this;
        // In standard browsers, children may not be parsed yet when
        // connectedCallback fires. Listen for slotchange to detect when
        // light DOM children are distributed, then rebuild the tab bar.
        var slot = this.shadowRoot.querySelector('slot');
        if (slot) {
            slot.addEventListener('slotchange', function() {
                self._buildTabBar();
                self._updateContentHeight();
            });
        }
        // Also build immediately for engines (like bro) that parse the full
        // DOM before firing connectedCallback.
        this._buildTabBar();
        this._updateContentHeight();
    }

    _updateContentHeight() {
        var self = this;
        // Defer to allow layout to complete
        setTimeout(function() {
            // Set overflow on the ui-menu inside the active tab.
            // This must be a light DOM element so the engine's scroll handler
            // finds it when walking up parentElement() from the target.
            var tab = self._activeTab();
            if (!tab) return;
            var menu = tab.querySelector('ui-menu');
            if (!menu) return;
            var hostRect = self.getBoundingClientRect();
            var bar = self.shadowRoot.querySelector('.tab-bar');
            var barH = bar ? bar.getBoundingClientRect().height : 40;
            var vh = window.innerHeight || 768;
            var available = vh - hostRect.y - barH - 8 - 50;
            if (available > 100) {
                menu.style.maxHeight = Math.floor(available) + 'px';
                menu.style.overflowY = 'auto';
            }
        }, 0);
    }

    _buildTabBar() {
        var bar = this.shadowRoot.querySelector('.tab-bar');
        bar.innerHTML = '';
        var tabs = this._getTabs();
        var self = this;
        for (var i = 0; i < tabs.length; i++) {
            (function(idx) {
                var btn = document.createElement('div');
                btn.className = 'tab-btn' + (idx === self._tabIndex ? ' active' : '');
                btn.textContent = tabs[idx].getAttribute('label') || 'Tab ' + (idx + 1);
                btn.addEventListener('click', function() { self.switchTab(idx); });
                bar.appendChild(btn);
            })(i);
        }
        this._updateVisibility();
    }

    _getTabs() {
        var result = [];
        var kids = this.children || [];
        for (var i = 0; i < kids.length; i++) {
            if (kids[i].tagName === 'UI-TAB') result.push(kids[i]);
        }
        return result;
    }

    _activeTab() {
        return this._getTabs()[this._tabIndex] || null;
    }

    switchTab(idx) {
        var tabs = this._getTabs();
        if (idx < 0 || idx >= tabs.length) return;

        var current = tabs[this._tabIndex];
        if (current) {
            var menu = current.querySelector('ui-menu');
            if (menu) {
                if (menu.deactivate) menu.deactivate();
                // Clear overflow from previous tab's menu
                menu.style.maxHeight = '';
                menu.style.overflowY = '';
            }
        }

        this._tabIndex = idx;
        this._updateTabBar();
        this._updateVisibility();

        var newTab = tabs[idx];
        if (newTab) {
            var menu = newTab.querySelector('ui-menu');
            if (menu && menu.activate) menu.activate();
        }
        GameUI.emit(this, 'ui-change', { name: 'tab', value: idx });
        this._updateContentHeight();
    }

    nextTab() { this.switchTab((this._tabIndex + 1) % this._getTabs().length); }
    prevTab() {
        var len = this._getTabs().length;
        this.switchTab((this._tabIndex - 1 + len) % len);
    }

    _updateTabBar() {
        var btns = this.shadowRoot.querySelectorAll('.tab-btn');
        for (var i = 0; i < btns.length; i++) {
            btns[i].className = (i === this._tabIndex) ? 'tab-btn active' : 'tab-btn';
        }
    }

    _updateVisibility() {
        var tabs = this._getTabs();
        for (var i = 0; i < tabs.length; i++) {
            tabs[i].style.display = (i === this._tabIndex) ? 'block' : 'none';
        }
    }

    activate() {
        var tab = this._activeTab();
        if (tab) {
            var menu = tab.querySelector('ui-menu');
            if (menu && menu.activate) menu.activate();
        }
    }

    deactivate() {
        var tab = this._activeTab();
        if (tab) {
            var menu = tab.querySelector('ui-menu');
            if (menu && menu.deactivate) menu.deactivate();
        }
    }
}

// <ui-tab> — Tab panel
class UITab extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-tab'));
    }
}

// <ui-separator> — Visual divider
class UISeparator extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.shadowRoot.appendChild(GameUI.tmpl('tmpl-ui-separator'));
    }

    connectedCallback() {
        var label = this.getAttribute('label');
        if (label) this.shadowRoot.querySelector('.text').textContent = label;
    }

    static get observedAttributes() { return ['label']; }

    attributeChangedCallback(name, oldVal, newVal) {
        if (name === 'label') {
            var el = this.shadowRoot.querySelector('.text');
            if (el) el.textContent = newVal || '';
        }
    }
}

customElements.define('ui-screen',    UIScreen);
customElements.define('ui-panel',     UIPanel);
customElements.define('ui-tabs',      UITabs);
customElements.define('ui-tab',       UITab);
customElements.define('ui-separator', UISeparator);

})();
