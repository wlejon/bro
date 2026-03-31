// =========================================================================
// 1. Custom Element: <click-counter>
// =========================================================================

class ClickCounter extends HTMLElement {
    static get observedAttributes() {
        return ['start'];
    }

    constructor() {
        super();
        this._count = 0;
    }

    connectedCallback() {
        var startAttr = this.getAttribute('start');
        if (startAttr !== null) {
            this._count = parseInt(startAttr, 10) || 0;
        }
        this._render();
        // Wire up the button after rendering
        var btn = this.querySelector('.inc-btn');
        if (btn) {
            var self = this;
            btn.addEventListener('click', function() {
                self._count++;
                self._render();
            });
        }
    }

    attributeChangedCallback(name, oldVal, newVal) {
        if (name === 'start' && this.parentElement) {
            this._count = parseInt(newVal, 10) || 0;
            this._render();
        }
    }

    _render() {
        this.innerHTML =
            '<div class="count">' + this._count + '</div>' +
            '<div class="label">clicks</div>' +
            '<button class="inc-btn">+1</button>';
        // Re-attach listener since innerHTML nukes it
        var btn = this.querySelector('.inc-btn');
        if (btn) {
            var self = this;
            btn.addEventListener('click', function() {
                self._count++;
                self._render();
            });
        }
    }
}

customElements.define('click-counter', ClickCounter);

// =========================================================================
// 2. Lifecycle Callbacks: <lifecycle-el>
// =========================================================================

var lifecycleColors = ['#e94560', '#44dd88', '#4488ee', '#ee8844', '#aa55cc', '#dddd44'];
var lifecycleColorIdx = 0;
var lifecycleElements = [];

class LifecycleEl extends HTMLElement {
    static get observedAttributes() {
        return ['color'];
    }

    constructor() {
        super();
    }

    connectedCallback() {
        logLifecycle('connectedCallback - tag: ' + this.tagName + ', color: ' + this.getAttribute('color'));
    }

    disconnectedCallback() {
        logLifecycle('disconnectedCallback - removed element');
    }

    attributeChangedCallback(name, oldVal, newVal) {
        logLifecycle('attributeChangedCallback - ' + name + ': ' + oldVal + ' -> ' + newVal);
        if (name === 'color') {
            this.style.backgroundColor = newVal;
        }
    }
}

customElements.define('lifecycle-el', LifecycleEl);

var logBox = null;

function logLifecycle(msg) {
    if (!logBox) logBox = document.getElementById('lifecycle-log');
    if (!logBox) return;
    var now = new Date();
    var ts = now.getMinutes() + ':' + (now.getSeconds() < 10 ? '0' : '') + now.getSeconds();
    logBox.textContent += '[' + ts + '] ' + msg + '\n';
}

document.getElementById('btn-add').addEventListener('click', function() {
    var el = document.createElement('lifecycle-el');
    var color = lifecycleColors[lifecycleColorIdx % lifecycleColors.length];
    lifecycleColorIdx++;
    el.setAttribute('color', color);
    el.textContent = '#' + lifecycleColorIdx;
    document.getElementById('lifecycle-target').appendChild(el);
    lifecycleElements.push(el);
});

document.getElementById('btn-remove').addEventListener('click', function() {
    if (lifecycleElements.length > 0) {
        var el = lifecycleElements.pop();
        el.parentElement.removeChild(el);
    }
});

document.getElementById('btn-attr').addEventListener('click', function() {
    if (lifecycleElements.length > 0) {
        var el = lifecycleElements[lifecycleElements.length - 1];
        var color = lifecycleColors[lifecycleColorIdx % lifecycleColors.length];
        lifecycleColorIdx++;
        el.setAttribute('color', color);
    }
});

// =========================================================================
// 3. Template stamping
// =========================================================================

var cardData = [
    { title: 'Warrior', body: 'Heavy armor, sword & shield', badge: 'STR' },
    { title: 'Mage', body: 'Arcane spells, glass cannon', badge: 'INT' },
    { title: 'Rogue', body: 'Stealth, daggers, critical hits', badge: 'DEX' },
    { title: 'Healer', body: 'Holy magic, party sustain', badge: 'WIS' },
    { title: 'Ranger', body: 'Bow mastery, animal companion', badge: 'DEX' },
    { title: 'Paladin', body: 'Divine warrior, party buffs', badge: 'CHA' },
];
var cardIdx = 0;

function addCard() {
    var tmpl = document.getElementById('card-template');
    var content = tmpl.content;
    var clone = content.cloneNode(true);

    var data = cardData[cardIdx % cardData.length];
    cardIdx++;

    // Fill in the cloned template
    var children = clone.children;
    if (children.length > 0) {
        var card = children[0];
        var title = card.querySelector('.card-title');
        var body = card.querySelector('.card-body');
        var badge = card.querySelector('.card-badge');
        if (title) title.textContent = data.title;
        if (body) body.textContent = data.body;
        if (badge) badge.textContent = data.badge;
    }

    document.getElementById('card-container').appendChild(clone);
}

// Start with 3 cards
addCard();
addCard();
addCard();

document.getElementById('btn-add-card').addEventListener('click', addCard);

// =========================================================================
// 4. SPA History
// =========================================================================

var histPath = document.getElementById('hist-path');
var histHash = document.getElementById('hist-hash');
var histLen = document.getElementById('hist-len');
var histState = document.getElementById('hist-state');
var histLog = document.getElementById('history-log');
var pushCount = 0;

function updateHistoryDisplay() {
    histPath.textContent = location.pathname;
    histHash.textContent = location.hash || '(none)';
    histLen.textContent = String(history.length);
    histState.textContent = JSON.stringify(history.state);
}

function logHistory(msg) {
    histLog.textContent += msg + '\n';
}

addEventListener('popstate', function(e) {
    logHistory('popstate fired - state: ' + JSON.stringify(e.state));
    updateHistoryDisplay();
});

document.getElementById('btn-push').addEventListener('click', function() {
    pushCount++;
    var path = '/page-' + (pushCount + 1);
    history.pushState({ page: pushCount + 1 }, '', path);
    logHistory('pushState -> ' + path);
    updateHistoryDisplay();
});

document.getElementById('btn-hash').addEventListener('click', function() {
    history.pushState(history.state, '', location.pathname + '#section-' + (pushCount + 1));
    logHistory('pushState hash -> ' + location.hash);
    updateHistoryDisplay();
});

document.getElementById('btn-replace').addEventListener('click', function() {
    history.replaceState({ replaced: true, at: location.pathname }, '', location.pathname);
    logHistory('replaceState at ' + location.pathname);
    updateHistoryDisplay();
});

document.getElementById('btn-back').addEventListener('click', function() {
    logHistory('calling back()');
    history.back();
    // Display updates via popstate handler
});

document.getElementById('btn-forward').addEventListener('click', function() {
    logHistory('calling forward()');
    history.forward();
});

updateHistoryDisplay();

// =========================================================================
// 5. sessionStorage
// =========================================================================

var ssResult = document.getElementById('storage-result');

function logStorage(msg) {
    ssResult.textContent += msg + '\n';
}

document.getElementById('btn-ss-set').addEventListener('click', function() {
    var key = document.getElementById('ss-key').value;
    var val = document.getElementById('ss-val').value;
    sessionStorage.setItem(key, val);
    logStorage('set("' + key + '", "' + val + '") - length: ' + sessionStorage.length);
});

document.getElementById('btn-ss-get').addEventListener('click', function() {
    var key = document.getElementById('ss-key').value;
    var val = sessionStorage.getItem(key);
    logStorage('get("' + key + '") = ' + JSON.stringify(val));
});

document.getElementById('btn-ss-clear').addEventListener('click', function() {
    sessionStorage.clear();
    logStorage('cleared - length: ' + sessionStorage.length);
});

// =========================================================================
// 6. innerText + scrollIntoView
// =========================================================================

document.getElementById('btn-innertext').addEventListener('click', function() {
    var source = document.getElementById('innertext-source');
    var output = document.getElementById('innertext-output');
    var text = source.innerText;
    output.textContent = 'innerText result:\n---\n' + text + '\n---\n(length: ' + text.length + ')';
});

document.getElementById('btn-scroll').addEventListener('click', function() {
    var target = document.getElementById('section-counter');
    target.scrollIntoView();
    // Show feedback since scrollIntoView is a no-op visually
    var output = document.getElementById('innertext-output');
    output.textContent = 'scrollIntoView() called on #section-counter (no-op stub)';
});

// =========================================================================
// 7. Shadow DOM: <shadow-box>
// =========================================================================

class ShadowBox extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        shadow.innerHTML =
            '<style>' +
            ':host { display: block; border: 2px solid #44dd88; padding: 16px; margin: 8px 0; background: #0a1a10; }' +
            '.shadow-title { color: #44dd88; font-size: 18px; font-weight: bold; margin-bottom: 8px; }' +
            '.shadow-content { color: #aaa; font-size: 14px; }' +
            '</style>' +
            '<div class="shadow-title"><slot name="title">Default Title</slot></div>' +
            '<div class="shadow-content"><slot></slot></div>';
    }

    connectedCallback() {
        logShadow('shadow-box connected, shadowRoot mode: ' + this.shadowRoot.mode);
    }
}
customElements.define('shadow-box', ShadowBox);

var shadowLogBox = null;
function logShadow(msg) {
    if (!shadowLogBox) shadowLogBox = document.getElementById('shadow-log');
    if (!shadowLogBox) return;
    shadowLogBox.textContent += msg + '\n';
}

console.log('Web Components test app loaded!');
