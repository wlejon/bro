// Two platform globals the QuickJS engine used to provide for free and the
// bronze port does not: a constructible TouchEvent family, and DOMException's
// legacy code table.
//
// A harness that synthesises input writes
// `new TouchEvent('touchstart', {touches: [new Touch({...})]})`; the port
// installed the class as a brand only, so that read as "not constructible".
// And `e.code === DOMException.NOT_FOUND_ERR` compared undefined to undefined
// — true for every exception there is.

// --- Touch / TouchList / TouchEvent are constructible ---------------------
assert(typeof Touch === 'function', 'Touch is a constructor');
assert(typeof TouchList === 'function', 'TouchList is a constructor');
assert(typeof TouchEvent === 'function', 'TouchEvent is a constructor');

const root = document.getElementById('root');
root.innerHTML = '<div id="pad" style="width:100px;height:100px;"></div>';
flush();
const pad = document.getElementById('pad');

const t = new Touch({ identifier: 3, target: pad, clientX: 10, clientY: 20,
                      pageX: 10, pageY: 20, force: 0.5 });
assert(t instanceof Touch, 'new Touch() makes a Touch');
assert(t.identifier === 3, 'Touch carries its identifier');
assert(t.target === pad, 'Touch carries its target');
assert(t.clientX === 10 && t.clientY === 20, 'Touch carries its coordinates');
assert(t.force === 0.5, 'Touch carries its force');
assert(t.radiusX === 0, 'a member the initializer omitted takes its default');

const list = new TouchList([t]);
assert(list.length === 1, 'TouchList reports its length');
assert(list[0] === t, 'TouchList is indexable');
assert(list.item(0) === t, 'TouchList.item works');
assert(list.item(5) === null, 'TouchList.item past the end is null');

const bare = new TouchEvent('touchstart');
assert(bare instanceof TouchEvent, 'new TouchEvent(type) works with no initializer');
assert(bare.type === 'touchstart', 'and carries the type');
assert(bare.touches.length === 0, 'with an empty touches list rather than undefined');
assert(bare.targetTouches.length === 0, 'and an empty targetTouches');
assert(bare.changedTouches.length === 0, 'and an empty changedTouches');
assert(bare.bubbles === false, 'bubbles defaults to false');

const full = new TouchEvent('touchmove', {
    bubbles: true, cancelable: true,
    touches: [t], targetTouches: [t], changedTouches: [t],
    shiftKey: true,
});
assert(full.type === 'touchmove', 'the full form carries the type');
assert(full.bubbles === true && full.cancelable === true, 'and the flags');
assert(full.touches.length === 1 && full.touches[0] === t,
       'and the touches it was given');
assert(full.changedTouches.item(0) === t, 'the lists are real TouchLists');
assert(full.shiftKey === true, 'and the modifier keys');

// It dispatches like any other event.
let got = null;
pad.addEventListener('touchmove', function (e) { got = e; });
pad.dispatchEvent(full);
assert(got === full, 'a constructed TouchEvent dispatches and arrives intact');
assert(got.touches[0] === t, 'with its touch list intact');

// GestureEvent too.
const g = new GestureEvent('gesturechange', { scale: 2, rotation: 45 });
assert(g.type === 'gesturechange', 'GestureEvent is constructible');
assert(g.scale === 2 && g.rotation === 45, 'and carries its members');
assert(new GestureEvent('x').scale === 1, 'scale defaults to 1');

// --- DOMException legacy codes -------------------------------------------
assert(typeof DOMException === 'function', 'DOMException is a constructor');
assert(DOMException.INDEX_SIZE_ERR === 1, 'INDEX_SIZE_ERR is 1');
assert(DOMException.DOMSTRING_SIZE_ERR === 2, 'DOMSTRING_SIZE_ERR is 2');
assert(DOMException.HIERARCHY_REQUEST_ERR === 3, 'HIERARCHY_REQUEST_ERR is 3');
assert(DOMException.WRONG_DOCUMENT_ERR === 4, 'WRONG_DOCUMENT_ERR is 4');
assert(DOMException.INVALID_CHARACTER_ERR === 5, 'INVALID_CHARACTER_ERR is 5');
assert(DOMException.NO_DATA_ALLOWED_ERR === 6, 'NO_DATA_ALLOWED_ERR is 6');
assert(DOMException.NO_MODIFICATION_ALLOWED_ERR === 7, 'NO_MODIFICATION_ALLOWED_ERR is 7');
assert(DOMException.NOT_FOUND_ERR === 8, 'NOT_FOUND_ERR is 8');
assert(DOMException.NOT_SUPPORTED_ERR === 9, 'NOT_SUPPORTED_ERR is 9');
assert(DOMException.INUSE_ATTRIBUTE_ERR === 10, 'INUSE_ATTRIBUTE_ERR is 10');
assert(DOMException.INVALID_STATE_ERR === 11, 'INVALID_STATE_ERR is 11');
assert(DOMException.SYNTAX_ERR === 12, 'SYNTAX_ERR is 12');
assert(DOMException.INVALID_MODIFICATION_ERR === 13, 'INVALID_MODIFICATION_ERR is 13');
assert(DOMException.NAMESPACE_ERR === 14, 'NAMESPACE_ERR is 14');
assert(DOMException.INVALID_ACCESS_ERR === 15, 'INVALID_ACCESS_ERR is 15');
assert(DOMException.VALIDATION_ERR === 16, 'VALIDATION_ERR is 16');
assert(DOMException.TYPE_MISMATCH_ERR === 17, 'TYPE_MISMATCH_ERR is 17');
assert(DOMException.SECURITY_ERR === 18, 'SECURITY_ERR is 18');
assert(DOMException.NETWORK_ERR === 19, 'NETWORK_ERR is 19');
assert(DOMException.ABORT_ERR === 20, 'ABORT_ERR is 20');
assert(DOMException.URL_MISMATCH_ERR === 21, 'URL_MISMATCH_ERR is 21');
assert(DOMException.QUOTA_EXCEEDED_ERR === 22, 'QUOTA_EXCEEDED_ERR is 22');
assert(DOMException.TIMEOUT_ERR === 23, 'TIMEOUT_ERR is 23');
assert(DOMException.INVALID_NODE_TYPE_ERR === 24, 'INVALID_NODE_TYPE_ERR is 24');
assert(DOMException.DATA_CLONE_ERR === 25, 'DATA_CLONE_ERR is 25');

// The constants are on the prototype too, which is where instance-side
// `e.NOT_FOUND_ERR` reads them from.
const ex = new DOMException('gone', 'NotFoundError');
assert(ex.NOT_FOUND_ERR === 8, 'the constants are reachable from an instance');
assert(ex.name === 'NotFoundError', 'the exception keeps its name');
assert(ex.code === 8, 'and `code` is derived from that name: ' + ex.code);
assert(new DOMException('x', 'AbortError').code === 20, 'AbortError codes as 20');
assert(new DOMException('x', 'NoSuchThingError').code === 0,
       'a name with no legacy code reports 0');

root.innerHTML = '';
