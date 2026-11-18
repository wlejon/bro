// Setting a MessagePort's onmessage starts it (no start() call), and a posted
// message is a task: React's scheduler relies on both.
const order = [];
const mc = new MessageChannel();
mc.port1.onmessage = (e) => order.push('msg:' + e.data);
mc.port2.postMessage(1);
Promise.resolve().then(() => order.push('microtask'));
for (let i = 0; i < 5; i++) { flush(); advanceTime(10); flush(); }
assert(order[0] === 'microtask', 'message delivered after microtasks: ' + order.join(','));
assert(order.includes('msg:1'), 'onmessage started the port: ' + order.join(','));
assert(typeof mc.port1.onmessage === 'function', 'onmessage reads back');
mc.port1.onmessage = null;
assert(mc.port1.onmessage === null, 'cleared');
console.log('test_messageport_onmessage: OK');
