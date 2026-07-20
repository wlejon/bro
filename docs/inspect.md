# CSS/Layout Inspection Tool

Headless globals for inspecting element layout, computed styles, and DOM structure. Available in all headless invocation modes (REPL, script, `-e`).

## Functions

### `inspect(selector [, verbose])`

Returns a formatted string with full box model, position, computed styles, and DOM info for the matched element.

```
inspect('#myDiv')
inspect('.card', true)   // verbose: show ALL computed styles
```

**Output:**
```
<DIV#myDiv.container>
  Box Model:
    content:  200 x 100
    padding:  10 20
    border:   1
    margin:   8 0
    full:     242 x 120
  Position:
    relative: (18, 132)
    absolute: (172, 256)
  Scroll:                          // only shown if element scrolls
    scrollTop:    45
    scrollHeight: 800
    overflow:     680px hidden
  Computed Styles:
    display: flex
    position: relative
    color: #e0e0e0
    background-color: #1a1a2e
    font-size: 16
    font-family: Arial, sans-serif
  Inline Styles:                   // only shown if element has inline styles
    color: red; margin-top: 10px
  Attributes:                      // non-id, non-class, non-style attrs
    data-active="true"
  Children: 3 elements, 1 text node
  Shadow DOM: open                 // only shown if element has shadow root
```

**Default mode** shows key layout/visual properties: display, position, flex properties, dimensions, overflow, color, background, font, opacity, visibility, z-index, transform, text-align, box-sizing, grid properties.

**Verbose mode** (`true` as second arg) shows every computed style property alphabetically.

### `inspectTree(selector [, depth])`

Returns a tree view showing layout geometry for all descendants. Default depth is 3. Uses the composed tree (sees through shadow DOM).

```
inspectTree('body', 2)
```

**Output:**
```
<BODY>  1024x768 @ (0, 0)
  <DIV#app>  984x728 @ (20, 20)
    <H1>  944x32 @ (40, 40)
    <DIV.cards>  944x400 @ (40, 82) [flex]
    <FOOTER>  944x60 @ (40, 492)
  <SCRIPT>  0x0 @ (0, 0) [none]
```

Each line shows: `indent` `<TAG#id.classes>` `WxH` `@ (x, y)` plus inline annotations:
- `[flex]`, `[inline-block]`, `[none]`, etc.: non-block display values
- `[relative]`, `[absolute]`, `[fixed]`: non-static position
- `[overflow:auto]`: overflow clipping
- `[shadow]`: element has a shadow root

Text nodes appear as `#text "content..."` (whitespace-only nodes are skipped, long text is truncated to 40 chars).

### `computedStyle(selector [, property])`

Returns computed style values.

```
computedStyle('#btn', 'display')       // returns "flex"
computedStyle('#btn', 'font-size')     // returns "14"
computedStyle('#btn')                  // returns JS object with all properties
```

With a property name: returns the string value (or `""` if not set).
Without: returns a plain JS object with all computed style key-value pairs, usable in assertions:

```js
let styles = computedStyle('#btn');
assert(styles.display === 'flex', 'button should be flex');
assert(styles['font-size'] === '14', 'font size should be 14');
```

### `elements(selector)`

Returns a summary of all matching elements with their sizes and positions.

```
elements('.card')
```

**Output:**
```
4 matches:
  [0] <DIV#card0.card>  200x100 @ (20, 80)
  [1] <DIV#card1.card>  200x100 @ (230, 80)
  [2] <DIV#card2.card>  200x100 @ (440, 80)
  [3] <DIV#card3.card>  200x100 @ (650, 80)
```

### `inspectOverlay(panelName, selector [, verbose])`

Same output as `inspect()`, but resolves the selector inside a **system panel's** document instead of the app's, the perf HUD, menu bar, preferences modal, splash and inspector each have their own `Document` (see [system-panels.md](system-panels.md)). Panel names are the relative paths the panel scan found, without the extension: `"perf"`, `"menu"`, `"nav"`, `"splash"`, `"inspector"`, `"settings/graphics"`.

```js
inspectOverlay('perf', '#fps')
inspectOverlay('settings/graphics', '.row', true)
```

Throws `TypeError` if no element in that panel matches the selector.

### `inspectOverlayTree(panelName, selector [, depth])`

Same output as `inspectTree()`, rooted at an element inside a system panel. Default depth is 3.

```js
inspectOverlayTree('nav', 'body', 4)
```

### `overlayPanels()`

Returns an array of the loaded panel names, the values `inspectOverlay` and `inspectOverlayTree` accept.

```js
overlayPanels()   // ['menu', 'perf', 'nav', 'settings/graphics', ...]
```

## Usage Patterns

### Debugging layout in scripts

```js
// Why is this element in the wrong place?
console.log(inspect('#sidebar'));

// What does the tree look like?
console.log(inspectTree('body', 4));

// Check specific style
assert(computedStyle('#modal', 'display') === 'none', 'modal should be hidden');
```

### Debugging from the REPL

In the REPL, return values are printed automatically, no `console.log` needed:

```
bro> inspect('#header')
<HEADER#header>
  Box Model: ...

bro> inspectTree('body', 2)
<BODY> ...

bro> computedStyle('#btn', 'color')
#ffffff

bro> elements('.active')
2 matches: ...
```

### Visual regression + layout assertions

```js
flush();

// Verify element dimensions
let styles = computedStyle('.card');
assert(styles.display === 'flex', 'card should use flexbox');

// Verify position
let rect = document.querySelector('.card').getBoundingClientRect();
assert(rect.x >= 0, 'card should be on screen');

// Dump tree for debugging if something looks wrong
console.log(inspectTree('.card', 3));
screenshot('card-layout.png', '.card');
```

### Shadow DOM inspection

`inspectTree` uses the composed tree, so it sees through shadow boundaries:

```js
inspectTree('my-component', 5)
// Shows shadow root children and slotted content
```

`inspect` reports whether the element has a shadow root and its mode:

```
Shadow DOM: open
```

## Notes

- All functions automatically call `flush()` before inspecting, so layout is always current.
- Selectors use the same syntax as `document.querySelector` (CSS selectors, `#id` shorthand).
- Throws `TypeError` if no element matches the selector.
- Positions are absolute (accumulated from layout parent chain, accounting for scroll offsets).
- `full` in box model = content + padding + border (same as `offsetWidth`/`offsetHeight`).
- Edge values use CSS shorthand notation: `5` (all), `5 10` (vertical horizontal), `5 10 5 10` (top right bottom left).
