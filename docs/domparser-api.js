/**
 * =============================================================================
 * DOMParser — HTML / XML Document Parser
 * =============================================================================
 *
 * Parses HTML markup string into a detached DOM Document tree with full
 * querySelector, getElementById, and node manipulation APIs.
 *
 * @example
 *   // Parse HTML markup into a new Document
 *   const parser = new DOMParser();
 *   const doc = parser.parseFromString(
 *     '<html><head><title>Parsed Doc</title></head><body><div id="app">Hello World</div></body></html>',
 *     'text/html'
 *   );
 *   console.log(doc.title);
 *   const el = doc.getElementById('app');
 *   if (el) {
 *     console.log(el.textContent);
 *   }
 *
 * @example
 *   // Parse SVG markup fragment
 *   const parser = new DOMParser();
 *   const svgDoc = parser.parseFromString('<svg><circle cx="50" cy="50" r="40"/></svg>', 'image/svg+xml');
 *   const circle = svgDoc.querySelector('circle');
 *   console.log('Parsed SVG node:', circle !== null);
 *
 * Other documents a script makes (src/bronze_host/host_document_node.cpp):
 * `document.implementation` is a DOMImplementation with
 * `createHTMLDocument(title)` (an HTMLDocument with `<!DOCTYPE html>`, a
 * `<title>` only when a title is given), `createDocument(ns, qualifiedName,
 * doctype)` (an XMLDocument, contentType by namespace: XHTML
 * application/xhtml+xml, SVG image/svg+xml, else application/xml; no root
 * element for an empty name), `createDocumentType(name, publicId, systemId)`
 * and `hasFeature()` (true). `new Document()` is an empty XML document and
 * `document.cloneNode(deep)` a copy of the page's. These documents have no
 * window: `defaultView` and `location` are null, `URL` is about:blank.
 *
 * A document is a node: `childNodes` / `firstChild` / `lastChild` are its
 * doctype and its element, `documentElement.parentNode === document`,
 * `doctype` is a DocumentType (`name`, `publicId`, `systemId`), and
 * appendChild / insertBefore / replaceChild / removeChild take one element
 * and one doctype before it (HierarchyRequestError otherwise). `importNode`
 * and `adoptNode` move nodes between documents (a document itself is
 * NotSupportedError). XMLSerializer writes namespace declarations, escapes
 * text and closes empty elements (`<rect/>`, `<br />` in XHTML), as Chromium.
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Interface for parsing XML or HTML source code from a string into a DOM Document.
 */
class DOMParser {

  /**
   * Initializes a new DOMParser instance.
   */
  constructor() {}

  /**
   * Parses a string containing either HTML or XML into a DOM Document.
   *
   * @param {string} str - Markup text to parse
   * @param {'text/html'|'text/xml'|'application/xml'|'application/xhtml+xml'|'image/svg+xml'} type - MIME type
   * @returns {Document} Parsed DOM Document instance
   */
  parseFromString(str, type) {}

}

/**
 * Serializes a DOM subtree into an XML/HTML markup string.
 */
class XMLSerializer {

  /**
   * Initializes a new XMLSerializer instance.
   */
  constructor() {}

  /**
   * Serializes the specified DOM node and its descendants to a string.
   *
   * @param {Node} node - Root node to serialize
   * @returns {string} Serialized markup string
   */
  serializeToString(node) {}

}


