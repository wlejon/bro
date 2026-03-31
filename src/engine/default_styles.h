#pragma once

namespace bro::engine {

// Browser-like default stylesheet applied before any app styles.
// Provides sensible visual defaults so apps render correctly without
// requiring an explicit stylesheet.  Roughly follows the CSS 2.1
// default rendering spec + common browser UA sheet conventions.
inline constexpr const char* kDefaultStyles = R"CSS(

html {
    background-color: #fff;
    color: #000;
    font-family: Arial, Helvetica, sans-serif;
    font-size: 16px;
    line-height: 1.2;
}

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
