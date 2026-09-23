// style() container queries against a container that is not the element's
// parent: a named container further up. The cascade answers a query against
// the parent from the style it is resolving with; any other container is
// asked through ElementRef::computedStyleValue, which bro answers from the
// element's computed style — its own custom properties, inherited ones, and
// standard properties (a size query's em reads the container's font-size).

const sheet = document.createElement('style');
sheet.textContent = `
  #card   { container-name: card; --theme: dark; }
  #panel  { container-name: panel; }
  #sized  { container-type: inline-size; container-name: sized;
            width: 300px; font-size: 20px; }
  .leaf   { color: rgb(0, 0, 0); }
  @container card style(--theme: dark)  { #a { color: rgb(0, 128, 0); } }
  @container card style(--theme: light) { #b { color: rgb(0, 128, 0); } }
  @container panel style(--theme: dark) { #c { color: rgb(0, 128, 0); } }
  @container card style(--missing)      { #d { color: rgb(0, 128, 0); } }
  /* 16em is 320px at the container's 20px font-size (256px at 16px). */
  @container sized (max-width: 16em)    { #e { color: rgb(0, 128, 0); } }
`;
document.head.appendChild(sheet);
document.body.innerHTML = `
  <div id="card"><div><div class="leaf" id="a">a</div><div class="leaf" id="b">b</div>
    <div class="leaf" id="d">d</div></div></div>
  <div style="--theme: dark"><div id="panel"><div><div class="leaf" id="c">c</div></div></div></div>
  <div id="sized"><div><div class="leaf" id="e">e</div></div></div>
`;
flush();
advanceTime(16);
flush();

const color = (id) => getComputedStyle(document.getElementById(id)).color;
const GREEN = 'rgb(0, 128, 0)';
const BLACK = 'rgb(0, 0, 0)';

assert(color('a') === GREEN,
    'style(--theme: dark) against a named grandparent container matches, got ' + color('a'));
assert(color('b') === BLACK,
    'style(--theme: light) against it does not, got ' + color('b'));
assert(color('c') === GREEN,
    'a named container\'s inherited custom property answers style(), got ' + color('c'));
assert(color('d') === BLACK,
    'a custom property the container does not have is unset, got ' + color('d'));
assert(color('e') === GREEN,
    'an em size query reads the named container\'s own font-size, got ' + color('e'));

// A change on the container re-evaluates the query for its descendants.
document.getElementById('card').style.setProperty('--theme', 'light');
flush();
assert(color('a') === BLACK, 'after --theme: light the dark query no longer holds, got ' + color('a'));
assert(color('b') === GREEN, 'and the light query does, got ' + color('b'));

console.log('test_container_style_query_named: OK');
