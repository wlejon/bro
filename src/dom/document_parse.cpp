// HTML in, tree out: everything that turns markup into this document's nodes.
// The full-document parse(), the gumbo walk both entry points share, the
// <template> pre-pass that runs before gumbo ever sees the source, the
// innerHTML fragment parse, and the child-list release a fragment parse starts
// from.
//
// Split out of document.cpp, which had grown past the size this repo keeps its
// translation units to. Style resolution, generated content and node ownership
// are the other three halves and live beside this file.

#include "dom/document.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "engine/css_transitions.h"
#include "layout/layout_node_adapter.h"
#include "css/parser.h"
#include <gumbo.h>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace bro::dom {

// ---------------------------------------------------------------------------
// Parsing with gumbo
// ---------------------------------------------------------------------------

void Document::parse(const std::string& html, const std::string& authorCss,
                     const std::string& uaCss) {
    // Clear any existing tree
    root_ = nullptr;
    documentElement_ = nullptr;
    body_ = nullptr;
    focusedElement_ = nullptr;   // ownedNodes_.clear() bypasses freeNode's scrub
    idMap_.clear();
    // LayoutRoot points into ownedNodes_ via raw Element*; clear it first so
    // we don't hold dangling pointers when ownedNodes_ drops its unique_ptrs.
    layoutRoot_.reset();
    // Any outstanding Range (including the Selection's backing range) points
    // into the tree we're about to destroy. ownedNodes_.clear() bypasses
    // freeNode, so onNodeDestroyed never fires — null endpoints first.
    if (selection_) selection_->removeAllRanges();
    for (auto* r : liveRanges_) {
        if (r) {
            r->onNodeDestroyed(r->startContainer());
            r->onNodeDestroyed(r->endContainer());
        }
    }
    // The CSS transition/animation managers hold raw Element* keys into the
    // storage cleared just below. This path bypasses freeNode(), so the
    // per-node scrub there never runs for them — drop the whole document's
    // entries while the keys are still alive to be compared.
    if (transitionManager_) transitionManager_->forgetDocument(this);
    if (animationManager_) animationManager_->forgetDocument(this);
    pendingFrees_.clear();
    pendingSet_.clear();
    ownedNodes_.clear();
    cascade_.clear();
    retainedSheets_.clear();

    // Add UA default styles (lowest priority — author styles always win)
    if (!uaCss.empty()) {
        addSheetToCascade(htmlayout::css::parse(uaCss), nullptr,
                          htmlayout::css::Origin::UserAgent);
    }

    // Add author CSS (app stylesheets)
    if (!authorCss.empty()) {
        addSheetToCascade(htmlayout::css::parse(authorCss));
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
            registerElementId(elem->id(), elem);

            // Extract <style> elements and add their CSS to the cascade
            if (elem->tagName() == "STYLE") {
                std::string css = elem->textContent();
                if (!css.empty()) {
                    addSheetToCascade(htmlayout::css::parse(css));
                    elem->setStyleSheetAdded(true);
                }
            }
        }
    }
    // The parse pass above added every <style> present in the source, so no
    // reconcile is owed until the DOM next changes.
    styleElsDirty_ = false;

    gumbo_destroy_output(&kGumboDefaultOptions, output);
    dirty_ = false;
}

void Document::buildTreeFromGumbo(::GumboNode* node, Element* parentElem) {
    // GUMBO_NODE_TEMPLATE is a distinct node type carrying a GumboElement, not a
    // GUMBO_NODE_ELEMENT — gumbo splits it out precisely so clients can choose
    // whether to descend (see the comment on the enum in gumbo.h). bro only ever
    // matched GUMBO_NODE_ELEMENT, so every <template> and everything inside it
    // silently vanished from any document parsed straight through gumbo
    // (DOMParser, innerHTML). The app-HTML path only escaped that because
    // extractTemplates() rewrites templates to placeholders before parsing.
    if (!node || (node->type != GUMBO_NODE_ELEMENT &&
                  node->type != GUMBO_NODE_TEMPLATE)) return;
    auto* gumboElem = &node->v.element;

    for (unsigned int i = 0; i < gumboElem->children.length; ++i) {
        auto* child = static_cast<GumboNode*>(gumboElem->children.data[i]);

        if (child->type == GUMBO_NODE_ELEMENT ||
            child->type == GUMBO_NODE_TEMPLATE) {
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

            if (child->type == GUMBO_NODE_TEMPLATE) {
                // Template children go into a separate DocumentFragment, never
                // the normal child list — that is what makes them inert: not
                // laid out, not painted, not reachable from a document query.
                auto* frag = allocateNode<Element>("#DOCUMENT-FRAGMENT");
                childElem->setTemplateContent(frag);
                buildTreeFromGumbo(child, frag);
            } else {
                buildTreeFromGumbo(child, childElem);
            }

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

    releaseChildrenPreservingElements(parent);

    if (html.empty()) {
        parent->markStructureDirty();
        return;
    }

    // Parse the fragment in the context of the element receiving it. This is
    // what the HTML fragment parsing algorithm requires, and skipping it is not
    // cosmetic: without a context the tree builder sits in "in body" mode,
    // where <tr>, <td>, <tbody>, <thead>, <tfoot> and <caption> start tags are
    // parse errors and get DROPPED, keeping only their text. So
    //
    //     tbody.innerHTML = '<tr><td>a</td></tr>'
    //
    // used to yield a row-less table of height 0, and setting a <tr>'s
    // innerHTML collapsed its cells into one bare text node. gumbo takes the
    // context as a tag enum; anything it doesn't know (custom elements) falls
    // back to the historical <div> wrapper.
    std::string lowerTag = parent->tagName();
    for (auto& c : lowerTag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    GumboTag ctxTag = gumbo_tag_enum(lowerTag.c_str());

    GumboOptions opts = kGumboDefaultOptions;
    std::string wrapper;
    GumboOutput* output = nullptr;

    if (ctxTag != GUMBO_TAG_UNKNOWN && ctxTag != GUMBO_TAG_LAST) {
        opts.fragment_context = ctxTag;
        opts.fragment_namespace = GUMBO_NAMESPACE_HTML;
        output = gumbo_parse_with_options(&opts, html.c_str(), html.length());
    } else {
        wrapper = "<html><body><div>" + html + "</div></body></html>";
        output = gumbo_parse_with_options(&opts, wrapper.c_str(), wrapper.length());
    }
    if (!output) {
        parent->markStructureDirty();
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

    // In fragment mode gumbo synthesizes an <html> root and inserts the parsed
    // children straight into it, so that root IS the source node. The wrapper
    // path still has to dig out its <div>.
    GumboNode* source = (opts.fragment_context != GUMBO_TAG_LAST)
                      ? output->root
                      : findWrapper(output->root);
    if (source) {
        buildTreeFromGumbo(source, parent);
    }

    gumbo_destroy_output(&opts, output);

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
                addSheetToCascade(htmlayout::css::parse(css));
                // Mark added so the post-mutation reconcile (armed by the
                // markStructureDirty below) doesn't add these rules a second time.
                elem->setStyleSheetAdded(true);
            }
        }
        registerElementId(elem->id(), elem);
    }

    // Only this element's children moved, so only its layout children have to be
    // rebuilt. The document-wide mark would throw away the cached geometry of
    // every subtree in the document — and `host.innerHTML = ...` on one small
    // container is the single most common DOM update an app makes.
    parent->markStructureDirty();
}

void Document::releaseChildrenPreservingElements(Node* parent) {
    if (!parent) return;
    // A copy: the loops below unparent and free, both of which would invalidate
    // an iterator into the live vector.
    std::vector<Node*> oldKids = parent->childNodes();
    for (Node* child : oldKids) child->setParent(nullptr);
    parent->childNodes().clear();
    for (Node* child : oldKids) {
        if (child->nodeType() == NodeType::Element) {
            auto* el = static_cast<Element*>(child);
            if (!el->id().empty()) unregisterElementId(el->id(), el);
            continue;   // detached, not destroyed — see the header
        }
        freeNode(child);
    }
}

} // namespace bro::dom
