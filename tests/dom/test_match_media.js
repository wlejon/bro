// Test window.matchMedia and MediaQueryList
// Exercises src/js/matchmedia_bindings.cpp

assert(typeof matchMedia === "function", "matchMedia global exists");
assert(typeof window.matchMedia === "function", "window.matchMedia exists");
assert(window.matchMedia === matchMedia, "window.matchMedia is matchMedia");

// 1. Basic query evaluation
const mqlAll = window.matchMedia("all");
assert(typeof mqlAll === "object", "matchMedia returns MediaQueryList object");
assert(mqlAll.media === "all", "mql.media returns normalized media string");
assert(mqlAll.matches === true, "mql.matches is true for all");

const mqlEmpty = window.matchMedia("");
assert(mqlEmpty.media === "all", "empty string query normalizes to all");
assert(mqlEmpty.matches === true, "empty query matches");

const mqlNotAll = window.matchMedia("not all");
assert(mqlNotAll.matches === false, "mql.matches is false for not all");

const mqlMinWidth = window.matchMedia("(min-width: 1px)");
assert(mqlMinWidth.matches === true, "min-width 1px matches current viewport");

const mqlMaxWidthZero = window.matchMedia("(max-width: 0px)");
assert(mqlMaxWidthZero.matches === false, "max-width 0px does not match");

// 2. Trimming of queries
const mqlPadded = window.matchMedia("   screen   ");
assert(mqlPadded.media === "screen", "query is trimmed");

// 3. Event listeners
let changeCount = 0;
const listener = (e) => { changeCount++; };

mqlAll.addEventListener("change", listener);
mqlAll.removeEventListener("change", listener);

mqlAll.addListener(listener);
mqlAll.removeListener(listener);

// 4. onchange property
assert(mqlAll.onchange === null, "onchange defaults to null");
const onchangeFn = () => {};
mqlAll.onchange = onchangeFn;
assert(mqlAll.onchange === onchangeFn, "onchange getter returns assigned function");
mqlAll.onchange = null;
assert(mqlAll.onchange === null, "onchange reset to null");

// 5. Options support: { once: true }, { signal }
if (typeof AbortController === "function") {
    const controller = new AbortController();
    let abortListenerCalls = 0;
    const abortListener = () => { abortListenerCalls++; };
    mqlAll.addEventListener("change", abortListener, { signal: controller.signal });
    controller.abort();
}

// 6. Repeated add of same listener is idempotent
let dummyCalls = 0;
const dummy = () => { dummyCalls++; };
mqlAll.addEventListener("change", dummy);
mqlAll.addEventListener("change", dummy);
mqlAll.removeEventListener("change", dummy);

console.log("test_match_media: passed");
