// Worker side of test_postmessage_object_identity: report which objects in
// the message arrived as one object, then send a graph with shared parts and
// a cycle back the other way.
self.onmessage = (e) => {
    const d = e.data;
    const report = {
        twice: d.a === d.b,
        inArray: d.list[0] === d.list[1] && d.list[0] === d.a,
        selfCycle: d.self === d,
        nested: d.child.parent === d,
        mapKeyIsValue: d.map ? d.map.get(d.a) === d.a : null,
        setHoldsIt: d.set ? d.set.has(d.a) : null,
        arrayCycle: d.arr ? d.arr[0] === d.arr : null,
        dateTwice: d.when ? d.when === d.again : null,
        viewTwice: d.v ? d.v === d.w : null,
    };
    const shared = { n: 7 };
    const back = { report, x: shared, y: [shared] };
    back.me = back;
    self.postMessage(back);
};
