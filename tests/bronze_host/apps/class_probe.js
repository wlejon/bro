// HOST CLASSES: what `Image` proves now that a handle can be born on a
// prototype (embed::makeHandle's 4-argument form).
//
// Every object this layer hands the program used to be a bare cell with its
// methods closed over per instance, which cost two things: a copy of every
// method for every instance, and a false answer to `instanceof`. bro's own
// source said so in as many words — host_image.cpp named `img instanceof
// Image` as observably false, and events_probe.js still names the same limit
// as the reason `new CustomEvent(...)` does not exist.
//
// This file pins the three claims that must hold for `Image` to be a real
// class rather than a decorated object: the chain is right, the methods are
// SHARED rather than copied, and a method reached THROUGH the prototype still
// unwraps its receiver (the handle brand survives, which is the part that had
// a landmine in it).
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
