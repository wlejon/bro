// Worker postMessage keeps object identity within one message, as
// structuredClone does: an object reached twice arrives as one object, and a
// cycle can be sent. Each occurrence used to arrive as its own copy, and a
// cycle recursed until the depth limit threw.

const w = new Worker('../workers/worker_object_identity.js');
let got = null;
w.onmessage = (e) => { got = e.data; };

function waitReply() {
    const deadline = Date.now() + 15000;
    while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    assert(got !== null, 'worker replied');
}

{
    const a = { tag: 'a' };
    const msg = { a, b: a, list: [a, a], child: {} };
    msg.self = msg;
    msg.child.parent = msg;
    msg.map = new Map([[a, a]]);
    msg.set = new Set([a]);
    const arr = [];
    arr.push(arr);
    msg.arr = arr;
    msg.when = new Date(5);
    msg.again = msg.when;
    msg.v = new Float32Array(2);
    msg.w = msg.v;
    w.postMessage(msg);
    waitReply();
    const r = got.report;
    const why = ' (' + JSON.stringify(r) + ')';
    assert(r.twice === true, 'an object under two keys arrives as one object' + why);
    assert(r.inArray === true, 'and inside an array too' + why);
    assert(r.selfCycle === true, 'a direct cycle resolves to the message' + why);
    assert(r.nested === true, 'a cycle through a child resolves' + why);
    assert(r.mapKeyIsValue === true, 'a Map key and value that are one object stay one' + why);
    assert(r.setHoldsIt === true, 'a Set holds the same object' + why);
    assert(r.arrayCycle === true, 'an array containing itself' + why);
    assert(r.dateTwice === true, 'a Date reached twice' + why);
    assert(r.viewTwice === true, 'a typed array reached twice' + why);
}

// The reply (worker to page) keeps identity and cycles too.
assert(got.x === got.y[0] && got.x.n === 7, 'the reply shares one object');
assert(got.me === got, 'the reply\'s cycle resolves');

// Many distinct objects still arrive distinct and in order.
{
    got = null;
    const many = [];
    for (let i = 0; i < 2000; i++) many.push({ i });
    const msg = { a: many[0], b: many[1], list: many, child: {} };
    msg.self = msg;
    msg.child.parent = msg;
    w.postMessage(msg);
    waitReply();
    assert(got.report.twice === false, 'distinct objects stay distinct');
    assert(got.report.selfCycle === true, 'the cycle still resolves after 2000 objects');
}

w.terminate();
console.log('test_postmessage_object_identity: OK');
