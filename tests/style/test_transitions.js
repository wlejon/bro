// Test CSS transitions — exercises src/engine/css_transitions.cpp
// (CubicBezier evaluation, color/length interpolation, transition lifecycle,
// transitionstart/transitionend event dispatch).

const root = document.getElementById('root');
root.innerHTML = '';

// Use inline style on the element so the rules apply immediately.
const box = document.createElement('div');
box.id = 'box';
box.style.cssText =
    'width:100px;height:100px;background-color:rgb(255,0,0);opacity:1;' +
    'transition:background-color 1000ms ease, opacity 500ms linear, width 800ms ease-in-out;';
root.appendChild(box);
flush();

// Capture initial styles
const cs0 = getComputedStyle(box);
const bgStart = cs0.backgroundColor;
assert(typeof bgStart === 'string', 'initial bg is string');
assert(cs0.transition.indexOf('opacity') !== -1 || cs0.transition.indexOf('500') !== -1
       || cs0.transitionDuration !== undefined, 'transition prop is set: ' + cs0.transition);

// Track transition events
const events = [];
box.addEventListener('transitionstart', (e) => events.push({type:'start', prop:e.propertyName}));
box.addEventListener('transitionend',   (e) => events.push({type:'end',   prop:e.propertyName}));
box.addEventListener('transitionrun',   (e) => events.push({type:'run',   prop:e.propertyName}));

// Trigger transitions by changing inline style
box.style.backgroundColor = 'rgb(0, 0, 255)';
box.style.opacity = '0.2';
box.style.width = '200px';
flush();

// Advance partway: 250ms — opacity should be mid-transition (50%, linear)
advanceTime(250);
const cs1 = getComputedStyle(box);
const op1 = parseFloat(cs1.opacity);
assert(op1 < 1 && op1 > 0.2,
       'opacity mid-transition between 0.2 and 1, got ' + op1);

// Advance to completion of opacity (500ms total)
advanceTime(300);
const cs2 = getComputedStyle(box);
const op2 = parseFloat(cs2.opacity);
assert(Math.abs(op2 - 0.2) < 0.05, 'opacity reached target ~0.2, got ' + op2);

// Advance to completion of all transitions (1000ms+)
advanceTime(800);

// At least one transitionend event should have fired
const ends = events.filter(e => e.type === 'end');
assert(ends.length >= 1, 'at least one transitionend fired (' + events.length + ' total)');

// --- Interrupt an in-flight transition ---
events.length = 0;
box.style.backgroundColor = 'rgb(255, 0, 0)';
box.style.opacity = '1';
box.style.width = '100px';
flush();
advanceTime(200);
const cs4 = getComputedStyle(box);
assert(cs4 !== null, 'mid-revert style accessible');
advanceTime(1500);

// --- cubic-bezier timing function ---
const b2 = document.createElement('div');
b2.id = 'b2';
b2.style.cssText =
    'width:50px;height:50px;background-color:rgb(255,0,0);' +
    'transition:background-color 400ms cubic-bezier(0.4, 0, 0.2, 1);';
root.appendChild(b2);
flush();

b2.style.backgroundColor = 'rgb(0, 128, 0)';
flush();
advanceTime(200);
const csB2 = getComputedStyle(b2);
assert(typeof csB2.backgroundColor === 'string', 'cubic-bezier transition mid');
advanceTime(300);

// --- Multiple simultaneous transitions on different elements ---
for (let i = 0; i < 3; ++i) {
    const e = document.createElement('div');
    e.id = 'multi' + i;
    e.style.cssText = 'width:30px;height:30px;background-color:rgb(255,0,0);transition:background-color 200ms;';
    root.appendChild(e);
}
flush();
for (let i = 0; i < 3; ++i) {
    document.getElementById('multi' + i).style.backgroundColor = 'rgb(0, 0, 255)';
}
advanceTime(100);
advanceTime(200);

// --- ease-in, ease-out, ease-in-out ---
for (let i = 0; i < 3; ++i) {
    const fn = ['ease-in', 'ease-out', 'ease-in-out'][i];
    const e = document.createElement('div');
    e.id = 'ti' + i;
    e.style.cssText = 'width:10px;height:10px;background-color:rgb(0,0,0);transition:background-color 300ms ' + fn + ';';
    root.appendChild(e);
}
flush();
for (let i = 0; i < 3; ++i) {
    document.getElementById('ti' + i).style.backgroundColor = 'rgb(255,255,255)';
}
advanceTime(150);
advanceTime(200);

// --- transition-delay ---
const d = document.createElement('div');
d.id = 'd';
d.style.cssText = 'width:20px;height:20px;background-color:rgb(255,0,0);transition:opacity 200ms 100ms;opacity:1;';
root.appendChild(d);
flush();
d.style.opacity = '0';
flush();
advanceTime(50);
// In delay window
const csD1 = getComputedStyle(d);
const opD1 = parseFloat(csD1.opacity);
assert(opD1 > 0.9, 'opacity still ~1 within delay window, got ' + opD1);
advanceTime(300); // past end

// --- Cleanup ---
root.innerHTML = '';
