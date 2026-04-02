// Shadow DOM headless tests
advanceTime(100);

// Test 1: Basic shadow DOM - fancy-card should exist and have shadow content
assert(document.querySelector('fancy-card') !== null, 'fancy-card exists');
assert(document.querySelector('fancy-card').shadowRoot !== null, 'fancy-card has shadowRoot');
assert(document.querySelector('fancy-card').shadowRoot.mode === 'open', 'shadowRoot mode is open');

// Test 2: Shadow root innerHTML contains card-title
assert(document.querySelector('fancy-card').shadowRoot.innerHTML.indexOf('card-title') >= 0,
       'shadowRoot contains card-title');

// Test 3: Shadow root querySelector
assert(document.querySelector('fancy-card').shadowRoot.querySelector('.card-title') !== null,
       'querySelector finds .card-title in shadow');
var titleText = document.querySelector('fancy-card').shadowRoot.querySelector('.card-title').textContent;
assert(titleText.length > 0, 'card-title has text content');

// Test 4: Style encapsulation - outer .title should NOT be affected by shadow .title
assert(document.querySelector('#outer-title') !== null, 'outer-title exists');

// Test 5: encap-demo has its own shadow root
assert(document.querySelector('encap-demo').shadowRoot !== null, 'encap-demo has shadowRoot');
assert(document.querySelector('encap-demo').shadowRoot.querySelector('.title') !== null,
       'encap-demo shadow has .title');

// Test 6: Slot projection - info-panel
assert(document.querySelector('info-panel') !== null, 'info-panel exists');
assert(document.querySelector('info-panel').shadowRoot !== null, 'info-panel has shadowRoot');
assert(document.querySelector('info-panel').shadowRoot.querySelector('.header') !== null,
       'info-panel shadow has .header');

// Test 7: Event retargeting - event-demo
assert(document.querySelector('event-demo').shadowRoot !== null, 'event-demo has shadowRoot');
assert(document.querySelector('event-demo').shadowRoot.querySelector('#shadow-btn-a') !== null,
       'event-demo has #shadow-btn-a');

// Test 8: Dynamic shadow DOM
var d = document.createElement('div');
d.attachShadow({mode: 'open'});
assert(d.shadowRoot !== null, 'dynamic attachShadow works');

var d2 = document.createElement('div');
var sr = d2.attachShadow({mode: 'open'});
sr.innerHTML = '<div id="t">hello</div>';
assert(sr.querySelector('#t').textContent === 'hello', 'dynamic shadow innerHTML works');

// Test 9: Nested components
assert(document.querySelector('outer-comp') !== null, 'outer-comp exists');
assert(document.querySelector('outer-comp').shadowRoot !== null, 'outer-comp has shadowRoot');

// Test 10: ShadowRoot host getter
assert(document.querySelector('fancy-card').shadowRoot.host === document.querySelector('fancy-card'),
       'shadowRoot.host returns host element');

// Test 11: Closed mode
var d3 = document.createElement('div');
d3.attachShadow({mode: 'closed'});
assert(d3.shadowRoot === null, 'closed shadow root not accessible via .shadowRoot');

// Test 12: Cannot attach twice
var d4 = document.createElement('div');
d4.attachShadow({mode: 'open'});
var caught = false;
try { d4.attachShadow({mode: 'open'}); } catch(e) { caught = true; }
assert(caught, 'double attachShadow throws');
