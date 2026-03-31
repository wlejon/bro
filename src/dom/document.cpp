#include "dom/document.h"
#include <litehtml/html_tag.h>
#include <litehtml/render_item.h>
#include <algorithm>
#include <sstream>

// Helper: get mutable children list from a litehtml element.
// element::children() is const; html_tag::children() is non-const.
static std::list<litehtml::element::ptr>& mutableChildren(litehtml::element::ptr& el) {
    return static_cast<litehtml::html_tag*>(el.get())->children();
}

// Access litehtml::html_tag::m_attrs (protected) via pointer-to-member.
struct LitehtmlTagAccess : litehtml::html_tag {
    static auto attrsPtr() { return &LitehtmlTagAccess::m_attrs; }
};
static const auto kAttrsPtr = LitehtmlTagAccess::attrsPtr();


namespace bro::dom {

Document::Document() = default;
Document::~Document() = default;

void Document::parse(const std::string& html, litehtml::document_container* container) {
    // Parse with litehtml
    litehtml_doc_ = litehtml::document::createFromString(html, container);
    if (!litehtml_doc_) return;

    // Clear any existing tree
    root_ = nullptr;
    documentElement_ = nullptr;
    body_ = nullptr;
    idMap_.clear();
    litehtmlMap_.clear();
    ownedNodes_.clear();

    // Build our Element tree from the litehtml tree
    auto lh_root = litehtml_doc_->root();
    if (!lh_root) return;

    const char* rootTag = lh_root->get_tagName();
    auto* rootElem = allocateNode<Element>(rootTag ? rootTag : "html");
    rootElem->setLitehtmlElement(lh_root);
    rootElem->setDocument(this);
    litehtmlMap_[lh_root] = rootElem;
    root_ = rootElem;
    documentElement_ = rootElem;

    // Copy attributes from litehtml root
    // Build children recursively
    buildTreeFromLitehtml(lh_root, rootElem);

    // Find <body> element
    for (auto* child : rootElem->children()) {
        if (child->tagName() == "BODY") {
            body_ = child;
            break;
        }
    }

    // Register IDs
    std::vector<Element*> allElems;
    collectElements(root_, allElems);
    for (auto* elem : allElems) {
        std::string elemId = elem->id();
        if (!elemId.empty()) {
            idMap_[elemId] = elem;
        }
    }

    dirty_ = false;
}

void Document::buildFrom(litehtml::document::ptr doc) {
    litehtml_doc_ = doc;
    if (!litehtml_doc_) return;

    root_ = nullptr;
    documentElement_ = nullptr;
    body_ = nullptr;
    idMap_.clear();
    litehtmlMap_.clear();
    ownedNodes_.clear();

    auto lh_root = litehtml_doc_->root();
    if (!lh_root) return;

    const char* rootTag = lh_root->get_tagName();
    auto* rootElem = allocateNode<Element>(rootTag ? rootTag : "html");
    rootElem->setLitehtmlElement(lh_root);
    rootElem->setDocument(this);
    litehtmlMap_[lh_root] = rootElem;
    root_ = rootElem;
    documentElement_ = rootElem;

    buildTreeFromLitehtml(lh_root, rootElem);

    for (auto* child : rootElem->children()) {
        if (child->tagName() == "BODY") {
            body_ = child;
            break;
        }
    }

    std::vector<Element*> allElems;
    collectElements(root_, allElems);
    for (auto* elem : allElems) {
        std::string elemId = elem->id();
        if (!elemId.empty()) {
            idMap_[elemId] = elem;
        }
    }

    dirty_ = false;
}

void Document::reparse(litehtml::document_container* container) {
    if (!documentElement_) return;

    // Re-serialize the tree to HTML
    std::string html = documentElement_->innerHTML();

    // Re-parse
    parse(html, container);
}

Element* Document::createElement(const std::string& tag) {
    auto* elem = allocateNode<Element>(tag);
    elem->setDocument(this);
    return elem;
}

TextNode* Document::createTextNode(const std::string& text) {
    return allocateNode<TextNode>(text);
}

CommentNode* Document::createComment(const std::string& data) {
    return allocateNode<CommentNode>(data);
}

DocumentFragment* Document::createDocumentFragment() {
    return allocateNode<DocumentFragment>();
}

ShadowRoot* Document::allocateShadowRoot(Element* host, ShadowRoot::Mode mode) {
    return allocateNode<ShadowRoot>(host, mode);
}

void Document::freeNode(Node* node) {
    if (!node) return;

    // Recursively free all children first (they are also in ownedNodes_).
    // Copy the children vector since freeNode modifies ownedNodes_.
    auto kids = node->childNodes();
    for (auto* child : kids) {
        freeNode(child);
    }

    auto it = std::find_if(ownedNodes_.begin(), ownedNodes_.end(),
        [node](const std::unique_ptr<Node>& p) { return p.get() == node; });
    if (it != ownedNodes_.end()) {
        // Swap with last element then pop — O(1) instead of O(n) shift.
        std::swap(*it, ownedNodes_.back());
        ownedNodes_.pop_back();
    }
}

Element* Document::getElementById(const std::string& id) {
    auto it = idMap_.find(id);
    if (it != idMap_.end()) {
        return it->second;
    }
    return nullptr;
}

Element* Document::querySelector(const std::string& selector) {
    // Try litehtml first
    if (litehtml_doc_ && litehtml_doc_->root()) {
        auto found = litehtml_doc_->root()->select_one(selector);
        if (found) {
            auto it = litehtmlMap_.find(found);
            if (it != litehtmlMap_.end()) return it->second;
        }
    }
    // Fallback: simple selector matching on bro::dom tree (for dynamic elements)
    if (root_ && root_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(root_)->querySelectorSimple(selector);
    }
    return nullptr;
}

std::vector<Element*> Document::querySelectorAll(const std::string& selector) {
    std::vector<Element*> result;

    // litehtml results (fast, covers parsed HTML elements)
    if (litehtml_doc_ && litehtml_doc_->root()) {
        auto found = litehtml_doc_->root()->select_all(selector);
        for (auto& lh_elem : found) {
            auto it = litehtmlMap_.find(lh_elem);
            if (it != litehtmlMap_.end()) {
                result.push_back(it->second);
            }
        }
    }

    // Also search the bro::dom tree for dynamically-created elements
    // that may not be in litehtml's selector index.
    if (root_ && root_->nodeType() == NodeType::Element) {
        size_t before = result.size();
        static_cast<Element*>(root_)->querySelectorAllSimple(selector, result);
        // Deduplicate only if we added new results
        if (result.size() > before) {
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
        }
    }

    return result;
}

std::string Document::title() const {
    if (!documentElement_) return {};

    // Walk the tree looking for a <TITLE> element
    std::vector<Element*> allElems;
    const_cast<Document*>(this)->collectElements(root_, allElems);
    for (auto* elem : allElems) {
        if (elem->tagName() == "TITLE") {
            return elem->textContent();
        }
    }
    return {};
}

void Document::setTitle(const std::string& title) {
    if (!documentElement_) return;

    std::vector<Element*> allElems;
    collectElements(root_, allElems);
    for (auto* elem : allElems) {
        if (elem->tagName() == "TITLE") {
            elem->setTextContent(title);
            return;
        }
    }

    // If no <title> found, try to find <head> and add one
    for (auto* elem : allElems) {
        if (elem->tagName() == "HEAD") {
            auto* titleElem = createElement("title");
            titleElem->setTextContent(title);
            elem->appendChild(titleElem);
            return;
        }
    }
}

void Document::registerElementId(const std::string& id, Element* elem) {
    if (!id.empty() && elem) {
        idMap_[id] = elem;
    }
}

void Document::unregisterElementId(const std::string& id) {
    idMap_.erase(id);
}

void Document::buildTreeFromLitehtml(litehtml::element::ptr root, Element* parentElem) {
    if (!root) return;

    for (auto& lh_child : root->children()) {
        std::string tag = lh_child->get_tagName() ? lh_child->get_tagName() : "";

        if (tag.empty()) {
            // Text node - litehtml represents text as elements with empty tag
            litehtml::string text;
            lh_child->get_text(text);
            auto* textNode = allocateNode<TextNode>(text);
            parentElem->appendChild(textNode);
        } else {
            auto* childElem = allocateNode<Element>(tag);
            childElem->setLitehtmlElement(lh_child);
            childElem->setDocument(this);
            litehtmlMap_[lh_child] = childElem;

            // Copy all attributes from litehtml's html_tag attribute map.
            auto* htmlTag = dynamic_cast<litehtml::html_tag*>(lh_child.get());
            if (htmlTag) {
                auto& attrs = htmlTag->*kAttrsPtr;
                for (auto& [name, val] : attrs) {
                    childElem->setAttribute(name, val);
                }
            }

            parentElem->appendChild(childElem);

            // Recurse
            buildTreeFromLitehtml(lh_child, childElem);
        }
    }
}

void Document::collectElements(Node* node, std::vector<Element*>& out) {
    if (!node) return;
    if (node->nodeType() == NodeType::Element) {
        out.push_back(static_cast<Element*>(node));
    }
    for (auto& child : node->childNodes()) {
        collectElements(child, out);
    }
}

// ---------------------------------------------------------------------------
// White-space / newline helpers for litehtml text sync
// ---------------------------------------------------------------------------

bool Document::preservesNewlines(const litehtml::element::ptr& lhElem) {
    if (!lhElem) return false;
    auto ws = lhElem->css().get_white_space();
    return ws == litehtml::white_space_pre ||
           ws == litehtml::white_space_pre_wrap ||
           ws == litehtml::white_space_pre_line;
}

std::string Document::textToLitehtmlHtml(const std::string& text,
                                         const litehtml::element::ptr& parentLh) {
    if (text.empty()) return {};

    bool hasNewlines = text.find('\n') != std::string::npos;
    bool shouldConvert = hasNewlines && preservesNewlines(parentLh);

    std::string html;
    html.reserve(text.size() + (shouldConvert ? text.size() / 4 : 0));
    for (char c : text) {
        if (c == '\n' && shouldConvert) html += "<br>";
        else if (c == '<')              html += "&lt;";
        else if (c == '&')              html += "&amp;";
        else                            html += c;
    }
    return html;
}

// ---------------------------------------------------------------------------
// Template extraction — pre-process HTML before litehtml parsing
// ---------------------------------------------------------------------------

std::string Document::extractTemplates(const std::string& html,
                                       std::vector<TemplateBlock>& out)
{
    std::string result;
    result.reserve(html.size());
    size_t pos = 0;
    int genId = 0;

    while (pos < html.size()) {
        // Find next <template
        size_t start = html.find("<template", pos);
        if (start == std::string::npos) {
            result.append(html, pos, html.size() - pos);
            break;
        }

        // Append everything before the <template
        result.append(html, pos, start - pos);

        // Find end of opening tag
        size_t tagEnd = html.find('>', start);
        if (tagEnd == std::string::npos) {
            result.append(html, start, html.size() - start);
            break;
        }

        // Extract attributes from the opening tag
        std::string openTag = html.substr(start, tagEnd - start + 1);

        // Parse id from attributes
        std::string id;
        size_t idPos = openTag.find("id=\"");
        if (idPos == std::string::npos) idPos = openTag.find("id='");
        if (idPos != std::string::npos) {
            char quote = openTag[idPos + 3];
            size_t idStart = idPos + 4;
            size_t idEnd = openTag.find(quote, idStart);
            if (idEnd != std::string::npos)
                id = openTag.substr(idStart, idEnd - idStart);
        }
        if (id.empty()) {
            id = "__bro_tmpl_" + std::to_string(genId++);
        }

        // Find matching </template>
        size_t contentStart = tagEnd + 1;
        size_t closeTag = html.find("</template>", contentStart);
        if (closeTag == std::string::npos) {
            // Malformed — just pass through
            result.append(html, start, html.size() - start);
            break;
        }

        std::string innerHTML = html.substr(contentStart, closeTag - contentStart);

        TemplateBlock block;
        block.id = id;
        block.innerHTML = innerHTML;
        out.push_back(std::move(block));

        // Replace with a hidden div placeholder that litehtml will keep
        result += "<div data-bro-template=\"" + id + "\" id=\"" + id + "\" style=\"display:none\"></div>";

        pos = closeTag + 11; // skip past </template>
    }

    return result;
}

void Document::injectTemplates(const std::vector<TemplateBlock>& templates)
{
    for (auto& tmpl : templates) {
        Element* placeholder = getElementById(tmpl.id);
        if (!placeholder) continue;

        // Create a TEMPLATE element, copy the placeholder's children/id,
        // then replace the placeholder's tag/nature
        // Simplest: just change the placeholder's tag to TEMPLATE and
        // parse the innerHTML as children. But we can't change tag_ easily.
        //
        // Instead: create a new TEMPLATE element, parse content into it,
        // and swap it into the tree where the placeholder is.

        auto* tmplElem = createElement("TEMPLATE");
        tmplElem->setAttribute("id", tmpl.id);

        // Parse the template innerHTML as child elements
        // We need a temporary litehtml parse to get the element tree.
        // Use a simpler approach: store the raw HTML and parse on .content access.
        // For now, store as a data attribute and parse in the JS .content getter.
        tmplElem->setAttribute("data-bro-template-html", tmpl.innerHTML);

        // Replace placeholder with template element in the tree
        auto* parent = placeholder->parentElement();
        if (parent) {
            parent->insertBefore(tmplElem, placeholder);
            parent->removeChild(placeholder);
            // Register in id map
            registerElementId(tmpl.id, tmplElem);
        }
    }
}

Element* Document::findElementByLitehtml(const litehtml::element::ptr& lhElem) {
    if (!lhElem) return nullptr;
    auto it = litehtmlMap_.find(lhElem);
    return (it != litehtmlMap_.end()) ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Dynamic DOM → litehtml sync
// ---------------------------------------------------------------------------

void Document::syncAppendToLitehtml(Element* child, Element* parent) {
    if (!child || !parent || !litehtml_doc_) return;
    auto parentLh = parent->litehtmlElement();
    if (!parentLh) return;
    if (child->litehtmlElement()) return; // already linked

    std::string html = child->outerHTML();
    size_t beforeCount = parentLh->children().size();

    // Add to litehtml element tree (CSS is applied, render items are NOT
    // reliably created here — the render tree rebuild handles that).
    litehtml_doc_->append_children_from_string(*parentLh, html.c_str(), false);

    // Link the newly created litehtml element to our bro::dom element
    auto& lhChildren = parentLh->children();
    if (lhChildren.size() > beforeCount) {
        auto it = lhChildren.begin();
        std::advance(it, beforeCount);
        linkElementToLitehtml(*it, child);
    }

    markStructureDirty();
}

void Document::syncInsertBeforeLitehtml(Element* newChild, Element* refChild,
                                         Element* parent) {
    if (!newChild || !parent || !litehtml_doc_) return;
    auto parentLh = parent->litehtmlElement();
    if (!parentLh) return;
    if (newChild->litehtmlElement()) return;

    std::string html = newChild->outerHTML();
    size_t beforeCount = parentLh->children().size();

    litehtml_doc_->append_children_from_string(*parentLh, html.c_str(), false);

    auto& lhChildren = parentLh->children();
    if (lhChildren.size() <= beforeCount) return;

    // Link the new element (currently at end)
    auto newLh = lhChildren.back();
    linkElementToLitehtml(newLh, newChild);

    // Move to before the reference element in the litehtml children list
    if (refChild && refChild->litehtmlElement()) {
        auto refLh = refChild->litehtmlElement();
        auto& mutChildren = mutableChildren(parentLh);
        auto newIt = std::prev(mutChildren.end());
        auto refIt = std::find(mutChildren.begin(), mutChildren.end(), refLh);
        if (refIt != mutChildren.end() && newIt != refIt) {
            mutChildren.splice(refIt, mutChildren, newIt);
        }

    }

    markStructureDirty();
}

void Document::syncRemoveFromLitehtml(Element* child, Element* parent) {
    if (!child || !parent) return;
    auto childLh = child->litehtmlElement();
    auto parentLh = parent->litehtmlElement();
    if (!childLh || !parentLh) return;

    // Remove from litehtml element tree
    parentLh->removeChild(childLh);

    // Clean up litehtmlMap entries for the whole subtree
    unlinkLitehtmlRecursive(child);

    markStructureDirty();
}

void Document::parseInnerHTML(Element* parent, const std::string& html) {
    if (!parent) return;

    // 1. Clear existing children from litehtml tree
    auto parentLh = parent->litehtmlElement();
    if (parentLh) {
        // Unlink bro::dom wrappers for all litehtml children
        for (auto* child : parent->children()) {
            unlinkLitehtmlRecursive(child);
        }
        // Clear litehtml children (element tree and render tree)
        parentLh->clearRecursive();
        auto ri = parentLh->get_render_item();
        if (ri) ri->children().clear();
    }

    // 2. Clear existing bro::dom children
    auto oldKids = parent->childNodes();
    for (auto* child : oldKids) {
        child->setParent(nullptr);
    }
    parent->childNodes().clear();
    for (auto* child : oldKids) {
        freeNode(child);
    }

    if (html.empty()) {
        markStructureDirty();
        return;
    }

    // 3. Parse into litehtml and mirror to bro::dom
    if (parentLh && litehtml_doc_) {
        // Use litehtml to parse the HTML fragment
        litehtml_doc_->append_children_from_string(*parentLh, html.c_str(), false);

        // Mirror the parsed litehtml children into bro::dom
        buildTreeFromLitehtml(parentLh, parent);
    } else {
        // Element not yet in litehtml tree — parse via a temporary wrapper.
        // When this element is later appended, outerHTML() will serialize
        // the children correctly for sync.
        // Use a minimal litehtml parse to extract the DOM structure.
        std::string wrapper = "<div>" + html + "</div>";
        litehtml::document_container* container = nullptr;
        // We need a container for parsing — get it from the litehtml doc
        if (litehtml_doc_) {
            container = litehtml_doc_->container();
        }
        if (container) {
            auto tempDoc = litehtml::document::createFromString(wrapper, container);
            if (tempDoc) {
                auto tempRoot = tempDoc->root();
                if (tempRoot) {
                    // Find the <div> wrapper — it's usually inside <html><body><div>
                    // Walk to find our wrapper div
                    std::function<litehtml::element::ptr(litehtml::element::ptr)> findDiv;
                    findDiv = [&](litehtml::element::ptr el) -> litehtml::element::ptr {
                        if (!el) return nullptr;
                        const char* tag = el->get_tagName();
                        if (tag && std::string(tag) == "div" && el == tempRoot->children().back()) {
                            // This might be our div, but let's look deeper for body>div
                        }
                        for (auto& child : el->children()) {
                            const char* ctag = child->get_tagName();
                            if (ctag && std::string(ctag) == "div") return child;
                            auto found = findDiv(child);
                            if (found) return found;
                        }
                        return nullptr;
                    };
                    auto wrapperDiv = findDiv(tempRoot);
                    if (wrapperDiv) {
                        buildTreeFromLitehtml(wrapperDiv, parent);
                    }
                }
            }
        }
    }

    markStructureDirty();
}

// ---------------------------------------------------------------------------
// Shadow DOM → litehtml sync
// ---------------------------------------------------------------------------

void Document::syncShadowToLitehtml(Element* host) {
    if (!host || !host->hasShadow() || !litehtml_doc_) return;
    auto hostLh = host->litehtmlElement();
    if (!hostLh) return;

    auto* shadow = host->shadowRoot();
    std::string scopeAttr = shadow->scopeId();

    // 1. Unlink shadow children from litehtml (but keep in bro::dom)
    // Walk all bro::dom elements that might have litehtml links from a previous sync.
    std::function<void(Node*)> unlinkShadowTree = [&](Node* node) {
        if (!node) return;
        if (node->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(node);
            auto lh = elem->litehtmlElement();
            if (lh) {
                litehtmlMap_.erase(lh);
                elem->setLitehtmlElement(nullptr);
            }
            // Also unlink children, but don't cross into nested shadow roots
            if (!elem->hasShadow() || elem == host) {
                for (auto* child : elem->childNodes()) {
                    unlinkShadowTree(child);
                }
            }
        }
    };
    for (auto* child : shadow->childNodes()) {
        unlinkShadowTree(child);
    }
    // Also unlink host's light DOM children (they may have been linked from slot projection)
    for (auto* child : host->children()) {
        unlinkLitehtmlRecursive(child);
    }

    // Clear litehtml children under host
    hostLh->clearRecursive();
    auto ri = hostLh->get_render_item();
    if (ri) ri->children().clear();

    // 2. Add scope attribute to the host element itself so `:host` and
    //    descendant selectors like `[data-bro-shadow="sN"] .foo` work.
    hostLh->set_attr("data-bro-shadow", scopeAttr.c_str());

    // 3. Apply :host styles as inline styles on the host element
    std::string hostCSS = shadow->hostStyles();
    if (!hostCSS.empty()) {
        // Merge with existing inline styles
        std::string existing;
        const char* cur = hostLh->get_attr("style");
        if (cur) existing = cur;
        if (!existing.empty() && existing.back() != ';') existing += "; ";
        existing += hostCSS;
        hostLh->set_attr("style", existing.c_str());
        hostLh->compute_styles(false);
    }

    // 4. Sync shadow tree children to litehtml under the host.
    //    Shadow CSS rules are resolved to inline styles on each element.
    shadow->distributeSlots();

    // Collect elements that need custom element upgrade after sync
    std::vector<Element*> elementsToUpgrade;

    std::function<void(Node*, litehtml::element::ptr)> syncNode;
    syncNode = [&](Node* node, litehtml::element::ptr parentLh) {
        if (!node) return;

        if (node->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(node);

            // Skip <style> elements (CSS already injected above)
            if (elem->tagName() == "STYLE") return;

            // <slot> element: substitute with assigned light DOM nodes
            if (elem->tagName() == "SLOT") {
                auto assigned = shadow->assignedNodes(elem);
                if (assigned.empty()) {
                    // Fallback: render slot's own children
                    for (auto* child : elem->childNodes()) {
                        syncNode(child, parentLh);
                    }
                } else {
                    for (auto* assignedNode : assigned) {
                        if (assignedNode->nodeType() == NodeType::Element) {
                            auto* assignedElem = static_cast<Element*>(assignedNode);
                            std::string html = assignedElem->outerHTML();
                            size_t before = parentLh->children().size();
                            litehtml_doc_->append_children_from_string(
                                *parentLh, html.c_str(), false);
                            auto& lhChildren = parentLh->children();
                            if (lhChildren.size() > before) {
                                auto it = lhChildren.begin();
                                std::advance(it, before);
                                linkElementToLitehtml(*it, assignedElem);
                            }
                        } else if (assignedNode->nodeType() == NodeType::Text) {
                            auto* textNode = static_cast<TextNode*>(assignedNode);
                            std::string html = textToLitehtmlHtml(textNode->data(), parentLh);
                            if (!html.empty()) {
                                litehtml_doc_->append_children_from_string(
                                    *parentLh, html.c_str(), false);
                            }
                        }
                    }
                }
                return;
            }

            // Regular shadow element: serialize to HTML and add to litehtml.
            // Resolve shadow CSS rules to inline styles on this element.
            std::string lower = elem->tagName();
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            std::string html = "<" + lower;
            // Add other attributes
            for (auto& [key, val] : elem->attributes()) {
                if (key == "style") continue; // handled below
                html += " " + key + "=\"" + val + "\"";
            }
            // Build inline style: existing styles + resolved shadow CSS
            std::string css = elem->style().cssText();
            auto attrIt = elem->attributes().find("style");
            if (attrIt != elem->attributes().end()) {
                if (!css.empty()) css += "; ";
                css += attrIt->second;
            }
            std::string shadowCSS = shadow->resolveInlineStyles(elem);
            if (!shadowCSS.empty()) {
                if (!css.empty()) css += "; ";
                css += shadowCSS;
            }
            if (!css.empty()) {
                html += " style=\"" + css + "\"";
            }
            html += "></" + lower + ">";

            size_t before = parentLh->children().size();
            litehtml_doc_->append_children_from_string(
                *parentLh, html.c_str(), false);

            auto& lhChildren = parentLh->children();
            if (lhChildren.size() > before) {
                auto it = lhChildren.begin();
                std::advance(it, before);
                // Link and recurse
                elem->setLitehtmlElement(*it);
                litehtmlMap_[*it] = elem;

                // Track for custom element upgrade
                if (lower.find('-') != std::string::npos) {
                    elementsToUpgrade.push_back(elem);
                }

                // Recurse into children (but not into elements with shadow roots -
                // those will get their own syncShadowToLitehtml call)
                if (!elem->hasShadow()) {
                    for (auto* child : elem->childNodes()) {
                        syncNode(child, *it);
                    }
                }
            }
        } else if (node->nodeType() == NodeType::Text) {
            auto* textNode = static_cast<TextNode*>(node);
            std::string html = textToLitehtmlHtml(textNode->data(), parentLh);
            if (!html.empty()) {
                litehtml_doc_->append_children_from_string(
                    *parentLh, html.c_str(), false);
            }
        }
    };

    for (auto* child : shadow->childNodes()) {
        syncNode(child, hostLh);
    }

    // 5. Upgrade any custom elements found in the shadow tree.
    //    Their constructors may call attachShadow + set innerHTML,
    //    which will recursively call syncShadowToLitehtml for nested shadows.
    for (auto* elem : elementsToUpgrade) {
        if (elem->hasShadow()) {
            syncShadowToLitehtml(elem);
        }
    }

    markStructureDirty();
}

void Document::linkElementToLitehtml(litehtml::element::ptr lh, Element* elem) {
    if (!lh || !elem) return;
    elem->setLitehtmlElement(lh);
    litehtmlMap_[lh] = elem;

    // Register ID if present
    std::string elemId = elem->id();
    if (!elemId.empty()) {
        idMap_[elemId] = elem;
    }

    // Recursively link child elements (skip litehtml text nodes)
    auto& lhChildren = lh->children();
    auto broChildren = elem->children(); // Element children only

    auto lhIt = lhChildren.begin();
    size_t broIdx = 0;

    while (lhIt != lhChildren.end() && broIdx < broChildren.size()) {
        const char* tag = (*lhIt)->get_tagName();
        if (tag && tag[0] != '\0') {
            // Element node — link to corresponding bro::dom child
            linkElementToLitehtml(*lhIt, broChildren[broIdx]);
            broIdx++;
        }
        ++lhIt;
    }
}

void Document::unlinkLitehtmlRecursive(Element* elem) {
    if (!elem) return;
    auto lh = elem->litehtmlElement();
    if (lh) {
        litehtmlMap_.erase(lh);
        elem->setLitehtmlElement(nullptr);
    }
    for (auto* child : elem->children()) {
        unlinkLitehtmlRecursive(child);
    }
}

} // namespace bro::dom
