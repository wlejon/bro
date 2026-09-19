// Runtime surface dump — runs unchanged under the pre-transition QuickJS
// bro-headless and the bronze one, so the two outputs can be diffed.
//
//   bro-headless <appdir> scripts/binding-audit/surface_dump.js 2>&1 | grep -o "SURF .*" > out.txt
//
// Emits one line per member:  SURF <path> <kind>
//   kind = fn <length> | acc g|s|gs | obj | <typeof>
// Paths: namespaces are dotted (bro.tts.loadQwen); instance/prototype members
// use '#' (AudioContext#playClip) and are the UNION of own + prototype-chain
// members, so a method that moved from prototype to own property (or back)
// does not show as drift. Only plain ES2020 — no modules, no optional chaining.
(function () {
  var seen = typeof WeakMap === 'function' ? new WeakMap() : null;
  var seenList = [];
  function mark(o) {
    if (seen) { if (seen.has(o)) return true; seen.set(o, 1); return false; }
    for (var i = 0; i < seenList.length; i++) if (seenList[i] === o) return true;
    seenList.push(o); return false;
  }
  function emit(s) { console.log('SURF ' + s); }
  var SKIP = { length: 1, name: 1, prototype: 1, arguments: 1, caller: 1, constructor: 1, __proto__: 1 };
  var STOP_PROTOS = [Object.prototype, Function.prototype, Array.prototype, Error.prototype];
  function isStopProto(p) { for (var i = 0; i < STOP_PROTOS.length; i++) if (p === STOP_PROTOS[i]) return true; return false; }
  function isTypedOrBig(v) {
    if (ArrayBuffer.isView(v) || v instanceof ArrayBuffer) return true;
    if (Array.isArray(v) && v.length > 50) return true;
    return false;
  }
  function descOf(obj, n) { try { return Object.getOwnPropertyDescriptor(obj, n); } catch (e) { return null; } }
  function kindOf(d) {
    if (!d) return 'undefined';
    if (d.get || d.set) return 'acc ' + (d.get ? 'g' : '') + (d.set ? 's' : '');
    var v = d.value, t = typeof v;
    if (t === 'function') return 'fn ' + v.length;
    if (t === 'object') return v === null ? 'null' : 'obj';
    return t;
  }
  function names(o) { try { return Object.getOwnPropertyNames(o).sort(); } catch (e) { return []; } }

  // Namespace walk: own properties, recurse into plain objects and class statics.
  function walkNs(obj, path, depth) {
    if (!obj || (typeof obj !== 'object' && typeof obj !== 'function')) return;
    if (mark(obj)) return;
    if (depth > 5) return;
    var ns = names(obj);
    for (var i = 0; i < ns.length; i++) {
      var n = ns[i];
      if (SKIP[n] || /^\d+$/.test(n)) continue;
      var d = descOf(obj, n); if (!d) continue;
      var k = kindOf(d);
      emit(path + '.' + n + ' ' + k);
      if (d.get || d.set) continue;
      var v = d.value;
      if (typeof v === 'function') {
        // class: members via its prototype; statics via own props
        if (v.prototype && names(v.prototype).length > 1) walkProto(v.prototype, path + '.' + n);
        if (names(v).length > 3) walkNs(v, path + '.' + n, depth + 1);
      } else if (v && typeof v === 'object' && !isTypedOrBig(v)) {
        walkNs(v, path + '.' + n, depth + 1);
      }
    }
  }
  // Prototype members of a class, union along the chain.
  function walkProto(proto, path) {
    var p = proto, hops = 0, done = {};
    while (p && !isStopProto(p) && hops < 8) {
      var ns = names(p);
      for (var i = 0; i < ns.length; i++) {
        var n = ns[i]; if (SKIP[n] || done[n]) continue; done[n] = 1;
        emit(path + '#' + n + ' ' + kindOf(descOf(p, n)));
      }
      p = Object.getPrototypeOf(p); hops++;
    }
  }
  // Instance: own + prototype chain, union. Plain sub-objects one level deep.
  function walkInstance(obj, path) {
    if (!obj || (typeof obj !== 'object' && typeof obj !== 'function')) { emit(path + ' ' + typeof obj); return; }
    var done = {}, p = obj, hops = 0;
    while (p && !isStopProto(p) && hops < 8) {
      var ns = names(p);
      for (var i = 0; i < ns.length; i++) {
        var n = ns[i]; if (SKIP[n] || done[n] || /^\d+$/.test(n)) continue; done[n] = 1;
        var d = descOf(p, n);
        emit(path + '#' + n + ' ' + kindOf(d));
      }
      p = Object.getPrototypeOf(p); hops++;
    }
  }
  var R = {};   // label -> constructed instance, for the resolve pass
  function tryMake(label, fn) {
    var v;
    try { v = fn(); } catch (e) { emit(label + ' !construct ' + String(e && e.message || e).replace(/\s+/g, ' ').slice(0, 80)); return null; }
    if (v === undefined || v === null) { emit(label + ' !null'); return null; }
    R[label] = v;
    walkInstance(v, label); return v;
  }

  // Resolve pass: when the host prepends `var __SURF_PATHS = [...]` (the paths
  // of another runtime's dump), look each one up by NAME instead of by
  // enumeration and emit `RES <path> <kind>`. bronze resolves host globals and
  // builtin prototype members on lookup but does not enumerate them, so a
  // name-driven probe is the fair comparison.
  function findDesc(obj, n) {
    var p = obj, hops = 0;
    while (p && hops < 12) { var d = descOf(p, n); if (d) return d; p = Object.getPrototypeOf(p); hops++; }
    return null;
  }
  function kindOfValue(v) {
    var t = typeof v;
    if (t === 'function') return 'fn ' + v.length;
    if (t === 'object') return v === null ? 'null' : 'obj';
    return t;
  }
  function resolve(path) {
    var toks = path.match(/[.#][^.#]+|^[^.#]+/g);
    if (!toks) return '!parse';
    var root = toks[0], obj;
    if (root === 'global') obj = globalThis;
    else if (root === 'inst' || root === 'mod') {
      // longest registered label prefix
      var best = null;
      for (var k in R) if (path === k || path.indexOf(k + '.') === 0 || path.indexOf(k + '#') === 0) if (!best || k.length > best.length) best = k;
      if (!best) return '!noinst';
      obj = R[best];
      toks = (path.slice(best.length)).match(/[.#][^.#]+/g) || [];
      if (!toks.length) return kindOfValue(obj);
      toks.unshift('');
    } else return '!root';
    for (var i = 1; i < toks.length; i++) {
      var t = toks[i], sep = t.charAt(0), n = t.slice(1), last = i === toks.length - 1;
      if (obj === null || obj === undefined) return '!chain';
      var holder = obj;
      if (sep === '#' && typeof obj === 'function' && obj.prototype && !(n in obj)) holder = obj.prototype;
      var d = findDesc(holder, n);
      if (!d) {
        var has = false; try { has = (n in holder); } catch (e) {}
        if (!has) return '!missing';
        var v; try { v = holder[n]; } catch (e) { return '!throw'; }
        if (last) return kindOfValue(v);
        obj = v; continue;
      }
      if (last) return kindOf(d);
      if (d.get || d.set) { try { obj = holder[n]; } catch (e) { return '!throw'; } }
      else obj = d.value;
    }
    return '!end';
  }
  function resolveAll() {
    var ps = globalThis.__SURF_PATHS;
    if (!ps) return;
    for (var i = 0; i < ps.length; i++) {
      var r; try { r = resolve(ps[i]); } catch (e) { r = '!err ' + String(e && e.message || e).slice(0, 40); }
      console.log('RES ' + ps[i] + ' ' + r);
    }
  }

  // ---- roots ----
  walkNs(globalThis, 'global', 0);
  var bro = globalThis.bro;
  // globals that are instances rather than classes
  var instGlobals = ['document', 'navigator', 'screen', 'localStorage', 'sessionStorage', 'performance', 'crypto', 'console', 'process', 'history', 'location', 'indexedDB', 'caches', 'speechSynthesis'];
  for (var g = 0; g < instGlobals.length; g++) { var gn = instGlobals[g]; if (globalThis[gn]) { R['inst.' + gn] = globalThis[gn]; walkInstance(globalThis[gn], 'inst.' + gn); } }

  // web instances
  tryMake('inst.AudioContext', function () { return new AudioContext(); });
  var canvas = tryMake('inst.canvas', function () { var c = document.createElement('canvas'); c.width = 8; c.height = 8; document.body.appendChild(c); return c; });
  if (canvas) {
    var c2 = tryMake('inst.ctx2d', function () { return canvas.getContext('2d'); });
    if (c2) {
      tryMake('inst.ctx2d.imageData', function () { return c2.createImageData(2, 2); });
      tryMake('inst.ctx2d.gradient', function () { return c2.createLinearGradient(0, 0, 1, 1); });
      tryMake('inst.ctx2d.textMetrics', function () { return c2.measureText('x'); });
    }
    var gl = tryMake('inst.webgl2', function () { var c = document.createElement('canvas'); document.body.appendChild(c); return c.getContext('webgl2'); });
    var scene = tryMake('inst.scene', function () { var c = document.createElement('canvas'); document.body.appendChild(c); return c.getContext('scene'); });
    if (scene) {
      var mk = ['createBox', 'createSphere', 'createNode', 'createGroup', 'createSprite', 'createLight', 'createTerrain', 'createTileWorld', 'createAnimationPlayer', 'createGaussianSplat', 'createMesh', 'createParticleSystem', 'createCamera', 'createClipmapTerrain', 'createText', 'createLine', 'createInstancedMesh', 'createPlane', 'createCylinder', 'createCapsule', 'createCone', 'createTorus', 'createSkybox', 'createWater', 'createVehicle', 'createCharacter', 'createDecal', 'createTrail', 'createBillboard'];
      for (var m = 0; m < mk.length; m++) (function (name) {
        if (typeof scene[name] !== 'function') return;
        tryMake('inst.scene.' + name + '()', function () { return scene[name]({}); });
      })(mk[m]);
      tryMake('inst.scene.camera', function () { return scene.camera; });
      tryMake('inst.scene.physics', function () { return scene.physics; });
      tryMake('inst.scene.lighting', function () { return scene.lighting; });
    }
  }
  var els = ['div', 'span', 'input', 'select', 'option', 'textarea', 'button', 'img', 'video', 'audio', 'a', 'form', 'table', 'iframe', 'template', 'label', 'details', 'svg', 'style', 'script', 'progress', 'meter', 'dialog', 'slot'];
  for (var e = 0; e < els.length; e++) (function (tag) {
    tryMake('inst.el.' + tag, function () { var x = document.createElement(tag); document.body.appendChild(x); return x; });
  })(els[e]);
  tryMake('inst.el.div.style', function () { return document.createElement('div').style; });
  tryMake('inst.el.div.classList', function () { return document.createElement('div').classList; });
  tryMake('inst.el.div.dataset', function () { return document.createElement('div').dataset; });
  tryMake('inst.el.div.attributes', function () { return document.createElement('div').attributes; });
  tryMake('inst.el.div.children', function () { return document.createElement('div').children; });
  tryMake('inst.el.div.childNodes', function () { return document.createElement('div').childNodes; });
  tryMake('inst.el.div.getBoundingClientRect', function () { return document.createElement('div').getBoundingClientRect(); });
  tryMake('inst.textNode', function () { return document.createTextNode('t'); });
  tryMake('inst.documentFragment', function () { return document.createDocumentFragment(); });
  tryMake('inst.range', function () { return document.createRange(); });
  tryMake('inst.selection', function () { return getSelection(); });
  tryMake('inst.computedStyle', function () { return getComputedStyle(document.body); });
  tryMake('inst.shadowRoot', function () { var d = document.createElement('div'); return d.attachShadow({ mode: 'open' }); });
  tryMake('inst.matchMedia', function () { return matchMedia('(min-width: 1px)'); });
  var ctors = [['Event', ['e']], ['CustomEvent', ['e', { detail: 1 }]], ['KeyboardEvent', ['keydown']], ['MouseEvent', ['click']], ['PointerEvent', ['pointerdown']], ['WheelEvent', ['wheel']], ['TouchEvent', ['touchstart']], ['InputEvent', ['input']], ['FocusEvent', ['focus']], ['DragEvent', ['drop']], ['MessageEvent', ['message']], ['ErrorEvent', ['error']], ['ProgressEvent', ['progress']], ['GamepadEvent', ['gamepadconnected']], ['AnimationEvent', ['animationend']], ['TransitionEvent', ['transitionend']],
    ['URL', ['http://h/p?q=1#f']], ['URLSearchParams', ['a=1']], ['Blob', [['a']]], ['File', [['a'], 'f.txt']], ['FileReader', []], ['TextEncoder', []], ['TextDecoder', []], ['AbortController', []], ['Headers', []], ['Request', ['http://h/']], ['Response', ['x']], ['ImageData', [2, 2]], ['DOMParser', []], ['XMLSerializer', []], ['XMLHttpRequest', []], ['DOMMatrix', []], ['DOMPoint', []], ['DOMRect', []], ['Path2D', []], ['Image', []], ['Audio', []], ['Option', []], ['MutationObserver', [function () {}]], ['ResizeObserver', [function () {}]], ['IntersectionObserver', [function () {}]], ['PerformanceObserver', [function () {}]], ['EventTarget', []], ['MessageChannel', []], ['BroadcastChannel', ['c']], ['WebSocket', ['ws://127.0.0.1:1']], ['Worker', ['data:,']], ['OffscreenCanvas', [4, 4]], ['ReadableStream', []], ['WritableStream', []], ['TransformStream', []], ['CompressionStream', ['gzip']], ['Notification', ['n']], ['SpeechSynthesisUtterance', ['x']], ['DataTransfer', []], ['FormData', []], ['Intl.DateTimeFormat', []], ['Intl.NumberFormat', []], ['Intl.Collator', []], ['Intl.PluralRules', []], ['Intl.RelativeTimeFormat', []], ['Intl.ListFormat', []], ['Intl.Segmenter', []], ['Intl.DisplayNames', ['en', { type: 'region' }]]];
  for (var c = 0; c < ctors.length; c++) (function (name, args) {
    var parts = name.split('.'), C = globalThis;
    for (var i = 0; i < parts.length && C; i++) C = C[parts[i]];
    if (typeof C !== 'function') { emit('inst.' + name + ' !missing'); return; }
    tryMake('inst.' + name, function () { switch (args.length) { case 0: return new C(); case 1: return new C(args[0]); default: return new C(args[0], args[1]); } });
  })(ctors[c][0], ctors[c][1]);
  var mods = ['fs', 'path', 'os', 'child_process', 'events', 'util', 'buffer', 'stream', 'crypto', 'net', 'dgram', 'http', 'zlib', 'url', 'querystring', 'assert', 'timers', 'process', 'worker_threads', 'readline', 'string_decoder', 'fs/promises'];
  if (typeof require === 'function') for (var r = 0; r < mods.length; r++) (function (mname) {
    tryMake('mod.' + mname, function () { return require(mname); });
  })(mods[r]);
  if (bro && bro.math) {
    tryMake('inst.bro.math.SpatialHash3D', function () { return new bro.math.SpatialHash3D(1); });
  }
  if (bro && bro.image) tryMake('inst.bro.image.gpu', function () { return bro.image.gpu; });
  resolveAll();
  emit('END');
})();
