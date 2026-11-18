#pragma once

// Markup out of a tree: the HTML fragment serialization algorithm (HTML
// §13.3, what innerHTML / outerHTML read) and the XML serialization
// algorithm (DOM Parsing §3.2, XMLSerializer), both as Chromium writes them.

#include <string>

namespace bro::dom {

class Node;
class Document;

// innerHTML: `node`'s children (a <template>'s content), HTML-serialized.
std::string serializeHtmlChildren(const Node* node);
// outerHTML: `node` itself, HTML-serialized.
std::string serializeHtmlNode(const Node* node);
// XMLSerializer.serializeToString(node): an element (with the namespace
// declarations its subtree needs), text, comment, doctype, fragment, or a
// whole document (`doc` set, `node` null).
std::string serializeXml(const Node* node, const Document* doc = nullptr);

} // namespace bro::dom
