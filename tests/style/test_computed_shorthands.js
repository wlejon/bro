// getComputedStyle serializes shorthands from their longhands, and reports
// the initial values htmlayout keeps no entry for, as Chromium does. Every
// expected string is Chromium's.

const style = document.createElement('style');
style.textContent = `
#d { margin: 1px 2px; border: 1px solid red; padding: 3px; background: blue; font: 12px serif; inset: 0;
     flex: 1; border-radius: 2px; outline: 1px solid red; list-style: none; columns: 2; place-items: center;
     border-width: 1px 2px; flex-flow: row wrap; overflow: auto hidden; transition: opacity 1s; }
#e { margin: 5px; padding: 0; border: none; background: no-repeat center / cover red;
     font: bold italic 14px/1.5 Arial, sans-serif; flex: 0 1 auto; transition: all 0s ease 0s; border-radius: 50%; }`;
document.head.appendChild(style);
const d = document.createElement('div');
d.id = 'd';
const e = document.createElement('div');
e.id = 'e';
document.body.append(d, e);
const cd = getComputedStyle(d), ce = getComputedStyle(e);

const expect = [
    [cd, 'margin', '1px 2px'], [cd, 'padding', '3px'], [cd, 'inset', '0px'], [cd, 'border-width', '1px 2px'],
    [cd, 'border-style', 'solid'], [cd, 'border-color', 'rgb(255, 0, 0)'], [cd, 'border-top', '1px solid rgb(255, 0, 0)'],
    [cd, 'border', ''], [cd, 'border-radius', '2px'], [cd, 'outline', 'rgb(255, 0, 0) solid 1px'],
    [cd, 'list-style', 'outside none none'], [cd, 'columns', '2'], [cd, 'place-items', 'center'],
    [cd, 'flex', '1 1 0%'], [cd, 'flex-flow', 'row wrap'], [cd, 'overflow', 'auto hidden'], [cd, 'font', '12px serif'],
    [cd, 'background', 'rgb(0, 0, 255) none repeat scroll 0% 0% / auto padding-box border-box'],
    [cd, 'transition', 'opacity 1s'], [cd, 'top', '0px'], [cd, 'font-stretch', '100%'],
    [ce, 'margin', '5px'], [ce, 'padding', '0px'], [ce, 'border', '0px none rgb(0, 0, 0)'], [ce, 'border-radius', '50%'],
    [ce, 'font', 'italic 700 14px / 21px Arial, sans-serif'], [ce, 'font-style', 'italic'], [ce, 'font-size', '14px'],
    [ce, 'font-family', 'Arial, sans-serif'], [ce, 'flex', '0 1 auto'], [ce, 'transition', 'all'],
    [ce, 'outline', 'rgb(0, 0, 0) none 3px'], [ce, 'place-items', 'normal'], [ce, 'columns', 'auto'],
    [ce, 'background-position', '50% 50%'], [ce, 'background-position-x', '50%'], [ce, 'background-attachment', 'scroll'],
    [ce, 'list-style', 'outside none disc'], [ce, 'justify-items', 'normal'],
];
for (const [cs, prop, want] of expect) {
    const got = cs.getPropertyValue(prop);
    assert(got === want, `${cs === cd ? '#d' : '#e'} ${prop}: '${got}', want '${want}'`);
}

console.log('test_computed_shorthands: OK');
