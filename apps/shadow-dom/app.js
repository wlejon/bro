// =========================================================================
// 1. Basic Shadow DOM: <fancy-card>
// =========================================================================

class FancyCard extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        var theme = this.getAttribute('theme') || 'red';
        var accent = theme === 'blue' ? '#4488ee' : '#e94560';

        shadow.innerHTML =
            '<style>' +
            ':host { display: block; border: 2px solid ' + accent + '; padding: 16px; margin: 8px 0; background: #0f3460; }' +
            '.card-title { color: ' + accent + '; font-size: 20px; font-weight: bold; margin-bottom: 8px; }' +
            '.card-body { color: #ccc; font-size: 14px; }' +
            '.badge { background: ' + accent + '; color: white; padding: 2px 8px; font-size: 11px; display: inline-block; margin-top: 8px; }' +
            '</style>' +
            '<div class="card-title">Shadow DOM Component</div>' +
            '<div class="card-body">This content is inside the shadow DOM. Its styles are fully encapsulated.</div>' +
            '<div class="badge">Theme: ' + theme + '</div>';
    }
}
customElements.define('fancy-card', FancyCard);

// =========================================================================
// 2. Slot Projection: <info-panel>
// =========================================================================

class InfoPanel extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        shadow.innerHTML =
            '<style>' +
            ':host { display: block; border: 1px solid #444; background: #0a0a1a; padding: 0; margin: 8px 0; }' +
            '.header { display: block; background: #0f3460; padding: 10px 14px; }' +
            '.header-icon { color: #44dd88; font-size: 16px; margin-right: 8px; }' +
            '.header-title { color: #e94560; font-size: 16px; font-weight: bold; }' +
            '.content { padding: 12px 14px; color: #aaa; font-size: 13px; font-family: monospace; }' +
            '</style>' +
            '<div class="header">' +
            '  <span class="header-icon"><slot name="icon"></slot></span>' +
            '  <span class="header-title"><slot name="title">Default Title</slot></span>' +
            '</div>' +
            '<div class="content"><slot></slot></div>';
    }
}
customElements.define('info-panel', InfoPanel);

// =========================================================================
// 3. Style Encapsulation: <encap-demo>
// =========================================================================

class EncapDemo extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        // This .title class is DIFFERENT from the outer document's .title
        shadow.innerHTML =
            '<style>' +
            ':host { display: block; border: 1px solid #44dd88; padding: 12px; margin: 8px 0; background: #0a0a2a; }' +
            '.title { color: #44dd88; font-size: 16px; background: none; padding: 0; }' +
            '.info { color: #888; font-size: 13px; margin-top: 6px; }' +
            '</style>' +
            '<div class="title">I am styled by the SHADOW stylesheet (green)</div>' +
            '<div class="info">The .title class inside shadow DOM is green, while the outer .title is red. They do not conflict.</div>';
    }
}
customElements.define('encap-demo', EncapDemo);

// =========================================================================
// 4. Event Retargeting: <event-demo>
// =========================================================================

class EventDemo extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        shadow.innerHTML =
            '<style>' +
            ':host { display: block; border: 1px solid #ee8844; padding: 16px; background: #1a1a3e; }' +
            '.inner-btn { background: #ee8844; color: white; border: none; padding: 8px 20px; font-size: 14px; cursor: pointer; margin: 4px; }' +
            '.inner-label { color: #ee8844; font-size: 14px; margin-bottom: 8px; }' +
            '.inner-box { border: 1px dashed #555; padding: 8px; margin-top: 8px; }' +
            '</style>' +
            '<div class="inner-label">Click the buttons inside shadow DOM:</div>' +
            '<button class="inner-btn" id="shadow-btn-a">Button A</button>' +
            '<button class="inner-btn" id="shadow-btn-b">Button B</button>' +
            '<div class="inner-box">This is a div inside shadow DOM</div>';

        // Internal listeners (see real target)
        var self = this;
        shadow.querySelector('#shadow-btn-a').addEventListener('click', function(e) {
            logEvent('INSIDE shadow: target.id = ' + (e.target ? e.target.id || e.target.tagName : '?'));
        });
        shadow.querySelector('#shadow-btn-b').addEventListener('click', function(e) {
            logEvent('INSIDE shadow: target.id = ' + (e.target ? e.target.id || e.target.tagName : '?'));
        });
    }
}
customElements.define('event-demo', EventDemo);

var eventLog = null;
function logEvent(msg) {
    if (!eventLog) eventLog = document.getElementById('event-log');
    if (!eventLog) return;
    eventLog.textContent += msg + '\n';
}

// Outer listener: should see retargeted target (the host element)
document.getElementById('event-host-wrapper').addEventListener('click', function(e) {
    var tgt = e.target;
    var id = tgt ? (tgt.id || tgt.tagName) : '?';
    logEvent('OUTSIDE shadow: target = ' + id + ' (should be host element)');
});

// =========================================================================
// 5. Dynamic Shadow DOM
// =========================================================================

var dynamicCount = 0;
var dynamicComponents = [];

document.getElementById('btn-create').addEventListener('click', function() {
    dynamicCount++;
    var el = document.createElement('div');
    el.id = 'dynamic-' + dynamicCount;

    var shadow = el.attachShadow({ mode: 'open' });
    shadow.innerHTML =
        '<style>' +
        ':host { display: block; border: 1px solid #aa55cc; padding: 12px; margin: 6px 0; background: #1a102a; }' +
        '.dyn-title { color: #aa55cc; font-weight: bold; }' +
        '.dyn-content { color: #aaa; margin-top: 4px; font-size: 13px; }' +
        '</style>' +
        '<div class="dyn-title">Dynamic Component #' + dynamicCount + '</div>' +
        '<div class="dyn-content">Created at runtime with attachShadow()</div>';

    document.getElementById('dynamic-container').appendChild(el);
    dynamicComponents.push(el);
});

document.getElementById('btn-update').addEventListener('click', function() {
    if (dynamicComponents.length === 0) return;
    var last = dynamicComponents[dynamicComponents.length - 1];
    var shadow = last.shadowRoot;
    if (!shadow) return;

    shadow.innerHTML =
        '<style>' +
        ':host { display: block; border: 2px solid #dddd44; padding: 12px; margin: 6px 0; background: #1a1a10; }' +
        '.dyn-title { color: #dddd44; font-weight: bold; }' +
        '.dyn-content { color: #ddd; margin-top: 4px; font-size: 13px; }' +
        '</style>' +
        '<div class="dyn-title">Updated Component!</div>' +
        '<div class="dyn-content">Shadow DOM content replaced dynamically.</div>';
});

document.getElementById('btn-reslot').addEventListener('click', function() {
    // Add a slotted component dynamically
    dynamicCount++;
    var el = document.createElement('div');

    var shadow = el.attachShadow({ mode: 'open' });
    shadow.innerHTML =
        '<style>' +
        ':host { display: block; border: 1px solid #44dd88; padding: 12px; margin: 6px 0; background: #0a1a10; }' +
        '.slot-label { color: #44dd88; font-size: 12px; margin-bottom: 4px; }' +
        '</style>' +
        '<div class="slot-label">Slotted content below:</div>' +
        '<slot></slot>';

    // Add light DOM children that will be slotted
    var child1 = document.createElement('div');
    child1.textContent = 'Light DOM child #1 (slotted)';
    child1.style.color = '#aaddaa';
    child1.style.padding = '4px';

    var child2 = document.createElement('div');
    child2.textContent = 'Light DOM child #2 (slotted)';
    child2.style.color = '#aaddaa';
    child2.style.padding = '4px';

    el.appendChild(child1);
    el.appendChild(child2);

    document.getElementById('dynamic-container').appendChild(el);
    dynamicComponents.push(el);
});

// =========================================================================
// 6. Nested Components: <outer-comp> containing <inner-badge>
// =========================================================================

class InnerBadge extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        var label = this.getAttribute('label') || 'Badge';
        var color = this.getAttribute('color') || '#e94560';
        shadow.innerHTML =
            '<style>' +
            ':host { display: inline-block; }' +
            '.badge { background: ' + color + '; color: white; padding: 4px 12px; font-size: 12px; font-weight: bold; margin: 2px; }' +
            '</style>' +
            '<span class="badge">' + label + '</span>';
    }
}
customElements.define('inner-badge', InnerBadge);

class OuterComp extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        shadow.innerHTML =
            '<style>' +
            ':host { display: block; border: 2px solid #4488ee; padding: 16px; background: #0a1030; margin: 8px 0; }' +
            '.outer-title { color: #4488ee; font-size: 18px; font-weight: bold; margin-bottom: 10px; }' +
            '.outer-desc { color: #aaa; font-size: 13px; margin-bottom: 10px; }' +
            '.badges { margin-top: 8px; }' +
            '</style>' +
            '<div class="outer-title">Outer Component</div>' +
            '<div class="outer-desc">This component contains inner-badge components in its shadow DOM:</div>' +
            '<div class="badges">' +
            '  <inner-badge label="HTML" color="#e94560"></inner-badge>' +
            '  <inner-badge label="CSS" color="#4488ee"></inner-badge>' +
            '  <inner-badge label="JS" color="#44dd88"></inner-badge>' +
            '</div>';
    }
}
customElements.define('outer-comp', OuterComp);

console.log('Shadow DOM demo loaded!');
