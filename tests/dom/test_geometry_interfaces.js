// DOMRect, DOMPoint, DOMMatrix, DOMQuad and the legacy SVG geometry types
// (js/geometry.js), with the values Chromium gives for the same calls.
const eq = (got, want, what) => assert(got === want, `${what}: got ${got}, want ${want}`);
const tag = (v) => Object.prototype.toString.call(v);

// DOMRect
const r = new DOMRect(1, 2, 3, 4);
eq([r.x, r.y, r.width, r.height, r.left, r.top, r.right, r.bottom].join(), '1,2,3,4,1,2,4,6', 'rect members');
eq(JSON.stringify(r), '{"x":1,"y":2,"width":3,"height":4,"top":2,"right":4,"bottom":6,"left":1}', 'rect toJSON');
eq(Object.keys(r).length, 0, 'a rect has no own keys');
eq(JSON.stringify(new DOMRect(5, 5, -3, -4)), '{"x":5,"y":5,"width":-3,"height":-4,"top":1,"right":5,"bottom":5,"left":2}', 'negative size');
assert(DOMRect.prototype instanceof DOMRectReadOnly, 'DOMRect extends DOMRectReadOnly');
r.width = 10;
eq(r.right, 11, 'a DOMRect is writable');
const ro = new DOMRectReadOnly(1, 1, 1, 1);
ro.x = 9;
eq(ro.x, 1, 'a DOMRectReadOnly is not');
eq(JSON.stringify(DOMRect.fromRect({ x: 1, width: 2 })), '{"x":1,"y":0,"width":2,"height":0,"top":0,"right":3,"bottom":0,"left":1}', 'fromRect');
eq(tag(r), '[object DOMRect]', 'rect tag');

// Element rects are DOMRects; getClientRects a DOMRectList.
document.body.style.margin = '0';
document.body.innerHTML = '<div id="b" style="position:absolute;left:10px;top:20px;width:30px;height:40px"></div>' +
    '<svg id="s" width="200" height="100" viewBox="0 0 400 200" style="position:absolute;left:50px;top:60px">' +
    '<g id="g" transform="translate(10,20) scale(2)"><rect id="r" x="5" y="6" width="7" height="8"/></g>' +
    '<circle id="c" cx="50" cy="50" r="10"/></svg>';
const b = document.getElementById('b');
const bcr = b.getBoundingClientRect();
assert(bcr instanceof DOMRect, 'getBoundingClientRect is a DOMRect');
eq(JSON.stringify(bcr), '{"x":10,"y":20,"width":30,"height":40,"top":20,"right":40,"bottom":60,"left":10}', 'getBoundingClientRect');
const list = b.getClientRects();
assert(list instanceof DOMRectList && list.length === 1 && list.item(0) instanceof DOMRect && list[0] === list.item(0), 'getClientRects is a DOMRectList');
eq([...list].length, 1, 'a DOMRectList iterates');

// DOMPoint
const pt = new DOMPoint(1, 2);
eq(JSON.stringify(pt), '{"x":1,"y":2,"z":0,"w":1}', 'point');
eq(JSON.stringify(pt.matrixTransform(new DOMMatrix([2, 0, 0, 2, 10, 20]))), '{"x":12,"y":24,"z":0,"w":1}', 'matrixTransform');
assert(DOMPoint.prototype instanceof DOMPointReadOnly, 'DOMPoint extends DOMPointReadOnly');
eq(JSON.stringify(DOMPoint.fromPoint({ x: 3, z: 4 })), '{"x":3,"y":0,"z":4,"w":1}', 'fromPoint');

// DOMMatrix
const f = (m) => Array.from(m.toFloat64Array()).map((x) => +x.toFixed(4)).join(',');
const id = new DOMMatrix();
eq(`${id.is2D} ${id.isIdentity} ${id}`, 'true true matrix(1, 0, 0, 1, 0, 0)', 'identity');
const m2 = new DOMMatrix([1, 2, 3, 4, 5, 6]);
eq(`${m2} ${m2.a}${m2.b}${m2.c}${m2.d}${m2.e}${m2.f} ${m2.m41}${m2.m42}`, 'matrix(1, 2, 3, 4, 5, 6) 123456 56', '2D from six');
eq(`${new DOMMatrix([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1]).is2D}`, 'false', 'sixteen is 3D');
eq(new DOMMatrix([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1]).toString(), 'matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1)', 'matrix3d string');
eq(new DOMMatrix('translate(10px, 20px) rotate(90deg) scale(2)').toString(), 'matrix(0, 2, -2, 0, 10, 20)', 'CSS transform');
eq(new DOMMatrix('translate3d(1px,2px,3px)').toString(), 'matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1)', 'CSS 3D');
eq(new DOMMatrix('none').isIdentity, true, 'none');
eq(new DOMMatrix().translate(5, 6).scale(2).rotate(30).toString(),
   'matrix(1.7320508075688774, 0.9999999999999999, -0.9999999999999999, 1.7320508075688774, 5, 6)', 'translate scale rotate');
eq(new DOMMatrix().translate(1, 2, 3).scale3d(2).rotateAxisAngle(1, 1, 0, 45).toString(),
   'matrix3d(1.7071067811865475, 0.29289321881345237, -1, 0, 0.29289321881345237, 1.7071067811865475, 1, 0, 1, -1, 1.4142135623730951, 0, 1, 2, 3, 1)', 'rotateAxisAngle');
eq(f(new DOMMatrix().rotate(10, 20, 30)), '0.8138,0.4698,-0.342,0,-0.441,0.8826,0.1632,0,0.3785,0.018,0.9254,0,0,0,0,1', 'rotate xyz');
eq(new DOMMatrix().rotateFromVector(1, 1).toString(),
   'matrix(0.7071067811865476, 0.7071067811865476, -0.7071067811865476, 0.7071067811865476, 0, 0)', 'rotateFromVector');
eq(new DOMMatrix().rotate(60).toString(),
   'matrix(0.49999999999999994, 0.8660254037844387, -0.8660254037844387, 0.49999999999999994, 0, 0)', 'rotate 60 as Chromium');
eq(new DOMMatrix().skewX(10).skewY(20).toString(),
   'matrix(1.064177772475912, 0.36397023426620234, 0.17632698070846498, 1, 0, 0)', 'skew');
eq(new DOMMatrix([1, 2, 3, 4, 5, 6]).flipX().flipY().toString(), 'matrix(-1, -2, -3, -4, 5, 6)', 'flip');
eq(new DOMMatrix([2, 0, 0, 4, 6, 8]).inverse().toString(), 'matrix(0.5, 0, 0, 0.25, -3, -2)', 'inverse');
eq(f(new DOMMatrix().translate(1, 2, 3).rotate(10, 20, 30).inverse()),
   '0.8138,-0.441,0.3785,0,0.4698,0.8826,0.018,0,-0.342,0.1632,0.9254,0,-0.7274,-1.8137,-3.1908,1', 'inverse 3D');
const sing = new DOMMatrix([0, 0, 0, 0, 0, 0]).inverse();
let singErr = null;
try { sing.toString(); } catch (e) { singErr = e.name; }
eq(`${sing.a} ${sing.is2D} ${singErr}`, 'NaN false InvalidStateError', 'a singular inverse');
eq(new DOMMatrix([1, 2, 3, 4, 5, 6]).multiply(new DOMMatrix([6, 5, 4, 3, 2, 1])).toString(), 'matrix(21, 32, 13, 20, 10, 14)', 'multiply');
const ms = new DOMMatrix([1, 2, 3, 4, 5, 6]);
ms.multiplySelf({ a: 2, d: 2 });
eq(ms.toString(), 'matrix(2, 4, 6, 8, 5, 6)', 'multiplySelf with a dictionary');
ms.preMultiplySelf(new DOMMatrix().translate(1, 1));
eq(ms.toString(), 'matrix(2, 4, 6, 8, 6, 7)', 'preMultiplySelf');
ms.invertSelf();
eq(ms.toString(), 'matrix(-1, 0.5, 0.75, -0.25, 0.75, -1.25)', 'invertSelf');
ms.translateSelf(1, 2).scaleSelf(3, 4, 1, 5, 6).rotateSelf(45).skewXSelf(5);
eq(f(ms), '0,0.3536,0,0,4.2426,-1.7368,0,0,0,0,1,0,-2.25,-1.75,0,1', 'the Self mutators');
eq(JSON.stringify(new DOMMatrix([1, 2, 3, 4, 5, 6]).transformPoint({ x: 1, y: 1 })), '{"x":9,"y":12,"z":0,"w":1}', 'transformPoint');
eq(new DOMMatrix([1, 2, 3, 4, 5, 6]).toFloat32Array().length, 16, 'toFloat32Array');
eq(DOMMatrix.fromFloat32Array(new Float32Array([1, 2, 3, 4, 5, 6])).toString(), 'matrix(1, 2, 3, 4, 5, 6)', 'fromFloat32Array');
eq(DOMMatrix.fromMatrix({ m11: 2, m22: 3, m41: 4 }).toString(), 'matrix(2, 0, 0, 3, 4, 0)', 'fromMatrix');
assert(DOMMatrix.prototype instanceof DOMMatrixReadOnly, 'DOMMatrix extends DOMMatrixReadOnly');
assert(WebKitCSSMatrix === DOMMatrix, 'WebKitCSSMatrix is DOMMatrix');
eq(JSON.stringify(new DOMMatrix([1, 2, 3, 4, 5, 6])),
   '{"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"m11":1,"m12":2,"m13":0,"m14":0,"m21":3,"m22":4,"m23":0,"m24":0,"m31":0,"m32":0,"m33":1,"m34":0,"m41":5,"m42":6,"m43":0,"m44":1,"is2D":true,"isIdentity":false}', 'matrix toJSON');
const sm = new DOMMatrix();
sm.setMatrixValue('scale(3)');
eq(sm.toString(), 'matrix(3, 0, 0, 3, 0, 0)', 'setMatrixValue');
sm.m13 = 1;
eq(sm.is2D, false, 'a 3D member set makes it 3D');
let badName = null;
try { new DOMMatrix([1, 2, 3]); } catch (e) { badName = e.name; }
eq(badName, 'TypeError', 'three numbers');
try { new DOMMatrix('foo'); } catch (e) { badName = e.name; }
eq(badName, 'SyntaxError', 'a bad transform string');
eq(JSON.stringify(DOMQuad.fromRect({ x: 1, y: 2, width: 3, height: 4 }).getBounds()),
   '{"x":1,"y":2,"width":3,"height":4,"top":2,"right":4,"bottom":6,"left":1}', 'DOMQuad');

// The canvas transform is a DOMMatrix and takes one.
const ctx = document.createElement('canvas').getContext('2d');
ctx.translate(3, 4);
ctx.scale(2, 2);
const t = ctx.getTransform();
assert(t instanceof DOMMatrix, 'getTransform is a DOMMatrix');
eq(t.toString(), 'matrix(2, 0, 0, 2, 3, 4)', 'getTransform');
ctx.setTransform(new DOMMatrix([1, 0, 0, 1, 7, 8]));
eq(ctx.getTransform().toString(), 'matrix(1, 0, 0, 1, 7, 8)', 'setTransform(DOMMatrix)');
ctx.setTransform({ a: 2, d: 2 });
eq(ctx.getTransform().toString(), 'matrix(2, 0, 0, 2, 0, 0)', 'setTransform(dictionary)');

// SVG: getBBox in user space, getCTM to the viewport, getScreenCTM to the client.
const R = (x) => [x.x, x.y, x.width, x.height].map((v) => +v.toFixed(3)).join(',');
const M = (x) => [x.a, x.b, x.c, x.d, x.e, x.f].map((v) => +v.toFixed(3)).join(',');
const rr = document.getElementById('r');
const s = document.getElementById('s');
eq(R(rr.getBBox()) + ' ' + tag(rr.getBBox()), '5,6,7,8 [object SVGRect]', 'rect getBBox');
eq(R(document.getElementById('g').getBBox()), '5,6,7,8', 'g getBBox');
eq(R(s.getBBox()), '20,32,40,28', 'svg getBBox');
eq(M(rr.getCTM()) + ' ' + tag(rr.getCTM()) + ' ' + (rr.getCTM() instanceof DOMMatrix), '1,0,0,1,5,10 [object SVGMatrix] false', 'getCTM');
eq(M(rr.getScreenCTM()), '1,0,0,1,55,70', 'getScreenCTM');
eq(M(document.getElementById('c').getCTM()), '0.5,0,0,0.5,0,0', 'circle getCTM');
eq(M(s.getCTM()), '0.5,0,0,0.5,0,0', 'svg getCTM');
eq(M(s.getScreenCTM()), '0.5,0,0,0.5,50,60', 'svg getScreenCTM');
const sp = s.createSVGPoint();
sp.x = 1;
sp.y = 1;
const tp = sp.matrixTransform(rr.getScreenCTM());
eq(`${tp.x},${tp.y} ${tag(tp)}`, '56,71 [object SVGPoint]', 'SVGPoint.matrixTransform');
eq(M(s.createSVGMatrix().translate(1, 2)), '1,0,0,1,1,2', 'createSVGMatrix');
const consolidated = document.getElementById('g').transform.baseVal.consolidate();
eq(M(consolidated.matrix), '2,0,0,2,10,20', 'transform.baseVal.consolidate');

console.log('test_geometry_interfaces: OK');
