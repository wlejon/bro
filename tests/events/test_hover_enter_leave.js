// The hover transition: out / over on the elements left and entered, leave
// and enter on every element the pointer left or entered (not just the hit
// target), each mouse event after its pointer alias, enter and leave not
// bubbling.

document.body.style.margin = '0';
document.body.innerHTML = `
  <div id="outer" style="position:absolute; left:0; top:0; width:400px; height:200px">
    <div id="a" style="position:absolute; left:0; top:0; width:200px; height:200px">
      <div id="a1" style="position:absolute; left:20px; top:20px; width:100px; height:100px"></div>
    </div>
    <div id="b" style="position:absolute; left:200px; top:0; width:200px; height:200px"></div>
  </div>`;
flush();

const log = [];
for (const id of ['outer', 'a', 'a1', 'b']) {
    const el = document.getElementById(id);
    for (const t of ['mouseenter', 'mouseleave', 'pointerenter', 'pointerleave', 'mouseover', 'pointerover',
                     'mouseout', 'pointerout']) {
        el.addEventListener(t, (e) => {
            // Bubbling over / out show up on ancestors too; log where each lands.
            log.push(`${t}:${id}${e.target.id !== id ? '<' + e.target.id : ''}`);
        });
    }
}
const step = (x, y) => {
    log.length = 0;
    mouseMove(x, y);
    flush();
    return log.join(' ');
};

step(600, 300);  // outside everything
let got = step(50, 50);  // into a1, through outer and a
assert(got === 'pointerover:a1 pointerover:a<a1 pointerover:outer<a1 mouseover:a1 mouseover:a<a1 mouseover:outer<a1 ' +
       'pointerenter:outer mouseenter:outer pointerenter:a mouseenter:a pointerenter:a1 mouseenter:a1',
       'entering a1 enters outer, a and a1 (outermost first): ' + got);

got = step(150, 150);  // a1 -> a: leaves a1 only
assert(got === 'pointerout:a1 pointerout:a<a1 pointerout:outer<a1 mouseout:a1 mouseout:a<a1 mouseout:outer<a1 ' +
       'pointerleave:a1 mouseleave:a1 pointerover:a pointerover:outer<a mouseover:a mouseover:outer<a',
       'a1 to its parent: leave a1, no enter: ' + got);

got = step(300, 50);  // a -> b: leave a, enter b, outer untouched
assert(got.includes('pointerleave:a mouseleave:a') && got.includes('pointerenter:b mouseenter:b'),
       'a to b: leave a, enter b: ' + got);
assert(!/enter:outer|leave:outer/.test(got), 'outer still holds the pointer: ' + got);

got = step(600, 300);  // out of everything: leave b then outer
assert(got.endsWith('pointerleave:b mouseleave:b pointerleave:outer mouseleave:outer'),
       'leaving all: innermost first: ' + got);
