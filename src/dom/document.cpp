#include "dom/document.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "engine/css_transitions.h"
#include "layout/element_ref_adapter.h"
#include "layout/layout_node_adapter.h"
#include "css/parser.h"
#include "css/cascade.h"
#include <gumbo.h>
#include <sstream>
#include <functional>

namespace bro::dom {

Document::Document() = default;

// Selection owns a Range whose destructor calls back into unregisterRange().
// Members destroy in reverse declaration order, so if we defaulted this the
// liveRanges_ set would already be gone by the time ~Selection ran. Tear
// selection_ down first, then clear any JS-owned Ranges that outlived us.
Document::~Document() {
    selection_.reset();
    auto ranges = std::move(liveRanges_);
    for (auto* r : ranges) {
        if (r) r->setDocument(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Selection + live Range registry
// ---------------------------------------------------------------------------

Selection* Document::selection() {
    if (!selection_) selection_ = std::make_unique<Selection>(this);
    return selection_.get();
}

void Document::registerRange(Range* r) {
    if (r) liveRanges_.insert(r);
}

void Document::unregisterRange(Range* r) {
    if (!r) return;
    liveRanges_.erase(r);
    // Range destruction invalidates Selection if this is its backing range.
    if (selection_ && r == selection_->getRangeAt(0)) {
        selection_->removeAllRanges();
    }
}

void Document::notifyNodeRemoved(Node* removed) {
    if (!removed) return;
    Node* parent = removed->parentNode();
    if (!parent) return;
    int idx = -1;
    const auto& kids = parent->childNodes();
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i] == removed) { idx = static_cast<int>(i); break; }
    if (idx < 0) return;
    for (auto* r : liveRanges_)
        r->onNodeRemoved(removed, parent, idx);
    if (selection_ && selection_->rangeCount() > 0) {
        selection_->schedulePendingChange();
        selection_->flushPendingChange();
    }
}

void Document::notifyTextDataChanged(Node* node, int offset, int count, int newLen) {
    if (!node) return;
    for (auto* r : liveRanges_)
        r->onTextDataChanged(node, offset, count, newLen);
    if (selection_ && selection_->rangeCount() > 0) {
        selection_->schedulePendingChange();
        selection_->flushPendingChange();
    }
}

void Document::notifyTextSplit(Node* node, int offset, Node* tail) {
    if (!node || !tail) return;
    for (auto* r : liveRanges_)
        r->onTextSplit(node, offset, tail);
}

void Document::notifyChildInserted(Node* parent, int index) {
    if (!parent) return;
    for (auto* r : liveRanges_)
        r->onChildInserted(parent, index);
}

void Document::fireSelectionChange() {
    if (selectionChangeCb_) selectionChangeCb_(this);
}

// ---------------------------------------------------------------------------
// Parsing with gumbo
// ---------------------------------------------------------------------------

void Document::parse(const std::string& html, const std::string& authorCss,
                     const std::string& uaCss) {
    // Clear any existing tree
    root_ = nullptr;
    documentElement_ = nullptr;
    body_ = nullptr;
    idMap_.clear();
    // LayoutRoot points into ownedNodes_ via raw Element*; clear it first so
    // we don't hold dangling pointers when ownedNodes_ drops its unique_ptrs.
    layoutRoot_.reset();
    ownedNodes_.clear();
    cascade_.clear();

    // Add UA default styles (lowest priority — author styles always win)
    if (!uaCss.empty()) {
        cascade_.addStylesheet(htmlayout::css::parse(uaCss), nullptr, nullptr,
                               htmlayout::css::Origin::UserAgent);
    }

    // Add author CSS (app stylesheets)
    if (!authorCss.empty()) {
        cascade_.addStylesheet(htmlayout::css::parse(authorCss));
    }

    // Parse HTML with gumbo
    GumboOutput* output = gumbo_parse(html.c_str());
    if (!output) return;

    // Find the <html> element in gumbo's tree
    GumboNode* htmlNode = output->root;
    if (htmlNode && htmlNode->type == GUMBO_NODE_ELEMENT) {
        const char* rootTag = gumbo_normalized_tagname(htmlNode->v.element.tag);
        auto* rootElem = allocateNode<Element>(rootTag && rootTag[0] ? rootTag : "html");
        rootElem->setDocument(this);

        // Copy attributes from gumbo root
        GumboVector* attrs = &htmlNode->v.element.attributes;
        for (unsigned int i = 0; i < attrs->length; ++i) {
            auto* attr = static_cast<GumboAttribute*>(attrs->data[i]);
            rootElem->setAttribute(attr->name, attr->value ? attr->value : "");
        }

        root_ = rootElem;
        documentElement_ = rootElem;

        // Build children recursively
        buildTreeFromGumbo(htmlNode, rootElem);
    }

    // Find <body> and extract <style> elements
    if (documentElement_) {
        std::vector<Element*> allElems;
        collectElements(root_, allElems);

        for (auto* elem : allElems) {
            // Find body
            if (!body_ && elem->tagName() == "BODY") {
                body_ = elem;
            }

            // Register IDs
            std::string elemId = elem->id();
            if (!elemId.empty()) {
                idMap_[elemId] = elem;
            }

            // Extract <style> elements and add their CSS to the cascade
            if (elem->tagName() == "STYLE") {
                std::string css = elem->textContent();
                if (!css.empty()) {
                    cascade_.addStylesheet(htmlayout::css::parse(css));
                }
            }
        }
    }

    gumbo_destroy_output(&kGumboDefaultOptions, output);
    dirty_ = false;
}

void Document::buildTreeFromGumbo(::GumboNode* node, Element* parentElem) {
    if (!node || node->type != GUMBO_NODE_ELEMENT) return;
    auto* gumboElem = &node->v.element;

    for (unsigned int i = 0; i < gumboElem->children.length; ++i) {
        auto* child = static_cast<GumboNode*>(gumboElem->children.data[i]);

        if (child->type == GUMBO_NODE_ELEMENT) {
            const char* tag = gumbo_normalized_tagname(child->v.element.tag);
            std::string tagStr = (tag && tag[0]) ? tag : "";

            // Handle unknown tags (gumbo returns "" for custom elements)
            if (tagStr.empty()) {
                GumboStringPiece original = child->v.element.original_tag;
                if (original.data && original.length > 0) {
                    // Extract tag name from "<tag-name ..." or "<tag-name>"
                    const char* start = original.data + 1; // skip '<'
                    const char* end = start;
                    while (end < original.data + original.length &&
                           *end != ' ' && *end != '>' && *end != '/' && *end != '\t' && *end != '\n') {
                        ++end;
                    }
                    tagStr = std::string(start, end);
                }
                if (tagStr.empty()) tagStr = "div";
            }

            auto* childElem = allocateNode<Element>(tagStr);
            childElem->setDocument(this);

            // Copy attributes
            GumboVector* attrs = &child->v.element.attributes;
            for (unsigned int j = 0; j < attrs->length; ++j) {
                auto* attr = static_cast<GumboAttribute*>(attrs->data[j]);
                std::string attrName = attr->name;
                // Reconstruct namespace prefix for SVG/XML attributes
                switch (attr->attr_namespace) {
                    case GUMBO_ATTR_NAMESPACE_XLINK:
                        attrName = "xlink:" + attrName; break;
                    case GUMBO_ATTR_NAMESPACE_XML:
                        attrName = "xml:" + attrName; break;
                    case GUMBO_ATTR_NAMESPACE_XMLNS:
                        if (attrName != "xmlns") attrName = "xmlns:" + attrName; break;
                    default: break;
                }
                childElem->setAttribute(attrName, attr->value ? attr->value : "");
            }

            parentElem->appendChild(childElem);
            buildTreeFromGumbo(child, childElem);

        } else if (child->type == GUMBO_NODE_TEXT ||
                   child->type == GUMBO_NODE_WHITESPACE) {
            const char* text = child->v.text.text;
            if (text && text[0]) {
                auto* textNode = allocateNode<TextNode>(text);
                parentElem->appendChild(textNode);
            }
        } else if (child->type == GUMBO_NODE_COMMENT) {
            const char* data = child->v.text.text;
            auto* commentNode = allocateNode<CommentNode>(data ? data : "");
            parentElem->appendChild(commentNode);
        }
    }
}

// ---------------------------------------------------------------------------
// Style resolution
// ---------------------------------------------------------------------------

void Document::resolveStyles() {
    if (!documentElement_) return;
    layout::ElementRefAdapter::clearCache();
    resolveStylesRecursive(documentElement_, nullptr);
    layout::ElementRefAdapter::clearCache();
}

void Document::resolveStylesRecursive(Element* elem,
                                       const htmlayout::css::ComputedStyle* parentStyle,
                                       bool force) {
    bool needsResolve = force || elem->isDirty() || elem->computedStyle().empty();

    if (needsResolve) {
        auto* adapter = layout::ElementRefAdapter::getOrCreate(elem);

        // Build inline style string from style attribute + StyleProxy.
        // StyleProxy (JS-set values) comes LAST so it overrides the HTML attribute
        // (later declarations of equal specificity win in CSS).
        std::string inlineStyle;
        auto attrIt = elem->attributes().find("style");
        if (attrIt != elem->attributes().end()) {
            inlineStyle = attrIt->second;
        }
        const auto& proxyStyle = elem->style().cssText();
        if (!proxyStyle.empty()) {
            if (!inlineStyle.empty()) inlineStyle += "; ";
            inlineStyle += proxyStyle;
        }

        auto computed = cascade_.resolve(*adapter, inlineStyle, parentStyle);

        // Resolve font-size to absolute px so all consumers get a usable value.
        // em/% are relative to the parent's (already-resolved) font-size.
        auto fsIt = computed.find("font-size");
        if (fsIt != computed.end() && !fsIt->second.empty()) {
            const auto& val = fsIt->second;
            char* end = nullptr;
            float num = std::strtof(val.c_str(), &end);
            if (end != val.c_str() && num > 0) {
                std::string unit(end);
                float resolved = num; // default: px or unitless
                if (unit == "em") {
                    float parentFs = 16.0f;
                    if (parentStyle) {
                        auto pit = parentStyle->find("font-size");
                        if (pit != parentStyle->end()) {
                            char* pe = nullptr;
                            float pv = std::strtof(pit->second.c_str(), &pe);
                            if (pe != pit->second.c_str() && pv > 0) parentFs = pv;
                        }
                    }
                    resolved = num * parentFs;
                } else if (unit == "%") {
                    float parentFs = 16.0f;
                    if (parentStyle) {
                        auto pit = parentStyle->find("font-size");
                        if (pit != parentStyle->end()) {
                            char* pe = nullptr;
                            float pv = std::strtof(pit->second.c_str(), &pe);
                            if (pe != pit->second.c_str() && pv > 0) parentFs = pv;
                        }
                    }
                    resolved = num * parentFs / 100.0f;
                } else if (unit == "rem") {
                    resolved = num * 16.0f;
                } else if (unit == "pt") {
                    resolved = num * 96.0f / 72.0f;
                }
                fsIt->second = std::to_string(resolved);
                // Clean up trailing zeros for readability (e.g. "32.000000" -> "32")
                auto& s = fsIt->second;
                if (s.find('.') != std::string::npos) {
                    s.erase(s.find_last_not_of('0') + 1, std::string::npos);
                    if (s.back() == '.') s.pop_back();
                }
            }
        }

        // CSS transitions: detect property changes and start transitions
        if (transitionManager_ && !elem->computedStyle().empty()) {
            transitionManager_->onStyleChange(elem, elem->computedStyle(), computed, transitionTime_);
        }

        // CSS animations: detect animation-name and start animations
        if (animationManager_) {
            animationManager_->onStyleChange(elem, computed, transitionTime_);
        }

        elem->setComputedStyle(std::move(computed));

        // CSS transitions: apply interpolated overrides after setting style
        if (transitionManager_) {
            transitionManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        // CSS animations: apply keyframe overrides
        if (animationManager_) {
            animationManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        elem->clearDirty();
    }

    // Recurse into children. If this element was re-resolved, force children
    // to re-resolve too (inherited styles or selector context may have changed).
    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == NodeType::Element) {
            resolveStylesRecursive(static_cast<Element*>(child), &elem->computedStyle(), needsResolve);
        }
    }

    // Recurse into shadow DOM children
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == NodeType::Element) {
                resolveStylesRecursive(static_cast<Element*>(child), &elem->computedStyle(), needsResolve);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void Document::performLayout(float viewportWidth, htmlayout::layout::TextMetrics& metrics) {
    if (!documentElement_) return;
    if (!layoutRoot_ || structureDirty_) {
        layoutRoot_ = layout::LayoutNodeAdapter::buildTree(documentElement_);
        structureDirty_ = false;
    }
    htmlayout::layout::layoutTree(layoutRoot_.get(), viewportWidth, metrics);
    layoutRoot_->syncBoxToElement();
}

void Document::performLayout(float viewportWidth, float viewportHeight, htmlayout::layout::TextMetrics& metrics) {
    if (!documentElement_) return;
    if (!layoutRoot_ || structureDirty_) {
        layoutRoot_ = layout::LayoutNodeAdapter::buildTree(documentElement_);
        structureDirty_ = false;
    }
    htmlayout::layout::Viewport vp{viewportWidth, viewportHeight};
    htmlayout::layout::layoutTree(layoutRoot_.get(), vp, metrics);
    layoutRoot_->syncBoxToElement();
}

// ---------------------------------------------------------------------------
// Node creation
// ---------------------------------------------------------------------------

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
    auto kids = node->childNodes();
    for (auto* child : kids) {
        freeNode(child);
    }
    // Unregister element id from the lookup map
    if (node->nodeType() == NodeType::Element) {
        auto* elem = static_cast<Element*>(node);
        std::string id = elem->id();
        if (!id.empty())
            unregisterElementId(id);
    }
    ownedNodes_.erase(node);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Element* Document::getElementById(const std::string& id) {
    auto it = idMap_.find(id);
    return (it != idMap_.end()) ? it->second : nullptr;
}

Element* Document::querySelector(const std::string& selector) {
    if (root_ && root_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(root_)->querySelector(selector);
    }
    return nullptr;
}

std::vector<Element*> Document::querySelectorAll(const std::string& selector) {
    if (root_ && root_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(root_)->querySelectorAll(selector);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Title
// ---------------------------------------------------------------------------

std::string Document::title() const {
    if (!documentElement_) return {};
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
// Template extraction — pre-process HTML before gumbo parsing
// ---------------------------------------------------------------------------

std::string Document::extractTemplates(const std::string& html,
                                       std::vector<TemplateBlock>& out)
{
    std::string result;
    result.reserve(html.size());
    size_t pos = 0;
    int genId = 0;

    while (pos < html.size()) {
        // Skip HTML comments that might contain "<template" as text
        size_t commentStart = html.find("<!--", pos);
        size_t start = html.find("<template", pos);
        if (start == std::string::npos) {
            result.append(html, pos, html.size() - pos);
            break;
        }
        // If a comment starts before this match, skip past it first
        while (commentStart != std::string::npos && commentStart < start) {
            size_t commentEnd = html.find("-->", commentStart + 4);
            if (commentEnd == std::string::npos) break;
            commentEnd += 3; // past "-->"
            if (start < commentEnd) {
                // The "<template" was inside a comment — skip and re-search
                result.append(html, pos, commentEnd - pos);
                pos = commentEnd;
                start = html.find("<template", pos);
                if (start == std::string::npos) break;
                commentStart = html.find("<!--", pos);
                continue;
            }
            break;
        }
        if (start == std::string::npos) {
            result.append(html, pos, html.size() - pos);
            break;
        }
        result.append(html, pos, start - pos);

        size_t tagEnd = html.find('>', start);
        if (tagEnd == std::string::npos) {
            result.append(html, start, html.size() - start);
            break;
        }

        std::string openTag = html.substr(start, tagEnd - start + 1);
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

        size_t contentStart = tagEnd + 1;
        size_t closeTag = html.find("</template>", contentStart);
        if (closeTag == std::string::npos) {
            result.append(html, start, html.size() - start);
            break;
        }

        std::string innerHTML = html.substr(contentStart, closeTag - contentStart);

        TemplateBlock block;
        block.id = id;
        block.innerHTML = innerHTML;
        out.push_back(std::move(block));

        result += "<div data-bro-template=\"" + id + "\" id=\"" + id + "\" style=\"display:none\"></div>";
        pos = closeTag + 11;
    }

    return result;
}

void Document::injectTemplates(const std::vector<TemplateBlock>& templates) {
    for (auto& tmpl : templates) {
        Element* placeholder = getElementById(tmpl.id);
        if (!placeholder) continue;

        auto* tmplElem = createElement("TEMPLATE");
        tmplElem->setAttribute("id", tmpl.id);
        tmplElem->setAttribute("data-bro-template-html", tmpl.innerHTML);

        auto* parent = placeholder->parentElement();
        if (parent) {
            parent->insertBefore(tmplElem, placeholder);
            parent->removeChild(placeholder);
            registerElementId(tmpl.id, tmplElem);
        }
    }
}

// ---------------------------------------------------------------------------
// innerHTML parsing with gumbo
// ---------------------------------------------------------------------------

void Document::parseInnerHTML(Element* parent, const std::string& html) {
    if (!parent) return;

    // Clear existing children
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

    // Parse HTML fragment with gumbo (wrapped in a div)
    std::string wrapper = "<html><body><div>" + html + "</div></body></html>";
    GumboOutput* output = gumbo_parse(wrapper.c_str());
    if (!output) {
        markStructureDirty();
        return;
    }

    // Navigate to our wrapper div: html > body > div
    std::function<GumboNode*(GumboNode*)> findWrapper = [&](GumboNode* node) -> GumboNode* {
        if (!node || node->type != GUMBO_NODE_ELEMENT) return nullptr;
        GumboVector* children = &node->v.element.children;
        for (unsigned int i = 0; i < children->length; ++i) {
            auto* child = static_cast<GumboNode*>(children->data[i]);
            if (child->type == GUMBO_NODE_ELEMENT) {
                GumboTag tag = child->v.element.tag;
                if (tag == GUMBO_TAG_BODY) {
                    // Look for div inside body
                    GumboVector* bodyChildren = &child->v.element.children;
                    for (unsigned int j = 0; j < bodyChildren->length; ++j) {
                        auto* bodyChild = static_cast<GumboNode*>(bodyChildren->data[j]);
                        if (bodyChild->type == GUMBO_NODE_ELEMENT &&
                            bodyChild->v.element.tag == GUMBO_TAG_DIV) {
                            return bodyChild;
                        }
                    }
                }
                auto* found = findWrapper(child);
                if (found) return found;
            }
        }
        return nullptr;
    };

    GumboNode* wrapperDiv = findWrapper(output->root);
    if (wrapperDiv) {
        buildTreeFromGumbo(wrapperDiv, parent);
    }

    gumbo_destroy_output(&kGumboDefaultOptions, output);

    // Extract <style> elements from the fragment and add CSS to the cascade
    std::vector<Element*> newElems;
    for (auto* child : parent->childNodes()) {
        if (child->nodeType() == NodeType::Element)
            collectElements(child, newElems);
    }
    for (auto* elem : newElems) {
        if (elem->tagName() == "STYLE") {
            std::string css = elem->textContent();
            if (!css.empty()) {
                cascade_.addStylesheet(htmlayout::css::parse(css));
            }
        }
        std::string elemId = elem->id();
        if (!elemId.empty()) idMap_[elemId] = elem;
    }

    markStructureDirty();
}

// ---------------------------------------------------------------------------
// Shadow DOM CSS
// ---------------------------------------------------------------------------

void Document::addShadowStylesheet(ShadowRoot* sr, const std::string& css) {
    if (!sr || css.empty()) return;
    cascade_.addStylesheet(htmlayout::css::parse(css), static_cast<void*>(sr));
}

} // namespace bro::dom
