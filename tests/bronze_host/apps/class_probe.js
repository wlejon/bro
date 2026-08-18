// HOST CLASSES: every family this layer has converted to a real class, now
// that a handle can be born on a prototype (embed::makeHandle's 4-argument
// form, wrapped as HostClass in src/bronze_host/host_class.cpp).
//
// Every object this layer hands the program used to be a bare cell with its
// methods closed over per instance, which cost two things: a copy of every
// method for every instance, and a false answer to `instanceof`. bro's own
// source said so in as many words — host_image.cpp named `img instanceof
// Image` as observably false.
//
// Three claims are pinned per class, because they are the ones that could
// silently be wrong: the chain is right, the methods are SHARED rather than
// copied, and a method reached THROUGH the prototype still unwraps its
// receiver. That last one has a landmine under it — the payload and the
// handle brand live in internal slots, and bronze had to move the brand out of
// the shape root to keep it reachable from an inherited method.
//
// `Image` is worked in the most detail, including a real decode end to end;
// the rest share `pinClass` plus whatever is specific to them.
//
// Output rule, as everywhere here: `APP <name>=<value>`, one per line, and the
// expectation beside this file is written by hand from what must be true, not
// recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

// ---------------------------------------------------------------------------
// The constructor and its prototype
// ---------------------------------------------------------------------------

// On the web `Image` IS `HTMLImageElement` — one constructor under two names.
say('ctor.aliased', Image === HTMLImageElement);
say('ctor.protoIsObject', typeof Image.prototype === 'object' && Image.prototype !== null);
// The minted prototype is an ordinary object, so an instance still reaches
// Object.prototype and `img.hasOwnProperty(...)` resolves.
say('ctor.protoChainsToObject', Object.getPrototypeOf(Image.prototype) === Object.prototype);

const img = new Image();

say('img.instanceofImage', img instanceof Image);
say('img.instanceofHTMLImageElement', img instanceof HTMLImageElement);
say('img.protoIsCtorProto', Object.getPrototypeOf(img) === Image.prototype);

// ---------------------------------------------------------------------------
// Shared, not copied
// ---------------------------------------------------------------------------

// Reachable, but not OWN: the whole point of the prototype.
say('img.methodReachable', typeof img.addEventListener === 'function');
say('img.methodNotOwn', !Object.prototype.hasOwnProperty.call(img, 'addEventListener'));
say('img.srcReachable', 'src' in img);
say('img.srcNotOwn', !Object.prototype.hasOwnProperty.call(img, 'src'));

// One copy per CLASS. Two instances reading the same function object is the
// observable form of "not a closure per instance".
const other = new Image();
say('img.methodShared', img.addEventListener === other.addEventListener);
say('img.instancesDistinct', img !== other);

// Own properties are STATE and nothing else, in registration order.
say('img.ownKeys', Object.keys(img).sort().join(','));

// ---------------------------------------------------------------------------
// A method reached through the prototype still finds its receiver
// ---------------------------------------------------------------------------

// This is the claim with a real failure mode behind it: the payload is in an
// internal slot and the brand used to live in the shape root, so a wrapper
// whose prototype was not the bare handle shape could have inherited working
// methods that unwrapped to nothing. A getter answering its default rather
// than throwing is exactly what that would look like, so read a value only the
// payload can supply.
say('img.completeInitially', img.complete === false);
say('img.widthInitially', img.width === 0);

// createElement takes the same path, so it is an instance too.
const created = document.createElement('img');
say('created.instanceofImage', created instanceof Image);
say('created.methodShared', created.addEventListener === img.addEventListener);

// ---------------------------------------------------------------------------
// The other converted families
// ---------------------------------------------------------------------------

// Each of these was a bare cell with per-instance closures and an `instanceof`
// that answered false. The check is the same three claims every time, so it is
// written once and applied.

function pinClass(label, instance, ctor) {
    say(label + '.instanceof', instance instanceof ctor);
    say(label + '.protoIsCtorProto', Object.getPrototypeOf(instance) === ctor.prototype);
}

const blob = new Blob(['hello'], { type: 'text/plain' });
pinClass('blob', blob, Blob);
// Methods live on the prototype, so two blobs read the same function object.
say('blob.methodShared', blob.slice === new Blob([]).slice);
say('blob.methodWorks', blob.size === 5 && blob.type === 'text/plain');
// A detached method still finds its receiver through the prototype, which the
// old per-instance closure form got right by accident and this form gets right
// on purpose.
const detachedSlice = Blob.prototype.slice;
say('blob.detachedMethodWorks', detachedSlice.call(blob, 0, 2).size === 2);

// `file instanceof Blob` is true on the web: File extends Blob.
const file = new File(['abc'], 'note.txt', { type: 'text/plain' });
pinClass('file', file, File);
say('file.isABlob', file instanceof Blob);
say('file.inheritsBlobMethod', typeof file.slice === 'function');
say('file.name', file.name);

const reader = new FileReader();
pinClass('reader', reader, FileReader);
// The readyState constants sit on the prototype AND the constructor, as on
// the web.
say('reader.constOnInstance', reader.DONE === 2);
say('reader.constOnCtor', FileReader.DONE === 2);

const xhr = new XMLHttpRequest();
pinClass('xhr', xhr, XMLHttpRequest);
say('xhr.constOnCtor', XMLHttpRequest.DONE === 4);
say('xhr.accessorWorks', xhr.readyState === 0);

const headers = new Headers();
pinClass('headers', headers, Headers);
headers.set('content-type', 'text/plain');
say('headers.methodWorks', headers.get('content-type') === 'text/plain');

const response = new Response();
pinClass('response', response, Response);
say('response.state', response.ok === true && response.status === 200);
say('response.headersAreHeaders', response.headers instanceof Headers);

const controller = new AbortController();
pinClass('controller', controller, AbortController);
say('controller.signalIsSignal', controller.signal instanceof AbortSignal);
// AbortSignal was a namespace object; it is a class with statics now.
say('signal.staticAbort', AbortSignal.abort() instanceof AbortSignal);
say('signal.notConstructible', (function () {
    try { new AbortSignal(); return false; } catch (e) { return true; }
})());
controller.abort();
say('controller.abortWorks', controller.signal.aborted === true);

const mo = new MutationObserver(function () {});
pinClass('mo', mo, MutationObserver);
say('mo.methodShared', mo.observe === new MutationObserver(function () {}).observe);

const ro = new ResizeObserver(function () {});
pinClass('ro', ro, ResizeObserver);

// WebSocket is not opened here — a connection is not this probe's business —
// but the class shape is readable without one.
say('ws.ctorConstants', WebSocket.OPEN === 1 && WebSocket.CLOSED === 3);
say('ws.protoConstants', WebSocket.prototype.OPEN === 1);

// ---------------------------------------------------------------------------
// Element, which is the one that actually costs something
// ---------------------------------------------------------------------------

// An element carried its own copy of fifty-eight members. A thousand-element
// UI allocated fifty-eight thousand function objects to say the same
// fifty-eight things; now it allocates fifty-eight, plus three per element
// (the event-target trio, which needs a per-element source).

const div = document.createElement('div');
const div2 = document.createElement('div');

pinClass('div', div, HTMLElement);
say('div.isElement', div instanceof Element);
say('div.ElementIsHTMLElement', Element === HTMLElement);

// The whole surface is shared between two elements...
say('div.methodShared', div.setAttribute === div2.setAttribute);
say('div.accessorShared',
    Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'id') !== undefined);
say('div.methodNotOwn', !Object.prototype.hasOwnProperty.call(div, 'setAttribute'));

// ...and the only own properties left are identity plus the event-target trio.
say('div.ownKeys', Object.keys(div).sort().join(','));

// A shared accessor still writes to the RIGHT element: this is the claim that
// would break if a member read a captured pointer instead of its receiver.
div.id = 'first';
div2.id = 'second';
say('div.accessorPerInstance', div.id === 'first' && div2.id === 'second');

div.setAttribute('data-x', '1');
say('div.methodPerInstance',
    div.getAttribute('data-x') === '1' && div2.getAttribute('data-x') === null);

// The per-instance sub-objects still resolve, and are still per instance:
// their receiver is the sub-object, not the element, so they keep closing over
// their own state rather than reading a receiver.
div.style.color = 'red';
say('div.styleIsPerInstance', div.style.color === 'red' && div2.style.color === '');
div.dataset.role = 'button';
say('div.datasetIsPerInstance',
    div.dataset.role === 'button' && div2.dataset.role === undefined);
div.classList.add('on');
say('div.classListIsPerInstance',
    div.classList.contains('on') && !div2.classList.contains('on'));

// The tree half comes from the same prototype and still finds its receiver.
div.appendChild(div2);
say('div.treeWorks', div.children.length === 1 && div2.parentElement === div);

// A canvas is an element that overrides a few members as OWN properties, so
// the prototype supplies the rest and the overrides shadow it.
const canvas = document.createElement('canvas');
say('canvas.isElement', canvas instanceof HTMLElement);
// getAttribute comes from the prototype — the same function object every
// element reads.
say('canvas.inheritsElementMethod', canvas.getAttribute === div.getAttribute);
// setAttribute is one it DOES override, because writing width or height has to
// resize the drawing buffer. The own property shadows the shared one.
say('canvas.ownOverrideWins',
    Object.prototype.hasOwnProperty.call(canvas, 'width') &&
    Object.prototype.hasOwnProperty.call(canvas, 'setAttribute') &&
    canvas.setAttribute !== div.setAttribute);

// A text node is NOT an element: it is built on the bare handle shape, and
// giving it Element's prototype would be a lie the tree walkers would believe.
const text = document.createTextNode('hi');
say('text.isNotElement', !(text instanceof HTMLElement));
say('text.treeStillWorks', typeof text.appendChild === 'function');

// ---------------------------------------------------------------------------
// Web Audio, which is where the class HIERARCHY earns its keep
// ---------------------------------------------------------------------------

// Nine interfaces, and every node kind shares one AudioNode base. Before the
// conversion each node carried its own copy of connect/disconnect/
// numberOfInputs/numberOfOutputs/channelCount and answered false to every
// `instanceof` a routing helper might ask.

const ac = new AudioContext();
pinClass('ac', ac, AudioContext);
say('ac.aliased', AudioContext === webkitAudioContext);
say('ac.windowSameObject', window.AudioContext === AudioContext);

const gain = ac.createGain();
const osc = ac.createOscillator();

pinClass('gain', gain, GainNode);
pinClass('osc', osc, OscillatorNode);

// The hierarchy: every node IS an AudioNode, and the base surface is one
// shared copy rather than one per node.
say('gain.isAudioNode', gain instanceof AudioNode);
say('osc.isAudioNode', osc instanceof AudioNode);
say('node.baseShared', gain.connect === osc.connect);
say('node.baseIsOnBase',
    Object.prototype.hasOwnProperty.call(AudioNode.prototype, 'connect'));
// ...but a GainNode is not an OscillatorNode.
say('gain.isNotOsc', !(gain instanceof OscillatorNode));

// numberOfInputs reads the RECEIVER's node kind, which is the one member of
// the shared base whose answer differs per node: a source has no input.
say('node.inputsPerKind', gain.numberOfInputs === 1 && osc.numberOfInputs === 0);
say('node.outputsPerKind', gain.numberOfOutputs === 1 &&
                           ac.destination.numberOfOutputs === 0);

// AudioParams are their own class, and each node owns its own.
pinClass('param', gain.gain, AudioParam);
say('param.perInstance', gain.gain !== osc.frequency);
say('param.methodShared', gain.gain.setValueAtTime === osc.frequency.setValueAtTime);
gain.gain.value = 0.5;
say('param.valuePerInstance', gain.gain.value === 0.5 && osc.frequency.value === 440);

// AudioBuffer is data rather than a node, and it IS constructible.
const buffer = new AudioBuffer({ length: 8, numberOfChannels: 2, sampleRate: 44100 });
pinClass('buffer', buffer, AudioBuffer);
say('buffer.isNotAudioNode', !(buffer instanceof AudioNode));
say('buffer.state', buffer.length === 8 && buffer.numberOfChannels === 2);

// ---------------------------------------------------------------------------
// End to end: a load through the inherited accessor
// ---------------------------------------------------------------------------

// A 1x1 PNG inline, so the decode needs no file and no network.
const PNG_1X1 =
    'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAA' +
    'DUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';

let loadFired = false;
let loadThisWasImg = false;

// addEventListener is the inherited one; the handler's receiver must still be
// this instance.
img.addEventListener('load', function () {
    loadFired = true;
    loadThisWasImg = this === img;
});

// `src` is the inherited ACCESSOR — the setter has to unwrap `this` to reach
// the payload it decodes into.
img.src = PNG_1X1;

// Reported from a timer so the ordering is fixed whether the decode settles
// synchronously or on the next turn.
setTimeout(function () {
    say('load.fired', loadFired);
    say('load.receiverWasInstance', loadThisWasImg);
    say('load.complete', img.complete === true);
    say('load.naturalWidth', img.naturalWidth);
    say('load.srcRoundTrips', img.src === PNG_1X1);
    // The other instance is untouched: state is per instance even though the
    // accessor that writes it is shared.
    say('load.otherUntouched', other.complete === false && other.naturalWidth === 0);
}, 0);
