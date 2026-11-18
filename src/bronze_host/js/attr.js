// Attr and NamedNodeMap, as JavaScript over the element's attribute methods:
// element.attributes (a live NamedNodeMap: index and name access, item,
// getNamedItem(NS), setNamedItem(NS), removeNamedItem(NS), iteration),
// getAttributeNode(NS) / setAttributeNode(NS) / removeAttributeNode, and
// document.createAttribute(NS). An Attr is one object per attribute of an
// element for as long as the attribute exists (attributes[0] ===
// getAttributeNode(name)); its value, nodeValue and textContent read and write
// the attribute; ownerElement is its element, or null for one created by
// createAttribute or removed.
(function () {
    'use strict';

    const g = globalThis;
    const ElementProto = g.Element && g.Element.prototype;
    if (!ElementProto) return;
    const getAttr = ElementProto.getAttribute;
    const setAttr = ElementProto.setAttribute;
    const hasAttr = ElementProto.hasAttribute;
    const removeAttr = ElementProto.removeAttribute;
    const getAttrNames = ElementProto.getAttributeNames;
    const getAttrNS = ElementProto.getAttributeNS;
    const setAttrNS = ElementProto.setAttributeNS;
    const removeAttrNS = ElementProto.removeAttributeNS;

    function domException(message, name) {
        return typeof g.DOMException === 'function' ? new g.DOMException(message, name) : new Error(message);
    }

    // element -> Map(qualified name -> Attr)
    const nodes = new WeakMap();

    class Attr {
        constructor() { throw new TypeError('Illegal constructor'); }
        get nodeType() { return 2; }
        get nodeName() { return this._name; }
        get name() { return this._name; }
        get localName() { const i = this._name.indexOf(':'); return this._ns && i >= 0 ? this._name.slice(i + 1) : this._name; }
        get prefix() { const i = this._name.indexOf(':'); return this._ns && i >= 0 ? this._name.slice(0, i) : null; }
        get namespaceURI() { return this._ns; }
        get ownerElement() { return this._owner; }
        get ownerDocument() { return this._owner ? this._owner.ownerDocument : this._doc; }
        get specified() { return true; }
        get value() {
            if (this._owner) {
                const v = getAttr.call(this._owner, this._name);
                if (v !== null) this._value = v;
            }
            return this._value;
        }
        set value(v) {
            v = String(v);
            this._value = v;
            if (this._owner) setAttr.call(this._owner, this._name, v);
        }
        get nodeValue() { return this.value; }
        set nodeValue(v) { this.value = v === null ? '' : v; }
        get textContent() { return this.value; }
        set textContent(v) { this.value = v === null ? '' : v; }
        get parentNode() { return null; }
        get parentElement() { return null; }
        get childNodes() { return []; }
        get firstChild() { return null; }
        get lastChild() { return null; }
        get previousSibling() { return null; }
        get nextSibling() { return null; }
        get isConnected() { return !!(this._owner && this._owner.isConnected); }
        hasChildNodes() { return false; }
        cloneNode() { return makeAttr(null, this._name, this._ns, this.value, this.ownerDocument); }
        isEqualNode(o) { return !!o && o.nodeType === 2 && o.name === this.name && o.namespaceURI === this.namespaceURI && o.value === this.value; }
        isSameNode(o) { return o === this; }
        getRootNode() { return this; }
        contains(o) { return o === this; }
        lookupNamespaceURI() { return null; }
    }

    if (g.Node && g.Node.prototype) Object.setPrototypeOf(Attr.prototype, g.Node.prototype);

    function makeAttr(owner, name, ns, value, doc) {
        const a = Object.create(Attr.prototype);
        a._owner = owner;
        a._name = name;
        a._ns = ns || null;
        a._value = value;
        a._doc = doc || null;
        return a;
    }

    function attrFor(el, name) {
        let m = nodes.get(el);
        if (!m) nodes.set(el, m = new Map());
        let a = m.get(name);
        if (!a) {
            a = makeAttr(el, name, name.startsWith('xlink:') ? 'http://www.w3.org/1999/xlink' : null, getAttr.call(el, name), null);
            m.set(name, a);
        }
        return a;
    }

    function getNode(el, name) {
        name = String(name);
        if (!hasAttr.call(el, name)) return null;
        // HTML attribute names are lowercase; the map is keyed by the name as stored.
        const names = getAttrNames.call(el);
        const stored = names.includes(name) ? name : names.find((n) => n.toLowerCase() === name.toLowerCase()) || name;
        return attrFor(el, stored);
    }

    function detach(el, name) {
        const m = nodes.get(el);
        const a = m && m.get(name);
        if (a) {
            a._value = getAttr.call(el, name) ?? a._value;
            a._owner = null;
            m.delete(name);
        }
        return a || null;
    }

    function setNode(el, attr) {
        if (!(attr instanceof Attr)) throw new TypeError("Failed to execute 'setAttributeNode' on 'Element': parameter 1 is not of type 'Attr'.");
        if (attr._owner && attr._owner !== el)
            throw domException("Failed to execute 'setAttributeNode' on 'Element': The node provided is an attribute node that is already an attribute of another Element; attribute nodes must be explicitly cloned.", 'InUseAttributeError');
        if (attr._owner === el) return attr;
        const old = hasAttr.call(el, attr._name) ? detach(el, attr._name) || makeAttr(null, attr._name, attr._ns, getAttr.call(el, attr._name), null) : null;
        const value = attr._value;
        if (attr._ns) setAttrNS.call(el, attr._ns, attr._name, value);
        else setAttr.call(el, attr._name, value);
        attr._owner = el;
        let m = nodes.get(el);
        if (!m) nodes.set(el, m = new Map());
        m.set(attr._name, attr);
        return old;
    }

    function removeNode(el, attr) {
        if (!(attr instanceof Attr) || attr._owner !== el)
            throw domException("Failed to execute 'removeAttributeNode' on 'Element': The node provided is owned by another element.", 'NotFoundError');
        const name = attr._name;
        detach(el, name);
        removeAttr.call(el, name);
        return attr;
    }

    class NamedNodeMap {
        constructor() { throw new TypeError('Illegal constructor'); }
        get length() { return getAttrNames.call(this._el).length; }
        item(i) {
            const n = getAttrNames.call(this._el)[Number(i) >>> 0];
            return n === undefined ? null : attrFor(this._el, n);
        }
        getNamedItem(name) { return getNode(this._el, name); }
        getNamedItemNS(ns, local) {
            const v = getAttrNS ? getAttrNS.call(this._el, ns, local) : null;
            if (v === null) return null;
            const n = getAttrNames.call(this._el).find((x) => x === local || x.endsWith(':' + local));
            return n ? attrFor(this._el, n) : null;
        }
        setNamedItem(attr) { return setNode(this._el, attr); }
        setNamedItemNS(attr) { return setNode(this._el, attr); }
        removeNamedItem(name) {
            const a = getNode(this._el, name);
            if (!a) throw domException("Failed to execute 'removeNamedItem' on 'NamedNodeMap': No item with name '" + name + "' was found.", 'NotFoundError');
            return removeNode(this._el, a);
        }
        removeNamedItemNS(ns, local) {
            const a = this.getNamedItemNS(ns, local);
            if (!a) throw domException("Failed to execute 'removeNamedItemNS' on 'NamedNodeMap': No item with namespace '" + ns + "' and local name '" + local + "' was found.", 'NotFoundError');
            if (removeAttrNS) { detach(this._el, a._name); removeAttrNS.call(this._el, ns, local); return a; }
            return removeNode(this._el, a);
        }
        *[Symbol.iterator]() {
            const names = getAttrNames.call(this._el);
            for (const n of names) yield attrFor(this._el, n);
        }
    }

    const maps = new WeakMap();
    function mapFor(el) {
        let m = maps.get(el);
        if (m) return m;
        const target = Object.create(NamedNodeMap.prototype);
        Object.defineProperty(target, '_el', { value: el });
        m = new Proxy(target, {
            get(t, key, receiver) {
                if (typeof key === 'string') {
                    if (/^\d+$/.test(key)) return t.item(Number(key)) || undefined;
                    if (!(key in t) && hasAttr.call(el, key)) return getNode(el, key);
                }
                return Reflect.get(t, key, receiver);
            },
            has(t, key) {
                if (typeof key === 'string' && /^\d+$/.test(key)) return Number(key) < t.length;
                return Reflect.has(t, key);
            },
            ownKeys(t) {
                const keys = getAttrNames.call(el).map((_, i) => String(i));
                return keys.concat(Reflect.ownKeys(t));
            },
            getOwnPropertyDescriptor(t, key) {
                if (typeof key === 'string' && /^\d+$/.test(key) && Number(key) < t.length)
                    return { value: t.item(Number(key)), writable: false, enumerable: true, configurable: true };
                return Reflect.getOwnPropertyDescriptor(t, key);
            },
        });
        maps.set(el, m);
        return m;
    }

    // removeAttribute and a setAttribute that changes nothing for the Attr
    // except its value keep the Attr objects right: a removed attribute's Attr
    // is detached (ownerElement null, its last value kept).
    function wrapRemove(name, fn, nameOf) {
        if (typeof fn !== 'function') return;
        Object.defineProperty(ElementProto, name, {
            value: function () {
                const n = nameOf(this, arguments);
                if (n !== null && nodes.has(this)) detach(this, n);
                return fn.apply(this, arguments);
            },
            writable: true, configurable: true, enumerable: true,
        });
    }
    wrapRemove('removeAttribute', removeAttr, (el, a) => {
        const name = String(a[0]);
        const names = getAttrNames.call(el);
        return names.includes(name) ? name : names.find((n) => n.toLowerCase() === name.toLowerCase()) || null;
    });

    function def(proto, name, fn) {
        Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true, enumerable: true });
    }
    Object.defineProperty(ElementProto, 'attributes', { get() { return mapFor(this); }, configurable: true, enumerable: true });
    def(ElementProto, 'getAttributeNode', function (name) { return getNode(this, name); });
    def(ElementProto, 'getAttributeNodeNS', function (ns, local) { return mapFor(this).getNamedItemNS(ns, local); });
    def(ElementProto, 'setAttributeNode', function (attr) { return setNode(this, attr); });
    def(ElementProto, 'setAttributeNodeNS', function (attr) { return setNode(this, attr); });
    def(ElementProto, 'removeAttributeNode', function (attr) { return removeNode(this, attr); });

    const docProto = g.Document && g.Document.prototype;
    const docs = [docProto, g.document].filter((d) => d);
    for (const d of docs) {
        def(d, 'createAttribute', function (name) {
            name = String(name);
            if (!/^[^\s"'>/=\0]+$/.test(name))
                throw domException("Failed to execute 'createAttribute' on 'Document': The localName provided ('" + name + "') contains an invalid character.", 'InvalidCharacterError');
            return makeAttr(null, name.toLowerCase(), null, '', this);
        });
        def(d, 'createAttributeNS', function (ns, qname) {
            return makeAttr(null, String(qname), ns === null || ns === undefined || ns === '' ? null : String(ns), '', this);
        });
    }

    g.Attr = Attr;
    g.NamedNodeMap = NamedNodeMap;
})();
