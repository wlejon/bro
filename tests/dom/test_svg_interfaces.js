// The SVG element interfaces: each SVG tag is born on its own interface
// under SVGGraphicsElement / SVGGeometryElement / SVGElement, as Chromium
// shapes them, and an HTML-namespace element of the same name is not.
document.body.innerHTML = '<svg><g><path d="M0 0L1 1"/><rect/><text>t<tspan>u</tspan></text><a href="#"><circle r="1"/></a>' +
    '<title>x</title><style></style><linearGradient/><feGaussianBlur/><foo/></g></svg>';
const got = [];
document.querySelectorAll('svg, svg *').forEach((e) => {
    const p = Object.getPrototypeOf(Object.getPrototypeOf(e));
    got.push(`${e.localName}=${Object.prototype.toString.call(e).slice(8, -1)}:${p.constructor.name}:${e instanceof SVGElement}${e instanceof SVGGraphicsElement}`);
});
const want = 'svg=SVGSVGElement:SVGGraphicsElement:truetrue g=SVGGElement:SVGGraphicsElement:truetrue ' +
    'path=SVGPathElement:SVGGeometryElement:truetrue rect=SVGRectElement:SVGGeometryElement:truetrue ' +
    'text=SVGTextElement:SVGTextPositioningElement:truetrue tspan=SVGTSpanElement:SVGTextPositioningElement:truetrue ' +
    'a=SVGAElement:SVGGraphicsElement:truetrue circle=SVGCircleElement:SVGGeometryElement:truetrue ' +
    'title=SVGTitleElement:SVGElement:truefalse style=SVGStyleElement:SVGElement:truefalse ' +
    'linearGradient=SVGLinearGradientElement:SVGGradientElement:truefalse ' +
    'feGaussianBlur=SVGFEGaussianBlurElement:SVGElement:truefalse foo=SVGElement:Element:truefalse';
assert(got.join(' ') === want, 'interfaces: ' + got.join(' '));
assert(document.createElementNS('http://www.w3.org/2000/svg', 'path') instanceof SVGPathElement, 'createElementNS path');
assert(!(document.createElement('path') instanceof SVGElement), 'an HTML <path> is not an SVGElement');
assert(document.createElement('title') instanceof HTMLTitleElement, 'an HTML <title> is an HTMLTitleElement');
console.log('test_svg_interfaces: OK');
