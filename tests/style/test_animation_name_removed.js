// An element whose animation-name stops naming its animation (the class that
// set it was removed) cancels it: the value snaps back to the un-animated
// style and animationcancel fires. The animation used to keep running, so an
// infinite animation span on after its class was gone, and taking the class
// off and putting it back (with a style flush in between, the web's restart
// idiom) carried on from the old start rather than restarting.

const style = document.createElement('style');
style.textContent = `
  @keyframes mv { from { transform: translateX(0px) } to { transform: translateX(1000px) } }
  .run { animation: mv 1000ms linear infinite; }
`;
document.head.appendChild(style);

const root = document.getElementById('root');
root.innerHTML = '<div id="a" style="width:10px;height:10px"></div>';
const a = document.getElementById('a');
const x = () => {
    const t = getComputedStyle(a).transform;
    if (t === 'none' || t === '') return 0;
    let m = /translateX\((-?[\d.e+-]+)px\)/.exec(t);
    if (m) return +m[1];
    m = /matrix\(([^)]*)\)/.exec(t);
    return m ? +m[1].split(',')[4] : NaN;
};

const cancels = [], starts = [];
a.addEventListener('animationcancel', (e) => cancels.push(e.animationName));
a.addEventListener('animationstart', (e) => starts.push(e.animationName + '@' + e.elapsedTime));

a.classList.add('run');
flush();
advanceTime(500);
flush();
const mid = x();
assert(mid > 400 && mid < 600, 'the animation runs, got ' + mid);
// The event carries its animationName and elapsedTime (they never reached
// the page before).
assert(starts.length === 1 && starts[0] === 'mv@0', 'animationstart names the animation: ' + JSON.stringify(starts));

// Remove + flush + re-add: a fresh start.
a.classList.remove('run');
void a.offsetWidth;
a.classList.add('run');
advanceTime(100);
flush();
const restarted = x();
assert(restarted > 50 && restarted < 150, 're-adding the class restarts it, got ' + restarted);
assert(cancels.length === 1 && cancels[0] === 'mv', 'the removal cancelled it: ' + JSON.stringify(cancels));

// Remove for good: it stops.
a.classList.remove('run');
advanceTime(100);
flush();
assert(x() === 0, 'with the class gone the element is un-animated, got ' + getComputedStyle(a).transform);
advanceTime(300);
flush();
assert(x() === 0, 'and stays so, got ' + getComputedStyle(a).transform);
assert(cancels.length === 2, 'a second animationcancel: ' + JSON.stringify(cancels));
