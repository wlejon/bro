#include "dom/accessibility.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"

#include <cctype>

namespace bro::dom {

std::string lowerTagName(const Element* el) {
    if (!el) return {};
    std::string tag = el->localName();
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tag;
}

namespace {

bool attrIs(const Element* el, const char* name, const char* value) {
    return el->hasAttribute(name) && el->getAttribute(name) == value;
}

bool isHeadingTag(const std::string& t) {
    return t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6';
}

// Inline by default, for a tree with no computed style yet.
bool isInlineTag(const std::string& t) {
    static const char* const kInline[] = {"a",   "abbr", "b",    "bdi",  "bdo", "cite", "code", "data", "dfn",
                                          "em",  "i",    "kbd",  "mark", "q",   "s",    "samp", "small", "span",
                                          "strong", "sub", "sup", "time", "u",  "var",  "label"};
    for (const char* k : kInline)
        if (t == k) return true;
    return false;
}

void appendRendered(const Node* n, std::string& out, size_t maxBytes) {
    if (out.size() >= maxBytes) return;
    if (n->nodeType() == NodeType::Text) {
        out += static_cast<const TextNode*>(n)->data();
        return;
    }
    if (n->nodeType() != NodeType::Element) return;
    const auto* el = static_cast<const Element*>(n);
    if (el->hasAttribute("hidden") || attrIs(el, "aria-hidden", "true")) return;
    const auto& style = el->computedStyle();
    if (auto it = style.find("display"); it != style.end() && it->second == "none") return;
    if (auto it = style.find("visibility"); it != style.end() && it->second == "hidden") return;
    const std::string tag = lowerTagName(el);
    if (tag == "script" || tag == "style" || tag == "template") return;
    if (tag == "img") {
        if (el->hasAttribute("alt")) out += " " + el->getAttribute("alt") + " ";
        return;
    }
    // A block's text is a word apart; an inline element's runs on.
    const auto d = style.find("display");
    const std::string display = d != style.end() ? d->second : std::string();
    const bool inlineBox = display.empty() ? isInlineTag(tag) : display.rfind("inline", 0) == 0;
    if (!inlineBox) out += ' ';
    if (el->hasShadow()) {
        for (const Node* c : el->composedChildNodes()) appendRendered(c, out, maxBytes);
    } else {
        for (const Node* c : el->childNodes()) appendRendered(c, out, maxBytes);
    }
    if (!inlineBox) out += ' ';
}

std::string labelledBy(const Element* el, Document* doc) {
    if (!doc) return {};
    const std::string& ids = el->getAttribute("aria-labelledby");
    std::string out;
    size_t i = 0;
    while (i < ids.size()) {
        while (i < ids.size() && std::isspace(static_cast<unsigned char>(ids[i]))) ++i;
        size_t j = i;
        while (j < ids.size() && !std::isspace(static_cast<unsigned char>(ids[j]))) ++j;
        if (j > i) {
            if (const Element* l = doc->getElementById(ids.substr(i, j - i))) out += " " + renderedText(l);
        }
        i = j;
    }
    return collapseWhitespace(out);
}

// A form control's <label>: one naming it with for=, else one wrapping it.
std::string labelFor(const Element* el, Document* doc) {
    if (doc && !el->id().empty()) {
        std::vector<Element*> labels;
        try {
            labels = doc->querySelectorAll("label[for=\"" + el->id() + "\"]");
        } catch (...) {
        }
        for (const Element* l : labels) {
            std::string t = renderedText(l);
            if (!t.empty()) return t;
        }
    }
    for (const Element* p = el->parentElement(); p; p = p->parentElement()) {
        if (lowerTagName(p) == "label") return renderedText(p);
    }
    return {};
}

}  // namespace

std::string collapseWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += static_cast<char>(c);
    }
    return out;
}

std::string renderedText(const Element* el, size_t maxBytes) {
    if (!el) return {};
    std::string out;
    if (el->hasShadow()) {
        for (const Node* c : el->composedChildNodes()) appendRendered(c, out, maxBytes);
    } else {
        for (const Node* c : el->childNodes()) appendRendered(c, out, maxBytes);
    }
    return collapseWhitespace(out);
}

std::string accessibleRole(const Element* el) {
    if (!el) return {};
    if (el->hasAttribute("role")) {
        const std::string& r = el->getAttribute("role");
        // The first token of a role list; "presentation"/"none" strip it.
        const size_t sp = r.find(' ');
        std::string first = sp == std::string::npos ? r : r.substr(0, sp);
        if (first == "presentation" || first == "none") return {};
        if (!first.empty()) return first;
    }
    const std::string tag = lowerTagName(el);
    if (tag == "button") return "button";
    if (tag == "a") return el->hasAttribute("href") ? "link" : "";
    if (tag == "input") {
        const std::string& type = el->getAttribute("type");
        if (type == "checkbox") return "checkbox";
        if (type == "radio") return "radio";
        if (type == "range") return "slider";
        if (type == "number") return "spinbutton";
        if (type == "button" || type == "submit" || type == "reset" || type == "image") return "button";
        if (type == "hidden") return {};
        if (type == "search") return "searchbox";
        return "textbox";
    }
    if (tag == "textarea") return "textbox";
    if (tag == "select") return el->hasAttribute("multiple") ? "listbox" : "combobox";
    if (tag == "option") return "option";
    if (tag == "dialog") return "dialog";
    if (tag == "terminal") return "terminal";
    if (isHeadingTag(tag)) return "heading";
    if (tag == "ul" || tag == "ol" || tag == "menu") return "list";
    if (tag == "li") return "listitem";
    if (tag == "img") return el->hasAttribute("alt") && el->getAttribute("alt").empty() ? "" : "img";
    if (tag == "canvas") return "canvas";
    if (tag == "table") return "table";
    if (tag == "tr") return "row";
    if (tag == "td") return "cell";
    if (tag == "th") return "columnheader";
    if (tag == "nav") return "navigation";
    if (tag == "aside") return "complementary";
    if (tag == "main") return "main";
    if (tag == "header") return "banner";
    if (tag == "footer") return "contentinfo";
    if (tag == "section") return "region";
    if (tag == "form") return "form";
    if (tag == "progress") return "progressbar";
    if (tag == "meter") return "meter";
    if (tag == "output") return "status";
    if (tag == "details") return "group";
    if (tag == "summary") return "button";
    if (tag == "html" || tag == "body") return "document";
    if (attrIs(el, "contenteditable", "true") || attrIs(el, "contenteditable", "")) return "textbox";
    return {};
}

bool isInteractiveRole(const std::string& r) {
    return r == "button" || r == "link" || r == "textbox" || r == "searchbox" || r == "checkbox" || r == "radio" ||
           r == "switch" || r == "slider" || r == "spinbutton" || r == "combobox" || r == "option" ||
           r == "menuitem" || r == "menuitemcheckbox" || r == "menuitemradio" || r == "tab" || r == "treeitem";
}

bool roleNamedFromContent(const std::string& r) {
    return r == "button" || r == "link" || r == "option" || r == "menuitem" || r == "menuitemcheckbox" ||
           r == "menuitemradio" || r == "tab" || r == "treeitem" || r == "heading" || r == "cell" ||
           r == "columnheader" || r == "rowheader" || r == "listitem" || r == "checkbox" || r == "radio" ||
           r == "switch" || r == "tooltip" || r == "status" || r == "alert";
}

std::string accessibleName(const Element* el, Document* doc) { return accessibleName(el, doc, accessibleRole(el)); }

std::string accessibleName(const Element* el, Document* doc, const std::string& role) {
    if (!el) return {};
    if (el->hasAttribute("aria-labelledby")) {
        std::string n = labelledBy(el, doc);
        if (!n.empty()) return n;
    }
    if (el->hasAttribute("aria-label")) {
        std::string n = collapseWhitespace(el->getAttribute("aria-label"));
        if (!n.empty()) return n;
    }
    const std::string tag = lowerTagName(el);
    const bool control = tag == "input" || tag == "textarea" || tag == "select" || tag == "meter" ||
                         tag == "progress" || tag == "output";
    if (control) {
        const std::string& type = el->getAttribute("type");
        if (tag == "input" && (type == "button" || type == "submit" || type == "reset")) {
            std::string v = el->getAttribute("value");
            if (v.empty()) v = type == "submit" ? "Submit" : type == "reset" ? "Reset" : "";
            if (!v.empty()) return collapseWhitespace(v);
        }
        std::string n = labelFor(el, doc);
        if (!n.empty()) return n;
    }
    if (tag == "img" && el->hasAttribute("alt")) return collapseWhitespace(el->getAttribute("alt"));
    if (roleNamedFromContent(role) && !control) {
        std::string n = renderedText(el);
        if (!n.empty()) return n;
    }
    if (el->hasAttribute("title")) return collapseWhitespace(el->getAttribute("title"));
    if (el->hasAttribute("placeholder")) return collapseWhitespace(el->getAttribute("placeholder"));
    return {};
}

}  // namespace bro::dom
