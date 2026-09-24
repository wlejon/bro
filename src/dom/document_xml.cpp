// XML in, tree out: DOMParser's application/xml, text/xml,
// application/xhtml+xml and image/svg+xml types.
//
// The HTML parser is the wrong tool for those — it folds names to lower
// case, treats `<b/>` as an open tag, invents <html>/<head>/<body>, and never
// fails — so this is a small well-formedness-checking XML 1.0 parser:
// elements, attributes, namespaces (xmlns / xmlns:p declarations, prefixes
// resolved per element), text with the five predefined entities and
// character references, CDATA sections, comments. The prolog's XML
// declaration, processing instructions and a DOCTYPE (internal subset
// included) are skipped: bro's DOM has no nodes for them, and no DTD
// validation or custom entity is supported (a reference to one is the
// well-formedness error it would be without its declaration).
//
// On the first error the partial tree is discarded and the document becomes
// what Firefox returns: a lone <parsererror> root in the Mozilla parsererror
// namespace, holding the message with its line and column, plus a
// <sourcetext> child with the offending line.

#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace bro::dom {

namespace {

constexpr const char* kParserErrorNs = "http://www.mozilla.org/newlayout/xml/parsererror.xml";
constexpr const char* kXmlNs = "http://www.w3.org/XML/1998/namespace";

struct XmlError {
    std::string message;
    size_t pos = 0;
};

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isNameStart(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80;
}
bool isNameChar(unsigned char c) {
    return isNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}
bool isXmlSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// One in-scope namespace binding: prefix ("" = default) to URI.
struct Binding {
    std::string prefix;
    std::string uri;
};

class XmlParser {
public:
    XmlParser(Document& doc, const std::string& src) : doc_(doc), s_(src) {}

    // The root element, or nullptr with `error` set.
    Element* run() {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) p_ = 3;
        if (!misc(true)) return nullptr;
        if (p_ >= s_.size() || s_[p_] != '<') return fail("no root element");
        Element* root = element();
        if (!root) return nullptr;
        if (!misc(false)) return nullptr;
        if (p_ < s_.size()) return fail("junk after the document element");
        return root;
    }

    XmlError error;

private:
    Document& doc_;
    const std::string& s_;
    size_t p_ = 0;
    std::vector<Binding> scope_;
    bool failed_ = false;

    std::nullptr_t fail(const std::string& msg) {
        if (!failed_) {
            failed_ = true;
            error.message = msg;
            error.pos = p_;
        }
        return nullptr;
    }

    bool startsWith(const char* lit) const { return s_.compare(p_, std::strlen(lit), lit) == 0; }
    void skipSpace() { while (p_ < s_.size() && isXmlSpace(s_[p_])) ++p_; }

    // Comments, PIs and whitespace around the root; the DOCTYPE only before
    // it. Comments at this level are dropped (bro's Document holds one root).
    bool misc(bool prolog) {
        while (true) {
            skipSpace();
            if (startsWith("<?")) {
                size_t end = s_.find("?>", p_ + 2);
                if (end == std::string::npos) { fail("unclosed processing instruction"); return false; }
                p_ = end + 2;
            } else if (startsWith("<!--")) {
                size_t end = s_.find("-->", p_ + 4);
                if (end == std::string::npos) { fail("unclosed comment"); return false; }
                p_ = end + 3;
            } else if (prolog && startsWith("<!DOCTYPE")) {
                if (!doctype()) return false;
            } else {
                if (p_ < s_.size() && s_[p_] != '<') {
                    fail(prolog ? "text before the document element" : "junk after the document element");
                    return false;
                }
                return true;
            }
        }
    }

    bool doctype() {
        int bracket = 0;
        char quote = 0;
        for (++p_; p_ < s_.size(); ++p_) {
            char c = s_[p_];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '[') ++bracket;
            else if (c == ']') --bracket;
            else if (c == '>' && bracket <= 0) { ++p_; return true; }
        }
        fail("unclosed DOCTYPE");
        return false;
    }

    bool name(std::string& out) {
        if (p_ >= s_.size() || !isNameStart(static_cast<unsigned char>(s_[p_]))) {
            fail("expected a name");
            return false;
        }
        size_t start = p_;
        while (p_ < s_.size() && isNameChar(static_cast<unsigned char>(s_[p_]))) ++p_;
        out.assign(s_, start, p_ - start);
        return true;
    }

    // An entity or character reference at p_ ('&'), decoded onto `out`.
    bool reference(std::string& out) {
        size_t semi = s_.find(';', p_);
        if (semi == std::string::npos || semi - p_ > 12) { fail("unescaped '&'"); return false; }
        std::string ref = s_.substr(p_ + 1, semi - p_ - 1);
        if (!ref.empty() && ref[0] == '#') {
            uint32_t cp = 0;
            bool hex = ref.size() > 1 && ref[1] == 'x';
            size_t i = hex ? 2 : 1;
            if (i >= ref.size()) { fail("empty character reference"); return false; }
            for (; i < ref.size(); ++i) {
                char c = ref[i];
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else { fail("bad character reference"); return false; }
                cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
                if (cp > 0x10FFFF) { fail("character reference out of range"); return false; }
            }
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) { fail("invalid character reference"); return false; }
            appendUtf8(out, cp);
        } else if (ref == "lt") out += '<';
        else if (ref == "gt") out += '>';
        else if (ref == "amp") out += '&';
        else if (ref == "quot") out += '"';
        else if (ref == "apos") out += '\'';
        else { fail("undefined entity &" + ref + ";"); return false; }
        p_ = semi + 1;
        return true;
    }

    const std::string* lookup(const std::string& prefix) const {
        for (size_t i = scope_.size(); i-- > 0;)
            if (scope_[i].prefix == prefix) return &scope_[i].uri;
        return nullptr;
    }

    Element* element() {
        ++p_;  // '<'
        std::string qname;
        if (!name(qname)) return nullptr;

        std::vector<std::pair<std::string, std::string>> attrs;
        const size_t scopeMark = scope_.size();
        while (true) {
            size_t before = p_;
            skipSpace();
            if (p_ >= s_.size()) return fail("unclosed start tag <" + qname + ">");
            if (s_[p_] == '>' || startsWith("/>")) break;
            if (p_ == before) return fail("expected whitespace between attributes");
            std::string an;
            if (!name(an)) return nullptr;
            skipSpace();
            if (p_ >= s_.size() || s_[p_] != '=') return fail("attribute " + an + " has no value");
            ++p_;
            skipSpace();
            if (p_ >= s_.size() || (s_[p_] != '"' && s_[p_] != '\''))
                return fail("attribute value of " + an + " is not quoted");
            const char q = s_[p_++];
            std::string val;
            while (true) {
                if (p_ >= s_.size()) return fail("unclosed attribute value");
                char c = s_[p_];
                if (c == q) { ++p_; break; }
                if (c == '<') return fail("'<' in attribute value");
                if (c == '&') { if (!reference(val)) return nullptr; continue; }
                // Attribute-value normalisation: white space becomes a space.
                val += isXmlSpace(c) ? ' ' : c;
                ++p_;
            }
            for (const auto& a : attrs)
                if (a.first == an) return fail("duplicate attribute " + an);
            if (an == "xmlns") scope_.push_back({"", val});
            else if (an.compare(0, 6, "xmlns:") == 0) {
                if (val.empty()) return fail("empty namespace for prefix " + an.substr(6));
                scope_.push_back({an.substr(6), val});
            }
            attrs.emplace_back(std::move(an), std::move(val));
        }

        // Resolve the element's namespace from the bindings now in scope.
        std::string prefix, local = qname;
        if (size_t colon = qname.find(':'); colon != std::string::npos) {
            prefix = qname.substr(0, colon);
            local = qname.substr(colon + 1);
            if (prefix.empty() || local.empty()) return fail("malformed name " + qname);
        }
        std::string uri;
        if (prefix == "xml") uri = kXmlNs;
        else if (const std::string* u = lookup(prefix)) uri = *u;
        else if (!prefix.empty()) return fail("unbound prefix " + prefix + ":");
        for (const auto& a : attrs) {
            size_t colon = a.first.find(':');
            if (colon == std::string::npos) continue;
            std::string ap = a.first.substr(0, colon);
            if (ap != "xml" && ap != "xmlns" && !lookup(ap)) return fail("unbound prefix " + ap + ":");
        }

        Element* el = doc_.createElement(lowerAscii(local));
        el->setNamespaceURI(uri);
        el->setQualifiedName(qname);
        for (auto& a : attrs) el->setAttribute(a.first, a.second);

        if (startsWith("/>")) {
            p_ += 2;
            scope_.resize(scopeMark);
            return el;
        }
        ++p_;  // '>'

        std::string text;
        auto flushText = [&]() {
            if (text.empty()) return;
            el->appendChild(doc_.createTextNode(text));
            text.clear();
        };
        while (true) {
            if (p_ >= s_.size()) return fail("unclosed element <" + qname + ">");
            char c = s_[p_];
            if (c == '<') {
                if (startsWith("</")) {
                    p_ += 2;
                    std::string closing;
                    if (!name(closing)) return nullptr;
                    if (closing != qname)
                        return fail("mismatched tag: expected </" + qname + ">, got </" + closing + ">");
                    skipSpace();
                    if (p_ >= s_.size() || s_[p_] != '>') return fail("unclosed end tag </" + closing);
                    ++p_;
                    flushText();
                    scope_.resize(scopeMark);
                    return el;
                }
                if (startsWith("<!--")) {
                    size_t end = s_.find("-->", p_ + 4);
                    if (end == std::string::npos) return fail("unclosed comment");
                    flushText();
                    el->appendChild(doc_.createComment(s_.substr(p_ + 4, end - p_ - 4)));
                    p_ = end + 3;
                    continue;
                }
                if (startsWith("<![CDATA[")) {
                    size_t end = s_.find("]]>", p_ + 9);
                    if (end == std::string::npos) return fail("unclosed CDATA section");
                    text.append(s_, p_ + 9, end - p_ - 9);
                    p_ = end + 3;
                    continue;
                }
                if (startsWith("<?")) {
                    size_t end = s_.find("?>", p_ + 2);
                    if (end == std::string::npos) return fail("unclosed processing instruction");
                    p_ = end + 2;
                    continue;
                }
                if (startsWith("<!")) return fail("unexpected markup declaration");
                flushText();
                Element* child = element();
                if (!child) return nullptr;
                el->appendChild(child);
                continue;
            }
            if (c == '&') { if (!reference(text)) return nullptr; continue; }
            if (startsWith("]]>")) return fail("']]>' in text");
            // Line ends normalise to \n.
            if (c == '\r') {
                text += '\n';
                ++p_;
                if (p_ < s_.size() && s_[p_] == '\n') ++p_;
                continue;
            }
            text += c;
            ++p_;
        }
    }
};

}  // namespace

bool Document::parseXml(const std::string& xml) {
    XmlParser parser(*this, xml);
    Element* root = parser.run();
    if (root) {
        root_ = root;
        documentElement_ = root;
        std::vector<Element*> all;
        collectElements(root_, all);
        for (Element* e : all) registerElementId(e->id(), e);
        dirty_ = false;
        return true;
    }

    // Line and column of the error, and the source line it sits on.
    size_t line = 1, lineStart = 0;
    const size_t at = std::min(parser.error.pos, xml.size());
    for (size_t i = 0; i < at; ++i) {
        if (xml[i] == '\n') { ++line; lineStart = i + 1; }
    }
    size_t lineEnd = xml.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = xml.size();

    Element* err = createElement("parsererror");
    err->setNamespaceURI(kParserErrorNs);
    err->setQualifiedName("parsererror");
    err->appendChild(createTextNode("XML Parsing Error: " + parser.error.message +
                                    "\nLine Number " + std::to_string(line) +
                                    ", Column " + std::to_string(at - lineStart + 1) + ":"));
    Element* src = createElement("sourcetext");
    src->setNamespaceURI(kParserErrorNs);
    src->setQualifiedName("sourcetext");
    src->appendChild(createTextNode(xml.substr(lineStart, lineEnd - lineStart)));
    err->appendChild(src);
    root_ = err;
    documentElement_ = err;
    dirty_ = false;
    return false;
}

}  // namespace bro::dom
