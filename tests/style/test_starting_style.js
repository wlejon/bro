// @starting-style (CSS Transitions 2 §3.1). An element with no before-change
// style — its first style, or its first after it or an ancestor left
// display:none — transitions from its starting style: its style with the
// @starting-style rules matching too. Both forms: a top-level block of rules
// and a block nested in a style rule. The rules match nowhere else: not in
// getComputedStyle, and not on an ordinary style change.
//
// Before: @starting-style was dropped by the parser, so nothing transitioned
// into view; an element inserted or shown took its end style at once.

const sheet = document.createElement('style');
sheet.textContent = `
  .fade { opacity: 1; transition: opacity 1000ms linear; }
  @starting-style { .fade { opacity: 0; } }
  .grow { width: 100px; height: 10px; transition: width 1000ms linear;
          @starting-style { width: 0px; } }
  .kid { opacity: 1; transition: opacity 1000ms linear; }
  @starting-style { .kid { opacity: 0.2; } }
  .still { opacity: 1; }
  @starting-style { .still { opacity: 0; } }
  .hidden { display: none; }
`;
document.head.appendChild(sheet);
flush();

const root = document.getElementById('root');
const $ = (id) => document.getElementById(id);
const op = (id) => parseFloat(getComputedStyle($(id)).opacity);
function near(v, want, eps, msg) {
    assert(Math.abs(v - want) <= eps, msg + ' (expected ~' + want + ', got ' + v + ')');
}

// --- inserted: its first style transitions from the starting style.
root.insertAdjacentHTML('beforeend', '<div id="ins" class="fade" style="width:10px;height:10px"></div>');
assert($('ins').getAnimations().length === 1,
       'an inserted element starts a transition from its starting style, got ' +
       $('ins').getAnimations().length);
near(op('ins'), 0, 0.02, 'it shows the starting value first');
advanceTime(500);
near(op('ins'), 0.5, 0.05, 'and runs to its style');
advanceTime(600);
near(op('ins'), 1, 0.001, 'ending on its style');
assert($('ins').getAnimations().length === 0, 'the transition is over');

// --- the nested form: declarations for the enclosing selector.
root.insertAdjacentHTML('beforeend', '<div id="nest" class="grow"></div>');
flush();
advanceTime(250);
near(parseFloat(getComputedStyle($('nest')).width), 25, 3,
     'a nested @starting-style block gives the enclosing rule its starting style');
advanceTime(1000);
near(parseFloat(getComputedStyle($('nest')).width), 100, 0.5, 'and it ends on its style');

// --- leaving display:none, the element and its descendants start over.
root.insertAdjacentHTML('beforeend',
    '<div id="box" class="fade hidden" style="width:10px;height:10px">' +
    '<div id="kid" class="kid" style="width:5px;height:5px"></div></div>');
advanceTime(1100);
assert($('box').getAnimations().length === 0 && $('kid').getAnimations().length === 0,
       'nothing transitions while display:none');
$('box').classList.remove('hidden');
flush();
assert($('box').getAnimations().length === 1, 'shown, it transitions from its starting style');
near(op('box'), 0, 0.02, 'from the starting value');
assert($('kid').getAnimations().length === 1,
       'a descendant of the element shown transitions from its own starting style');
near(op('kid'), 0.2, 0.02, 'from its starting value');
advanceTime(500);
near(op('box'), 0.5, 0.05, 'the shown element runs');
near(op('kid'), 0.6, 0.05, 'the descendant runs');
advanceTime(600);
near(op('box'), 1, 0.001, 'the shown element ends on its style');

// --- an ordinary style change is not one: the starting style stays out.
$('box').style.width = '20px';
flush();
assert($('box').getAnimations().length === 0, 'a change to a rendered element ignores @starting-style');
near(op('box'), 1, 0.001, 'and its opacity stays');

// --- without a transition the starting style is never seen, and the
//     @starting-style rules do not match in getComputedStyle.
root.insertAdjacentHTML('beforeend', '<div id="still" class="still" style="width:10px;height:10px"></div>');
flush();
near(op('still'), 1, 0.001, 'an element with no transition shows its style');
assert($('still').getAnimations().length === 0, 'and starts nothing');

// --- no @starting-style rule for the element: an inserted element with a
//     transition starts nothing.
root.insertAdjacentHTML('beforeend',
    '<div id="plain" style="width:10px;height:10px;opacity:0.5;transition:opacity 1000ms linear"></div>');
flush();
assert($('plain').getAnimations().length === 0, 'no starting style, no transition');
near(op('plain'), 0.5, 0.001, 'it takes its style at once');
