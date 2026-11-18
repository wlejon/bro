// The document's sheets as the CSSOM sees them: which element each sheet is
// (a <style>, a <link rel=stylesheet>), their tree order, a CSSOM edit's
// text, the disabled flag, and the constructed sheets a document or shadow
// root adopts. The CSSOM objects themselves are JavaScript (bronze_host's
// js/cssom.js); this is what they read and write.
//
// The cascade is rebuilt from retainedSheets_ whenever a sheet is removed,
// edited, disabled, reordered or adopted; a <style> appended after every
// other document sheet (the CSS-in-JS case) is still added incrementally.

#include "dom/document.h"
#include "dom/document_internal.h"
#include "dom/element.h"
#include "dom/shadow_root.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bro::dom {

bool isStylesheetLink(const Element* el) {
    // rel is a space-separated token list; "stylesheet" among them, and not
    // an alternate stylesheet (which does not apply).
    std::string rel = el->getAttribute("rel");
    for (auto& c : rel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool sheet = false, alternate = false;
    size_t i = 0;
    while (i < rel.size()) {
        while (i < rel.size() && std::isspace(static_cast<unsigned char>(rel[i]))) ++i;
        size_t j = i;
        while (j < rel.size() && !std::isspace(static_cast<unsigned char>(rel[j]))) ++j;
        const std::string tok = rel.substr(i, j - i);
        if (tok == "stylesheet") sheet = true;
        if (tok == "alternate") alternate = true;
        i = j;
    }
    return sheet && !alternate;
}

void Document::rebuildCascade() {
    cascade_.clear();
    for (auto& rs : retainedSheets_)
        if (!rs.disabled)
            cascade_.addStylesheet(rs.sheet, rs.scope, hasMediaContext_ ? &mediaContext_ : nullptr, rs.origin);
    mediaRebuilt_ = true;
    markDirty();
}

void Document::orderSheets(const std::vector<Element*>& order) {
    std::unordered_map<const Element*, size_t> pos;
    for (size_t i = 0; i < order.size(); ++i) pos.emplace(order[i], i);
    // Ranks: the UA sheet, then the document's sheets (a linked sheet with no
    // element first, then the element-owned ones by tree position), then the
    // document's adopted sheets, then shadow-scoped ones as they came.
    auto rank = [&](const RetainedSheet& rs) -> std::pair<int, size_t> {
        if (rs.origin == htmlayout::css::Origin::UserAgent) return {0, 0};
        if (rs.scope) return {4, 0};
        if (rs.adopted) return {3, 0};
        if (!rs.owner) return {1, 0};
        auto it = pos.find(rs.owner);
        return {2, it == pos.end() ? order.size() : it->second};
    };
    std::stable_sort(retainedSheets_.begin(), retainedSheets_.end(),
                     [&](const RetainedSheet& a, const RetainedSheet& b) { return rank(a) < rank(b); });
}

// A <style> inserted at runtime (createElement("style") + head.appendChild, the
// CSS-in-JS pattern) never went through parse(), so its rules aren't in the
// cascade; one removed takes its rules with it; one whose text changed is
// parsed again (unless a CSSOM edit owns its rules now). Runs from
// resolveStyles when the tree's structure changed. Returns whether the rules
// changed.
bool Document::reconcileStyleElements() {
    styleElsDirty_ = false;
    if (!documentElement_) return false;
    std::vector<Element*> all;
    collectElements(root_, all);
    std::unordered_set<const Element*> connected;
    for (auto* e : all) {
        const std::string& tag = e->tagName();
        if (tag == "STYLE" || tag == "LINK") connected.insert(e);
    }

    bool changed = sheetsStale_;
    sheetsStale_ = false;
    // Document-scope sheets whose element left the document go with it.
    const size_t before = retainedSheets_.size();
    retainedSheets_.erase(std::remove_if(retainedSheets_.begin(), retainedSheets_.end(),
                                         [&](const RetainedSheet& rs) {
                                             return rs.owner && !rs.scope && !connected.count(rs.owner);
                                         }),
                          retainedSheets_.end());
    if (retainedSheets_.size() != before) changed = true;

    std::unordered_map<const Element*, RetainedSheet*> byOwner;
    for (auto& rs : retainedSheets_)
        if (rs.owner && !rs.scope) byOwner.emplace(rs.owner, &rs);
    bool appendedOnly = true;   // every new sheet comes after every existing one
    bool hasAdopted = false;
    for (auto& rs : retainedSheets_) hasAdopted = hasAdopted || (rs.adopted && !rs.scope);
    std::vector<size_t> fresh;  // indices into retainedSheets_ of sheets added here
    bool seenNew = false;
    for (auto* e : all) {
        if (e->tagName() != "STYLE") {
            if (e->tagName() == "LINK" && byOwner.count(e) && seenNew) appendedOnly = false;
            continue;
        }
        auto it = byOwner.find(e);
        if (it != byOwner.end()) {
            if (seenNew) appendedOnly = false;
            RetainedSheet& rs = *it->second;
            if (rs.edited) continue;
            std::string css = e->textContent();
            if (css == rs.source) continue;
            rs.sheet = htmlayout::css::parse(css);
            rs.source = std::move(css);
            changed = true;
            continue;
        }
        std::string css = e->textContent();
        if (css.empty()) continue;   // its content may be assigned after insertion
        RetainedSheet rs{htmlayout::css::parse(css), nullptr, htmlayout::css::Origin::Author, false};
        rs.owner = e;
        rs.source = std::move(css);
        retainedSheets_.push_back(std::move(rs));
        fresh.push_back(retainedSheets_.size() - 1);
        e->setStyleSheetAdded(true);
        seenNew = true;
    }
    if (fresh.empty() && !changed) return false;
    if (!changed && appendedOnly && !hasAdopted) {
        // Appended after everything the document has: no reorder, and the
        // sheets already in the cascade stay.
        for (size_t i : fresh) {
            auto& rs = retainedSheets_[i];
            cascade_.addStylesheet(rs.sheet, rs.scope, hasMediaContext_ ? &mediaContext_ : nullptr, rs.origin);
        }
        return true;
    }
    orderSheets(all);
    rebuildCascade();
    return true;
}

bool Document::hasOwnedSheet(const Element* owner) const {
    for (const auto& rs : retainedSheets_)
        if (owner && rs.owner == owner) return true;
    return false;
}

std::string Document::ownedSheetText(const Element* owner) const {
    for (const auto& rs : retainedSheets_)
        if (owner && rs.owner == owner) return rs.source;
    return {};
}

bool Document::ownedSheetDisabled(const Element* owner) const {
    for (const auto& rs : retainedSheets_)
        if (owner && rs.owner == owner) return rs.disabled;
    return false;
}

void Document::setOwnedSheetText(Element* owner, const std::string& css) {
    for (auto& rs : retainedSheets_) {
        if (!owner || rs.owner != owner) continue;
        const std::string base = rs.sheet.baseUrl;
        rs.sheet = htmlayout::css::parse(css);
        rs.sheet.baseUrl = base;
        rs.source = css;
        rs.edited = true;
        rebuildCascade();
        return;
    }
}

void Document::setOwnedSheetDisabled(Element* owner, bool disabled) {
    for (auto& rs : retainedSheets_) {
        if (!owner || rs.owner != owner || rs.disabled == disabled) continue;
        rs.disabled = disabled;
        rebuildCascade();
        return;
    }
}

void Document::setAdoptedSheets(void* scope, const std::vector<std::string>& css) {
    retainedSheets_.erase(std::remove_if(retainedSheets_.begin(), retainedSheets_.end(),
                                         [scope](const RetainedSheet& rs) { return rs.adopted && rs.scope == scope; }),
                          retainedSheets_.end());
    for (const auto& text : css) {
        RetainedSheet rs{htmlayout::css::parse(text), scope, htmlayout::css::Origin::Author, false};
        rs.source = text;
        rs.adopted = true;
        retainedSheets_.push_back(std::move(rs));
    }
    // A shadow root's adopted sheets follow its own; its sheets stay in the
    // order they came, so appending keeps that.
    if (!scope) {
        std::vector<Element*> all;
        if (root_) collectElements(root_, all);
        orderSheets(all);
    }
    rebuildCascade();
}

void Document::forgetSheetOwner(const Element* owner) {
    for (auto& rs : retainedSheets_) {
        if (rs.owner != owner) continue;
        // A shadow tree's sheet stays scoped to its root, which may live on;
        // a document sheet goes at the next reconcile.
        rs.owner = nullptr;
        if (!rs.scope) {
            rs.disabled = true;
            rs.adopted = false;
            sheetsStale_ = true;
            styleElsDirty_ = true;
        }
    }
    if (sheetsStale_)
        retainedSheets_.erase(std::remove_if(retainedSheets_.begin(), retainedSheets_.end(),
                                             [](const RetainedSheet& rs) {
                                                 return !rs.owner && !rs.scope && rs.disabled && !rs.linked &&
                                                        rs.origin != htmlayout::css::Origin::UserAgent;
                                             }),
                              retainedSheets_.end());
}

void Document::addShadowStylesheet(ShadowRoot* sr, const std::string& css, Element* owner) {
    if (!sr || css.empty()) return;
    addSheetToCascade(htmlayout::css::parse(css), static_cast<void*>(sr));
    retainedSheets_.back().owner = owner;
    retainedSheets_.back().source = css;
}

}  // namespace bro::dom
