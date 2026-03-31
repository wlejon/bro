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
    font-size: 16px;
    line-height: 1.2;
}

/* ---------- Hidden elements ---------- */

head, title, meta, link, style, script, noscript {
    display: none;
}

/* ---------- Block-level elements ---------- */

body, div, p, section, article, nav, aside, header, footer, main,
figure, figcaption, blockquote, fieldset, form, details, summary,
address, hgroup, search, dl, dt, dd, dialog {
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

ul, ol { display: block; padding-left: 40px; margin-top: 1em; margin-bottom: 1em; }
li { display: list-item; }

b, strong { font-weight: bold; }
i, em { font-style: italic; }
u { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
small { font-size: 0.83em; }
sub { font-size: 0.83em; vertical-align: sub; }
sup { font-size: 0.83em; vertical-align: super; }
mark { background-color: yellow; }

span, a, abbr, cite, q { display: inline; }
br { display: block; }

/* ---------- Form controls ---------- */

button, input[type="button"], input[type="submit"], input[type="reset"] {
    display: inline-block;
    padding: 2px 8px;
    border: 1px solid #767676;
    border-radius: 3px;
    background-color: #f0f0f0;
    color: #000;
    font-size: 14px;
    cursor: pointer;
}

input, textarea, select {
    display: inline-block;
    padding: 2px 4px;
    border: 1px solid #767676;
    background-color: #fff;
    color: #000;
    font-size: 14px;
}

input[type="checkbox"], input[type="radio"] {
    padding: 0;
    border: none;
    background-color: transparent;
    vertical-align: middle;
}

input[type="range"] {
    padding: 0;
    border: none;
    background-color: transparent;
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

/* ---------- Links ---------- */

a {
    color: #00e;
    text-decoration: underline;
}

/* ---------- Horizontal rule ---------- */

hr {
    border: none;
    border-top: 1px solid #ccc;
    margin-top: 0.5em;
    margin-bottom: 0.5em;
}

/* ---------- Code / preformatted ---------- */

code, pre, kbd, samp {
    font-family: monospace;
    font-size: 14px;
}

pre {
    background-color: #f5f5f5;
    padding: 8px;
    border: 1px solid #ddd;
    overflow: auto;
}

/* ---------- SVG ---------- */

svg {
    display: inline-block;
    overflow: hidden;
}

)CSS";

} // namespace bro::engine
