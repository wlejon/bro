// navigator.getBattery() — BatteryManager-shaped snapshot over
// SDL_GetPowerInfo. Headless always resolves the pinned desktop no-battery
// shape {charging: true, chargingTime: 0, dischargingTime: Infinity,
// level: 1}, so exact values are assertable.

assert(typeof navigator.getBattery === 'function', 'navigator.getBattery exists');

let bat = null;
const p = navigator.getBattery();
assert(p instanceof Promise, 'getBattery returns a Promise');
p.then(b => { bat = b; });
flush();
advanceTime(10);
flush();

assert(bat !== null, 'battery promise resolved');
assert(bat.charging === true, 'headless: charging true');
assert(bat.chargingTime === 0, 'headless: chargingTime 0');
assert(bat.dischargingTime === Infinity, 'headless: dischargingTime Infinity');
assert(bat.level === 1, 'headless: level 1');
assert(typeof bat.addEventListener === 'function', 'inert addEventListener present');
assert(typeof bat.removeEventListener === 'function', 'inert removeEventListener present');
assert(bat.onlevelchange === null, 'onlevelchange handler slot null');

// Snapshot-on-call: each call yields a fresh object.
let bat2 = null;
navigator.getBattery().then(b => { bat2 = b; });
flush();
advanceTime(10);
flush();
assert(bat2 !== null && bat2 !== bat, 'each call returns a fresh snapshot');

console.log('getBattery OK');
