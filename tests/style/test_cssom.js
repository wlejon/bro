// The CSSOM: document.styleSheets, a <style>'s sheet and its rules,
// insertRule / deleteRule / a rule's style edited restyling the page,
// disabled, removal of a <style> taking its rules, the readable at-rules,
// and constructable sheets adopted by the document and by a shadow root.
// Every expectation is what Chromium gives.

const color = (el) => getComputedStyle(el).color;
const RED = 'rgb(255, 0, 0)', GREEN = 'rgb(0, 128, 0)', BLUE = 'rgb(0, 0, 255)', BLACK = 'rgb(0, 0, 0)';

const style = document.createElement('style');
style.textContent = '.a { color: red; }\n@media (min-width: 1px) { .b { color: blue; } }\n' +
    '@keyframes spin { from { opacity: 0; } to { opacity: 1; } }\n@font-face { font-family: X; src: url(x.woff); }';
document.head.appendChild(style);
const a = document.createElement('div');
a.className = 'a';
const b = document.createElement('div');
b.className = 'b';
document.body.append(a, b);

assert(document.styleSheets.length >= 1, 'styleSheets has the <style>');
const sheet = style.sheet;
assert(sheet instanceof CSSStyleSheet, 'style.sheet is a CSSStyleSheet');
assert(Array.from(document.styleSheets).includes(sheet), 'and is in document.styleSheets');
assert(sheet.ownerNode === style, 'ownerNode');
assert(sheet.href === null && sheet.type === 'text/css', 'href / type');

const rules = sheet.cssRules;
assert(rules.length === 4, 'four rules: ' + rules.length);
assert(rules[0] instanceof CSSStyleRule && rules[0].type === 1, 'a CSSStyleRule');
assert(rules[0].selectorText === '.a', 'selectorText ' + rules[0].selectorText);
assert(rules[0].style.color === 'red' && rules[0].style.getPropertyValue('color') === 'red', 'rule style');
assert(rules[0].cssText === '.a { color: red; }', 'cssText ' + rules[0].cssText);
assert(rules[1] instanceof CSSMediaRule && rules[1].media.mediaText === '(min-width: 1px)', 'CSSMediaRule');
assert(rules[1].cssRules[0].selectorText === '.b', 'media rule children');
assert(rules[2] instanceof CSSKeyframesRule && rules[2].name === 'spin' && rules[2].cssRules.length === 2,
       'CSSKeyframesRule');
assert(rules[2].cssRules[1].keyText === 'to', 'keyText');
assert(rules[3] instanceof CSSFontFaceRule && rules[3].style.getPropertyValue('font-family') === 'X',
       'CSSFontFaceRule');

assert(color(a) === RED && color(b) === BLUE, 'the sheet applies');

// insertRule restyles; the <style>'s own text stays.
const before = style.textContent;
assert(sheet.insertRule('.a { color: green; }', 4) === 4, 'insertRule returns the index');
assert(color(a) === GREEN, 'insertRule restyles: ' + color(a));
assert(style.textContent === before, "the element's text is left as written");
assert(sheet.cssRules.length === 5, 'five rules now');

// A rule's style edited restyles.
sheet.cssRules[4].style.color = 'blue';
assert(color(a) === BLUE, 'rule.style.color = restyles: ' + color(a));
sheet.cssRules[4].style.setProperty('color', 'green');
assert(color(a) === GREEN, 'setProperty restyles');

// deleteRule.
sheet.deleteRule(4);
assert(color(a) === RED, 'deleteRule restyles: ' + color(a));
let threw = '';
try { sheet.deleteRule(99); } catch (e) { threw = e.name; }
assert(threw === 'IndexSizeError', 'deleteRule out of range: ' + threw);
threw = '';
try { sheet.insertRule('not a rule', 0); } catch (e) { threw = e.name; }
assert(threw === 'SyntaxError', 'insertRule of garbage: ' + threw);

// disabled.
sheet.disabled = true;
assert(sheet.disabled && color(a) === BLACK, 'a disabled sheet does not apply: ' + color(a));
sheet.disabled = false;
assert(color(a) === RED, 'enabled again');

// Removing the <style> takes its rules.
style.remove();
assert(color(a) === BLACK && color(b) === BLACK, 'a removed <style> no longer applies: ' + color(a));
assert(!Array.from(document.styleSheets).includes(sheet), 'and is out of styleSheets');
document.head.appendChild(style);
assert(color(a) === RED, 'appended again, it applies again: ' + color(a));

// Tree order: a <style> inserted before another loses to it.
const late = document.createElement('style');
late.textContent = '.a { color: blue; }';
document.head.appendChild(late);
assert(color(a) === BLUE, 'the later sheet wins');
const early = document.createElement('style');
early.textContent = '.a { color: green; }';
document.head.insertBefore(early, document.head.firstChild);
assert(color(a) === BLUE, 'a sheet inserted first loses: ' + color(a));
late.remove();
early.remove();

// Constructable sheets.
const cs = new CSSStyleSheet();
cs.replaceSync('.a { color: green; }');
assert(cs.cssRules.length === 1 && cs.ownerNode === null, 'a constructed sheet');
document.adoptedStyleSheets = [cs];
assert(color(a) === GREEN, 'adopted by the document, it applies after the <style>: ' + color(a));
cs.insertRule('.b { color: green; }');
assert(color(b) === GREEN, "an adopted sheet's insertRule restyles");
document.adoptedStyleSheets = [];
assert(color(a) === RED && color(b) === BLUE, 'unadopted');
document.adoptedStyleSheets.push(cs);
assert(document.adoptedStyleSheets.length === 1 && color(a) === GREEN, 'push() adopts, as on an ObservableArray');
document.adoptedStyleSheets.pop();
assert(color(a) === RED, 'pop() unadopts');
threw = '';
try { style.sheet.replaceSync('.a{}'); } catch (e) { threw = e.name; }
assert(threw === 'NotAllowedError', "replaceSync on an element's sheet: " + threw);
threw = '';
try { document.adoptedStyleSheets = [style.sheet]; } catch (e) { threw = e.name; }
assert(threw === 'NotAllowedError', 'adopting a non-constructed sheet: ' + threw);

// Shadow roots.
const host = document.createElement('div');
document.body.appendChild(host);
const root = host.attachShadow({ mode: 'open' });
const inner = document.createElement('span');
inner.className = 'a';
inner.textContent = 'x';
root.appendChild(inner);
assert(color(inner) === BLACK, 'document rules do not reach the shadow tree');
const shadowSheet = new CSSStyleSheet();
shadowSheet.replaceSync('.a { color: blue; }');
root.adoptedStyleSheets = [shadowSheet];
assert(color(inner) === BLUE, "a shadow root's adopted sheet applies: " + color(inner));
assert(color(a) === RED, 'and not to the document');
shadowSheet.replaceSync('.a { color: green; }');
assert(color(inner) === GREEN, 'replaceSync on an adopted sheet restyles');

let replaced = null;
new CSSStyleSheet().replace('.c { color: red; }').then((s) => { replaced = s; });

flush();
assert(replaced instanceof CSSStyleSheet && replaced.cssRules[0].selectorText === '.c', 'replace() resolves to the sheet');

console.log('test_cssom: OK');
