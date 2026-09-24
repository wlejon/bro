// transition-behavior (CSS Transitions 2). A value pair only a discrete flip
// can join (two keywords, `auto` and a length, display) does not transition:
// the new value applies at once, with no CSSTransition and no events. Under
// `transition-behavior: allow-discrete` it does, flipping halfway through.
// display allows an exit animation: going to display:none keeps the element
// rendered at its old display until the transition ends, while its other
// properties transition. The `transition` shorthand carries the keyword and
// a later transition-behavior longhand overrides the shorthand's reset.
// visibility is interpolable on its own (visible throughout when one end is
// visible), and `transform: none` runs from the identity.
//
// Before: every changed property transitioned, discrete ones snapping at 50%,
// and transition-behavior was ignored.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="plain" style="width:10px;height:10px;justify-content:flex-start;transition:all 1000ms linear"></div>' +
    '<div id="auto" style="width:10px;height:10px;transition:width 1000ms linear"></div>' +
    '<div id="disc" style="width:10px;height:10px;justify-content:flex-start;transition:all 1000ms linear allow-discrete"></div>' +
    '<div id="late" style="width:10px;height:10px;justify-content:flex-start;transition:justify-content 1000ms linear;transition-behavior:allow-discrete"></div>' +
    '<div id="exit" style="width:10px;height:10px;opacity:1;display:flex;transition:opacity 1000ms linear, display 1000ms allow-discrete"></div>' +
    '<div id="gone" style="width:10px;height:10px;opacity:1;transition:opacity 1000ms linear"></div>' +
    '<div id="vis" style="width:10px;height:10px;transition:visibility 1000ms linear"></div>' +
    '<div id="tf" style="width:10px;height:10px;transform:none;transition:transform 1000ms linear"></div>';
flush();

const $ = (id) => document.getElementById(id);
const cs = (id) => getComputedStyle($(id));
function near(v, want, eps, msg) {
    assert(Math.abs(v - want) <= eps, msg + ' (expected ~' + want + ', got ' + v + ')');
}
const events = [];
for (const t of ['transitionrun', 'transitionstart', 'transitionend', 'transitioncancel'])
    for (const id of ['plain', 'exit'])
        $(id).addEventListener(t, (e) => events.push(id + ':' + e.type + ':' + e.propertyName));

// Longhands from the shorthand.
assert(cs('disc').transitionBehavior === 'allow-discrete',
       'the shorthand sets transition-behavior: ' + cs('disc').transitionBehavior);
assert(cs('plain').transitionBehavior === 'normal', 'and resets it: ' + cs('plain').transitionBehavior);
assert(cs('late').transitionBehavior === 'allow-discrete', 'a later longhand overrides the shorthand');
assert(cs('plain').transitionDuration === '1000ms', 'transition-duration from the shorthand');

// --- a discrete pair without allow-discrete: no transition.
$('plain').style.justifyContent = 'center';
$('auto').style.width = 'auto';
assert($('plain').getAnimations().length === 0,
       'justify-content does not transition by default: ' + $('plain').getAnimations().length);
assert($('auto').getAnimations().length === 0, 'nor does width to auto');
assert(cs('plain').justifyContent === 'center', 'the new value applies at once: ' + cs('plain').justifyContent);
assert(cs('auto').width !== '10px', 'width is auto at once: ' + cs('auto').width);

// --- allow-discrete: a CSSTransition that flips halfway.
$('disc').style.justifyContent = 'center';
$('late').style.justifyContent = 'center';
assert($('disc').getAnimations().length === 1, 'allow-discrete: a transition runs');
assert($('late').getAnimations().length === 1, 'allow-discrete from the longhand too');
advanceTime(400);
assert(cs('disc').justifyContent === 'flex-start', 'before halfway, the old value: ' + cs('disc').justifyContent);
advanceTime(200);
assert(cs('disc').justifyContent === 'center', 'past halfway, the new value: ' + cs('disc').justifyContent);
assert(cs('late').justifyContent === 'center', 'and for the longhand: ' + cs('late').justifyContent);
advanceTime(600);

// --- display exit animation.
$('exit').style.cssText += ';display:none;opacity:0';
$('gone').style.cssText += ';display:none;opacity:0';
flush();
assert($('gone').getAnimations().length === 0, 'display:none without a display transition cancels');
assert(cs('exit').display === 'flex', 'the exiting element keeps its display: ' + cs('exit').display);
assert($('exit').getAnimations().length === 2, 'display and opacity transition: ' +
       $('exit').getAnimations().map((a) => a.transitionProperty).join());
advanceTime(500);
near(parseFloat(cs('exit').opacity), 0.5, 0.03, 'opacity fades while it exits');
assert(cs('exit').display === 'flex', 'still displayed halfway: ' + cs('exit').display);
assert($('exit').getBoundingClientRect().height === 10, 'and still laid out');
advanceTime(600);
flush();
assert(cs('exit').display === 'none', 'display:none once it ends: ' + cs('exit').display);
assert($('exit').getBoundingClientRect().height === 0, 'and no longer laid out');
assert($('exit').getAnimations().length === 0, 'nothing left running');
assert(events.indexOf('exit:transitionend:display') >= 0, 'transitionend for display: ' + events.join());
advanceTime(100);
flush();
assert(cs('exit').display === 'none', 'and it stays gone');
assert(events.filter((e) => e.startsWith('plain:')).length === 0,
       'the discrete change fired no events: ' + events.join());

// Coming back: no before-change style, so it shows at once.
$('exit').style.display = 'flex';
flush();
assert(cs('exit').display === 'flex', 'shown again: ' + cs('exit').display);

// --- visibility: visible throughout when one end is visible.
$('vis').style.visibility = 'hidden';
assert($('vis').getAnimations().length === 1, 'visibility transitions without allow-discrete');
advanceTime(900);
assert(cs('vis').visibility === 'visible', 'visible until the end: ' + cs('vis').visibility);
advanceTime(200);
flush();
assert(cs('vis').visibility === 'hidden', 'hidden at the end: ' + cs('vis').visibility);

// --- transform: none runs from the identity.
$('tf').style.transform = 'translateX(100px)';
advanceTime(500);
const m = /translateX\(([\d.]+)px\)/.exec(cs('tf').transform);
assert(m && Math.abs(+m[1] - 50) < 2, 'transform from none interpolates: ' + cs('tf').transform);
advanceTime(600);

root.innerHTML = '';
console.log('PASS: test_transition_behavior.js');
