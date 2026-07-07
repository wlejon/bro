#pragma once

namespace bro::engine {

// Browser-like default stylesheet applied before any app styles.
// Provides sensible visual defaults so apps render correctly without
// requiring an explicit stylesheet.  Roughly follows the CSS 2.1
// default rendering spec + common browser UA sheet conventions.
inline constexpr const char* kDefaultStyles = R"CSS(

html {
    display: block;
    background-color: #fff;
    color: #000;
    font-family: Arial, Helvetica, sans-serif;
    /* medium, not 16px: the keyword resolves to 13px inside generic
       monospace (Chromium quirk) and 16px everywhere else; a fixed px
       value here would kill that re-resolution chain. */
    font-size: medium;
    line-height: normal;
}

/* ---------- Hidden elements ---------- */

head, title, meta, link, style, script, noscript {
    display: none;
}

/* ---------- Directionality ---------- */
/* The HTML `dir` attribute is a presentational hint for the `direction`
   property (HTML §15.3.3). A nested dir="ltr"/"rtl" overrides the inherited
   direction, which is how ltr islands inside an rtl block resolve. */
[dir="ltr"] { direction: ltr; }
[dir="rtl"] { direction: rtl; }

/* ---------- Block-level elements ---------- */

body, div, p, pre, section, article, nav, aside, header, footer, main,
figure, figcaption, blockquote, fieldset, form, details, summary,
address, hgroup, search, dl, dt, dd, dialog, canvas {
    display: block;
}

body {
    margin: 8px;
}

h1, h2, h3, h4, h5, h6 {
    display: block;
    font-weight: bold;
}

h1 { font-size: 2em; margin-top: 0.67em; margin-bottom: 0.67em; }
h2 { font-size: 1.5em; margin-top: 0.83em; margin-bottom: 0.83em; }
h3 { font-size: 1.17em; margin-top: 1em; margin-bottom: 1em; }
h4 { font-size: 1em; margin-top: 1.33em; margin-bottom: 1.33em; }
h5 { font-size: 0.83em; margin-top: 1.67em; margin-bottom: 1.67em; }
h6 { font-size: 0.67em; margin-top: 2.33em; margin-bottom: 2.33em; }

p { margin-top: 1em; margin-bottom: 1em; }

blockquote { margin: 1em 40px; }
figure { margin: 1em 40px; }
dl { margin-top: 1em; margin-bottom: 1em; }
dd { margin-left: 40px; }
address { font-style: italic; }

ul, ol { display: block; padding-left: 40px; margin-top: 1em; margin-bottom: 1em; }
li { display: list-item; }
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
/* Nested lists: no vertical margins, and ul markers cycle disc → circle →
   square by nesting depth (matches the HTML rendering spec / Chromium). */
ul ul, ul ol, ol ul, ol ol { margin-top: 0; margin-bottom: 0; }
ul ul, ol ul { list-style-type: circle; }
ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square; }

table { display: table; border-collapse: separate; border-spacing: 2px; box-sizing: border-box; }
thead { display: table-header-group; }
tbody { display: table-row-group; }
tfoot { display: table-footer-group; }
tr { display: table-row; }
td, th { display: table-cell; padding: 1px; vertical-align: middle; }
th { font-weight: bold; text-align: center; }
caption { display: table-caption; text-align: center; }
colgroup { display: table-column-group; }
col { display: table-column; }

b, strong { font-weight: bold; }
i, em { font-style: italic; }
u { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
small { font-size: 0.83em; }
sub { font-size: 0.83em; vertical-align: sub; }
sup { font-size: 0.83em; vertical-align: super; }
mark { background-color: yellow; }

span, a, abbr, cite, q { display: inline; }
br { display: inline; }

/* Inline quotations: the UA wraps <q> content in locale quote marks via
   generated content, honouring the (inherited) `quotes` property and the
   current quote-nesting depth. */
q::before { content: open-quote; }
q::after  { content: close-quote; }

/* ---------- Form controls ---------- */

button, input[type="button"], input[type="submit"], input[type="reset"] {
    display: inline-block;
    box-sizing: border-box;
    padding: 1px 6px;
    border: 2px solid #767676;
    border-radius: 3px;
    background-color: #f0f0f0;
    color: #000;
    font-size: 13.333px;
    cursor: pointer;
}

input, textarea, select {
    display: inline-block;
    padding: 1px 2px;
    border: 2px solid #767676;
    background-color: #fff;
    color: #000;
    font-size: 13.333px;
}

/* Text-like inputs derive their width from the `size` attribute (default 20)
   against the resolved font — computed in ElInput::getContentSize, not a
   fixed px value, so the box grows with `font-size` the way browsers do. */

textarea {
    box-sizing: content-box;
    padding: 2px;
    border: 1px solid #767676;
}

select {
    box-sizing: border-box;
    padding: 0;
    border: 1px solid #767676;
}

input[type="checkbox"] {
    box-sizing: border-box;
    width: 13px;
    height: 13px;
    padding: 0;
    border: none;
    background-color: transparent;
    margin: 3px 3px 3px 4px;
}

input[type="radio"] {
    box-sizing: border-box;
    width: 13px;
    height: 13px;
    padding: 0;
    border: none;
    background-color: transparent;
    margin: 3px 3px 0px 5px;
}

input[type="range"] {
    padding: 0;
    border: none;
    background-color: transparent;
    height: 20px;
}

input[type="color"] {
    padding: 0;
    border: none;
    background-color: transparent;
}

input[type="hidden"] {
    display: none;
}

/* Options live in the select's popup, never in document flow: keep them
   display:none so getBoundingClientRect reports an empty rect (as browsers do
   for a closed select's options) instead of projecting them onto the select's
   content origin. The padding matches the browser's option box for parity of
   the computed style. */
option {
    display: none;
    padding: 0 2px 1px 2px;
}

/* ---------- fieldset / legend ---------- */

fieldset {
    margin-left: 2px;
    margin-right: 2px;
    padding: 0.35em 0.75em 0.625em;
    border: 2px groove #767676;
}

legend {
    display: block;
    padding-left: 2px;
    padding-right: 2px;
}

/* ---------- progress / meter ---------- */

progress {
    display: inline-block;
    box-sizing: border-box;
    width: 160px;
    height: 16px;
    vertical-align: -0.2em;
}

meter {
    display: inline-block;
    box-sizing: border-box;
    width: 80px;
    height: 16px;
    vertical-align: -0.2em;
}

template {
    display: none;
}

/* ---------- <details> / <summary> ---------- */
/* When the <details> element is not open, hide everything except its
   <summary>. Clicking the <summary> toggles the [open] attribute (see
   the click handler in replaced_elements.cpp), and CSS does the rest. */

details > summary {
    display: list-item;
    list-style-type: disclosure-closed;
    list-style-position: inside;
    cursor: pointer;
}

details[open] > summary {
    list-style-type: disclosure-open;
}

details:not([open]) > *:not(summary) {
    display: none;
}

/* ---------- Links ---------- */

a {
    color: #00e;
    text-decoration: underline;
}

/* ---------- Horizontal rule ---------- */

/* HTML rendering spec: hr is a block with inset 1px borders whose color
   derives from `color: gray`, centered by auto inline margins when a width
   is set. */
hr {
    display: block;
    overflow: hidden;
    margin-top: 0.5em;
    margin-bottom: 0.5em;
    margin-left: auto;
    margin-right: auto;
    border-style: inset;
    border-width: 1px;
    color: gray;
}

/* ---------- Code / preformatted ---------- */

code, kbd, samp {
    font-family: monospace;
}

pre {
    font-family: monospace;
    white-space: pre;
    margin-top: 1em;
    margin-bottom: 1em;
}

/* ---------- SVG ---------- */

svg {
    display: inline-block;
    overflow: hidden;
}

/* Chromium computes SVG <text> as a block-level box (getComputedStyle reports
   display:block); <tspan> stays inline. Match so computed-style parity holds. */
text {
    display: block;
}

)CSS";

} // namespace bro::engine
