window.views = window.views || {};

// Define custom elements once
if (!customElements.get('fancy-card')) {

    class FancyCard extends HTMLElement {
        constructor() {
            super();
            var shadow = this.attachShadow({ mode: 'open' });
            var theme = this.getAttribute('theme') || 'red';
            var accent = theme === 'blue' ? '#4488ee' : '#e94560';
            shadow.innerHTML =
                '<style>' +
                ':host { display: block; border: 2px solid ' + accent + '; padding: 16px; margin: 8px 0; background: #0f3460; }' +
                '.title { color: ' + accent + '; font-size: 20px; font-weight: bold; margin-bottom: 8px; }' +
                '.body { color: #ccc; font-size: 14px; }' +
                '.badge { background: ' + accent + '; color: white; padding: 2px 8px; font-size: 11px; display: inline-block; margin-top: 8px; }' +
                '</style>' +
                '<div class="title">Shadow DOM Component</div>' +
                '<div class="body">Styles are fully encapsulated inside the shadow DOM.</div>' +
                '<div class="badge">Theme: ' + theme + '</div>';
        }
    }
    customElements.define('fancy-card', FancyCard);

    class InfoPanel extends HTMLElement {
        constructor() {
            super();
            var shadow = this.attachShadow({ mode: 'open' });
            shadow.innerHTML =
                '<style>' +
                ':host { display: block; border: 1px solid #444; background: #0a0a1a; margin: 8px 0; }' +
                '.header { background: #0f3460; padding: 10px 14px; }' +
                '.icon { color: #44dd88; font-size: 16px; margin-right: 8px; }' +
                '.title { color: #e94560; font-size: 16px; font-weight: bold; }' +
                '.content { padding: 12px 14px; color: #aaa; font-size: 13px; font-family: monospace; }' +
                '</style>' +
                '<div class="header"><span class="icon"><slot name="icon"></slot></span><span class="title"><slot name="title">Default</slot></span></div>' +
                '<div class="content"><slot></slot></div>';
        }
    }
    customElements.define('info-panel', InfoPanel);

    class EncapDemo extends HTMLElement {
        constructor() {
            super();
            var shadow = this.attachShadow({ mode: 'open' });
            shadow.innerHTML =
                '<style>' +
                ':host { display: block; border: 1px solid #44dd88; padding: 12px; margin: 8px 0; background: #0a0a2a; }' +
                '.title { color: #44dd88; font-size: 16px; }' +
                '.info { color: #888; font-size: 13px; margin-top: 6px; }' +
                '</style>' +
                '<div class="title">Styled by SHADOW stylesheet (green)</div>' +
                '<div class="info">The .title class is green here but red outside. No conflict.</div>';
        }
    }
    customElements.define('encap-demo', EncapDemo);

    class EventDemo extends HTMLElement {
        constructor() {
            super();
            var shadow = this.attachShadow({ mode: 'open' });
            shadow.innerHTML =
                '<style>' +
                ':host { display: block; border: 1px solid #ee8844; padding: 16px; background: #1a1a3e; }' +
                'button { background: #ee8844; color: white; border: none; padding: 8px 20px; font-size: 14px; cursor: pointer; margin: 4px; }' +
                '.label { color: #ee8844; font-size: 14px; margin-bottom: 8px; }' +
                '</style>' +
                '<div class="label">Click these buttons inside shadow DOM:</div>' +
                '<button id="shadow-btn-a">Button A</button>' +
                '<button id="shadow-btn-b">Button B</button>';

            shadow.querySelector('#shadow-btn-a').addEventListener('click', function(e) {
                wcLogEvent('INSIDE shadow: target.id = ' + (e.target ? e.target.id : '?'));
            });
            shadow.querySelector('#shadow-btn-b').addEventListener('click', function(e) {
                wcLogEvent('INSIDE shadow: target.id = ' + (e.target ? e.target.id : '?'));
            });
        }
    }
    customElements.define('event-demo', EventDemo);

    class InnerBadge extends HTMLElement {
        constructor() {
            super();
            var shadow = this.attachShadow({ mode: 'open' });
            var label = this.getAttribute('label') || 'Badge';
            var color = this.getAttribute('color') || '#e94560';
            shadow.innerHTML =
                '<style>:host { display: inline-block; } .badge { background: ' + color + '; color: white; padding: 4px 12px; font-size: 12px; font-weight: bold; margin: 2px; }</style>' +
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
                '.title { color: #4488ee; font-size: 18px; font-weight: bold; margin-bottom: 10px; }' +
                '.desc { color: #aaa; font-size: 13px; margin-bottom: 10px; }' +
                '</style>' +
                '<div class="title">Outer Component</div>' +
                '<div class="desc">Contains inner-badge components in its shadow DOM:</div>' +
                '<inner-badge label="HTML" color="#e94560"></inner-badge>' +
                '<inner-badge label="CSS" color="#4488ee"></inner-badge>' +
                '<inner-badge label="JS" color="#44dd88"></inner-badge>';
        }
    }
    customElements.define('outer-comp', OuterComp);
}

function wcLogEvent(msg) {
    var el = document.getElementById('wc-event-log');
    if (el) el.textContent += msg + '\n';
}

window.views.components = {
    init: function(el) {
        // Event retargeting
        var wrapper = el.querySelector('#wc-event-wrapper');
        if (wrapper) {
            wrapper.addEventListener('click', function(e) {
                var id = e.target ? (e.target.id || e.target.tagName) : '?';
                wcLogEvent('OUTSIDE shadow: target = ' + id);
            });
        }

        // Dynamic shadow DOM
        var dynamicComponents = [];
        var count = 0;

        el.querySelector('#wc-btn-create').addEventListener('click', function() {
            count++;
            var div = document.createElement('div');
            var shadow = div.attachShadow({ mode: 'open' });
            shadow.innerHTML =
                '<style>:host { display: block; border: 1px solid #aa55cc; padding: 12px; margin: 6px 0; background: #1a102a; } .t { color: #aa55cc; font-weight: bold; } .c { color: #aaa; margin-top: 4px; font-size: 13px; }</style>' +
                '<div class="t">Dynamic Component #' + count + '</div><div class="c">Created at runtime with attachShadow()</div>';
            el.querySelector('#wc-dynamic-container').appendChild(div);
            dynamicComponents.push(div);
        });

        el.querySelector('#wc-btn-update').addEventListener('click', function() {
            if (dynamicComponents.length === 0) return;
            var last = dynamicComponents[dynamicComponents.length - 1];
            var shadow = last.shadowRoot;
            if (!shadow) return;
            shadow.innerHTML =
                '<style>:host { display: block; border: 2px solid #dddd44; padding: 12px; margin: 6px 0; background: #1a1a10; } .t { color: #dddd44; font-weight: bold; } .c { color: #ddd; margin-top: 4px; font-size: 13px; }</style>' +
                '<div class="t">Updated!</div><div class="c">Shadow DOM content replaced dynamically.</div>';
        });

        el.querySelector('#wc-btn-reslot').addEventListener('click', function() {
            var div = document.createElement('div');
            var shadow = div.attachShadow({ mode: 'open' });
            shadow.innerHTML =
                '<style>:host { display: block; border: 1px solid #44dd88; padding: 12px; margin: 6px 0; background: #0a1a10; } .label { color: #44dd88; font-size: 12px; margin-bottom: 4px; }</style>' +
                '<div class="label">Slotted content:</div><slot></slot>';
            var child = document.createElement('div');
            child.textContent = 'Light DOM child (slotted)';
            child.style.color = '#aaddaa';
            child.style.padding = '4px';
            div.appendChild(child);
            el.querySelector('#wc-dynamic-container').appendChild(div);
            dynamicComponents.push(div);
        });
    }
};
