// An external classic script of tests/engine/page_lifecycle_app: its own
// element is document.currentScript, whose src is an absolute URL.
__current.push(document.currentScript && document.currentScript.src);
