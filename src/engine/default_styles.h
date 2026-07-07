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

input {
    width: 169px;
}

input[type="checkbox"], input[type="radio"] {
    width: 13px;
    height: 13px;
    padding: 0;
    border: none;
    background-color: transparent;
    vertical-align: middle;
    margin-right: 3px;
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

option {
    display: none;
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

)CSS";

} // namespace bro::engine
