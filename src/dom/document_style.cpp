// The restyle pass: the walk that turns the cascade plus the dirty bits into
// every element's computed style. Focus tracking and the class-change question
// live here too, because both exist only to decide what that walk has to
// re-resolve, as does the runtime <style> reconcile it runs first.
//
// Split out of document.cpp, which had grown past the size this repo keeps its
// translation units to. The computed-style diff this pass and the generated
// content pass share sits in document_internal.h.

#include "dom/document.h"
#include "dom/document_internal.h"
#include "engine/css_transitions.h"
#include "engine/web_animations.h"
#include "layout/element_ref_adapter.h"
#include "css/parser.h"
#include "css/color.h"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace bro::dom {

void Document::setActiveElement(Element* el) {
    if (focusedElement_ == el) return;
    // A focus change adds/removes :focus (and :focus-within on ancestors)
    // styling and toggles the native input/textarea caret, all of which live
    // in the cached HTML base. Dirty the outgoing and incoming elements so
    // their styles re-resolve and the base is re-recorded; without this the
    // retained-base cache re-presents the stale frame and the focus change
    // doesn't paint until some unrelated restyle (e.g. a :hover) forces a
    // rebuild. markDirty() propagates to the document, so this also drives the
    // relayout the :focus rules may need.
    if (focusedElement_) focusedElement_->markDirty();
    if (el) el->markDirty();
    focusedElement_ = el;
}

namespace {

// light-dark(<light>, <dark>) resolves at computed-value time to one of its
// two arguments, picked by the element's used colour scheme. The cascade
// hands values over as text, and every consumer (paint, getComputedStyle,
// transitions, inheritance) reads that text, so the pick happens here, on the
// text: each light-dark(...) is replaced by the chosen argument verbatim.
// Replacing it with the argument's text rather than a resolved rgb() keeps
// `currentcolor` inside a branch meaning what it means where it is used.
// Nested calls (a light-dark() inside a branch or inside color-mix()) are
// resolved by repeating until none remain.
bool resolveLightDark(std::string& value, bool dark) {
    static constexpr std::string_view kFn = "light-dark(";
    bool changed = false;
    for (int guard = 0; guard < 64; ++guard) {
        // Case-insensitive search for the function name.
        size_t at = std::string::npos;
        for (size_t i = 0; i + kFn.size() <= value.size(); ++i) {
            bool match = true;
            for (size_t k = 0; k < kFn.size(); ++k) {
                if (std::tolower(static_cast<unsigned char>(value[i + k])) != kFn[k]) {
                    match = false;
                    break;
                }
            }
            // Not the tail of a longer identifier (`my-light-dark(`).
            if (match && i > 0) {
                const unsigned char p = static_cast<unsigned char>(value[i - 1]);
                if (std::isalnum(p) || p == '-' || p == '_') match = false;
            }
            if (match) { at = i; break; }
        }
        if (at == std::string::npos) break;

        // Split the arguments at the top-level comma, find the closing paren.
        const size_t open = at + kFn.size();
        int depth = 0;
        size_t comma = std::string::npos, close = std::string::npos;
        for (size_t i = open; i < value.size(); ++i) {
            const char c = value[i];
            if (c == '(') ++depth;
            else if (c == ')') {
                if (depth == 0) { close = i; break; }
                --depth;
            } else if (c == ',' && depth == 0 && comma == std::string::npos) {
                comma = i;
            }
        }
        if (close == std::string::npos || comma == std::string::npos) break;  // malformed: leave it

        auto trim = [](std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return std::string(s);
        };
        const std::string_view whole(value);
        std::string pick = dark ? trim(whole.substr(comma + 1, close - comma - 1))
                                : trim(whole.substr(open, comma - open));
        value.replace(at, close + 1 - at, pick);
        changed = true;
    }
    return changed;
}

void resolveLightDarkValues(htmlayout::css::ComputedStyle& computed,
                            const htmlayout::css::MediaContext* media) {
    // Cheap reject: most styles name no light-dark() at all. Every spelling
    // of the name has a '-' followed by d/D, then "ark(" in some case.
    auto mentions = [](const std::string& v) {
        for (size_t i = v.find('-'); i != std::string::npos; i = v.find('-', i + 1)) {
            if (i + 6 <= v.size() && (v[i + 1] | 0x20) == 'd' && (v[i + 2] | 0x20) == 'a' &&
                (v[i + 3] | 0x20) == 'r' && (v[i + 4] | 0x20) == 'k' && v[i + 5] == '(')
                return true;
        }
        return false;
    };
    bool any = false;
    for (const auto& [prop, val] : computed) {
        if (mentions(val)) {
            any = true;
            break;
        }
    }
    if (!any) return;

    const auto preferred = (media && media->colorScheme == "dark") ? htmlayout::css::ColorScheme::Dark
                                                                    : htmlayout::css::ColorScheme::Light;
    auto csIt = computed.find("color-scheme");
    const std::string_view csValue = csIt != computed.end() ? std::string_view(csIt->second)
                                                            : std::string_view("normal");
    const bool dark =
        htmlayout::css::usedColorScheme(csValue, preferred) == htmlayout::css::ColorScheme::Dark;
    for (auto& [prop, val] : computed) {
        // A custom property is a token stream until var() substitutes it into
        // a real property, which is where its light-dark() is resolved.
        if (prop.size() > 1 && prop[0] == '-' && prop[1] == '-') continue;
        resolveLightDark(val, dark);
    }
}

// Split a class attribute into its whitespace-separated tokens.
std::vector<std::string> classTokens(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string t;
    while (iss >> t) out.push_back(t);
    return out;
}
}  // namespace

void Document::resolveColorSchemeValues(htmlayout::css::ComputedStyle& style) const {
    resolveLightDarkValues(style, hasMediaContext_ ? &mediaContext_ : nullptr);
}

bool Document::classChangeAffectsDescendants(const std::string& oldCls,
                                             const std::string& newCls) const {
    auto before = classTokens(oldCls);
    auto after  = classTokens(newCls);
    auto changedOne = [&](const std::vector<std::string>& a,
                          const std::vector<std::string>& b) {
        for (const auto& t : a) {
            if (std::find(b.begin(), b.end(), t) != b.end()) continue;  // unchanged
            if (cascade_.classAffectsDescendants(t)) return true;
        }
        return false;
    };
    return changedOne(before, after) || changedOne(after, before);
}

void Document::resolveStyles() {
    if (!documentElement_) return;
    // A top-layer entry whose element left the tree or lost its required
    // attribute (a dialog's `open` removed by hand) leaves before :modal is
    // matched again.
    pruneTopLayer();
    auto styleT0 = std::chrono::steady_clock::now();
    // A new stylesheet can restyle anything, and the elements it now matches
    // were never marked dirty (nobody touched them — the *rules* changed). So a
    // sheet arriving forces a full re-resolve; otherwise a runtime
    // document.head.appendChild(styleEl) would only reach elements that some
    // unrelated change happened to dirty later.
    bool sheetAdded = styleElsDirty_ && reconcileStyleElements();
    // A media-context change (viewport resize, color-scheme flip) rebuilt the
    // cascade: the rules changed under every element, same invalidation as a
    // new sheet.
    if (mediaRebuilt_) { sheetAdded = true; mediaRebuilt_ = false; }
    // Sticky: once a sheet declares `border: inherit` (or any other forced
    // inherit of a non-inherited property), the scoped restyle below can no
    // longer prove a clean subtree, for this document, for good.
    if (cascade_.usesForcedInherit()) forcedInherit_ = true;
    layout::ElementRefAdapter::clearCache();
    restyled_.clear();
    // A new sheet can change what matches anywhere, so it is a selector-level
    // invalidation, not just a re-resolve of the values already matched.
    resolveStylesRecursive(documentElement_, nullptr, /*force=*/sheetAdded,
                           /*selectorForce=*/sheetAdded);
    auto genT0 = std::chrono::steady_clock::now();
    resolveGeneratedContent();
    perf_.genContentMs += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - genT0).count();
    restyled_.clear();
    layout::ElementRefAdapter::clearCache();
    perf_.styleMs += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - styleT0).count();
}

// A <style> inserted at runtime (createElement("style") + head.appendChild, the
// CSS-in-JS pattern) never went through parse(), so its rules aren't in the
// cascade. Scan the connected tree for any <style> not yet added and add it.
// Incremental (no cascade clear) to preserve UA / linked / shadow-scoped sheets
// and @keyframes/@font-face. A <style> whose text is still empty is left for a
// later pass (its content may be assigned after insertion).
bool Document::reconcileStyleElements() {
    styleElsDirty_ = false;
    if (!documentElement_) return false;
    std::vector<Element*> all;
    collectElements(root_, all);
    bool added = false;
    for (auto* e : all) {
        if (e->tagName() != "STYLE" || e->styleSheetAdded()) continue;
        std::string css = e->textContent();
        if (css.empty()) continue;
        addSheetToCascade(htmlayout::css::parse(css));
        e->setStyleSheetAdded(true);
        added = true;
    }
    return added;
}

void Document::resolveStylesRecursive(Element* elem,
                                       const htmlayout::css::ComputedStyle* parentStyle,
                                       bool force,
                                       bool selectorForce,
                                       bool hoverForce) {
    // Did a selector input change on this element (class/id/attribute/:hover),
    // or on an ancestor? Either way every rule in this subtree may now match
    // differently, so the subtree has to re-resolve and `selDirty` carries that
    // all the way down — `.dark .btn` can match a grandchild.
    //
    // An inline-style write sets neither: it cannot change what matches, only
    // what this element hands down. Then the inherited diff below decides, and a
    // paint-only write like `container.style.opacity = x` re-resolves exactly one
    // element instead of its entire subtree.
    const bool selDirty = selectorForce | elem->takeSelectorDirty();

    // A :hover flipped at or under this element's scope (set on the hovered
    // chains' common ancestor, so siblings are covered too). Unlike selDirty
    // this does NOT re-resolve the subtree: the only selector input that moved
    // is :hover, so an element can only re-match if some rule names :hover
    // outside its subject compound AND names this element as that subject
    // (`.row:hover .label` — hoverCanAffect). Everything else in the subtree
    // keeps the style it has, which is what keeps a mouse move off the bill of
    // whatever container it happens to be over.
    const bool hoverDirty = hoverForce | elem->takeHoverScopeDirty();
    const bool hoverResolve =
        hoverDirty && !selDirty &&
        cascade_.hoverCanAffect(elem->tagName(), elem->getAttribute("id"),
                                elem->getAttribute("class"));

    // An element with an active CSS animation or transition must re-resolve
    // its style every frame so applyOverrides() below re-runs and advances the
    // interpolated value — even when nothing marked it dirty. This is
    // deliberately decoupled from markDirty(): a compositor-promoted animation
    // (transform/opacity only) advances its style here without dirtying the
    // document, so the cached base is never re-recorded on its account. Without
    // this, applyOverrides only ran on the frame the animation was registered
    // and every animation froze on its first applied value.
    bool animatingSelf =
        (animationManager_ && animationManager_->hasActive(elem)) ||
        (transitionManager_ && transitionManager_->hasActive(elem)) ||
        (webAnimationManager_ && webAnimationManager_->hasActive(elem));

    bool needsResolve = force || selDirty || hoverResolve || elem->isDirty() ||
                        elem->computedStyle().empty() || animatingSelf;

    // Set below from the style diff: can this element's re-resolve have changed
    // anything a descendant sees? Only through an inherited value.
    bool passedDownChanged = false;

    if (needsResolve) {
        perf_.elementsStyled++;
        auto* adapter = layout::ElementRefAdapter::getOrCreate(elem);

        // Inline style: StyleProxy is the sole source (Element::setAttribute
        // routes "style" attribute writes into it too, see element.cpp), so
        // there's only one declaration block to resolve, not two to merge.
        auto cascadeT0 = std::chrono::steady_clock::now();
        auto computed = cascade_.resolve(*adapter, elem->style().cssText(), parentStyle);
        perf_.cascadeMs += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - cascadeT0).count();

        // <svg> width/height attributes are presentational hints: they map to
        // the CSS width/height properties below author-stylesheet priority
        // (SVG 2). When the cascade produced no value, the attribute applies
        // directly — this is what makes `svg { width: 100% }` plus
        // height="180" lay out 180px tall in browsers, rather than deriving
        // the height from an intrinsic aspect ratio.
        {
            const std::string& tag = elem->tagName();
            if (tag == "svg" || tag == "SVG") {
                for (const char* prop : {"width", "height"}) {
                    if (computed.find(prop) != computed.end()) continue;
                    const std::string& v = elem->getAttribute(prop);
                    if (v.empty() || v == "auto") continue;
                    char* end = nullptr;
                    float num = std::strtof(v.c_str(), &end);
                    if (end == v.c_str() || num < 0) continue;
                    std::string rest(end);
                    if (rest.empty())
                        computed[prop] = v + "px";
                    else if (rest == "px" || rest == "%")
                        computed[prop] = v;
                }
            }
        }

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
                    resolved = num * rootFontSize_;
                } else if (unit == "pt") {
                    resolved = num * 96.0f / 72.0f;
                }
                fsIt->second = std::to_string(resolved);
                // The document element (<html>) defines the rem reference for
                // every descendant. It is resolved first (parentStyle==nullptr),
                // so capture its px font-size before children consume rem.
                if (!parentStyle) rootFontSize_ = resolved;
                // Clean up trailing zeros for readability (e.g. "32.000000" -> "32")
                auto& s = fsIt->second;
                if (s.find('.') != std::string::npos) {
                    s.erase(s.find_last_not_of('0') + 1, std::string::npos);
                    if (s.back() == '.') s.pop_back();
                }
                // A computed length carries its unit: getComputedStyle reports
                // "20px", and htmlayout (a container query's em, for one)
                // reads the value as CSS, where a bare number is not a length.
                s += "px";
            }
        }

        // light-dark(): the element's used colour scheme (its color-scheme,
        // weighed against the prefers-color-scheme setting) picks the branch,
        // before transitions compare old and new values or children inherit.
        resolveColorSchemeValues(computed);

        auto mgrT0 = std::chrono::steady_clock::now();
        // CSS transitions: detect property changes and start transitions
        if (transitionManager_ && !elem->computedStyle().empty()) {
            transitionManager_->onStyleChange(elem, elem->computedStyle(), computed, transitionTime_);
        }

        // CSS animations: detect animation-name and start animations
        if (animationManager_) {
            animationManager_->onStyleChange(elem, computed, transitionTime_);
        }

        // Did this re-resolve move any geometry? A hover that only repainted a
        // background didn't, and skips layout entirely; a change to width or
        // font-size did, and dirties this element's layout node (and, through
        // it, the ancestors that have to reflow around it) so the incremental
        // pass recomputes that chain and reuses every other subtree. Skipped
        // when the whole tree is already queued for relayout — the diff would
        // tell us nothing and every element would pay for it.
        // Both questions in one walk. When fullLayout_ is set the whole tree is
        // already relaying out, so the layout-affecting half is skipped (and can
        // never come back true).
        auto diffT0 = std::chrono::steady_clock::now();
        perf_.styleManagersMs += std::chrono::duration<double, std::milli>(diffT0 - mgrT0).count();
        StyleChange change = classifyStyleChange(elem->computedStyle(), computed,
                                                 /*wantLayout=*/!fullLayout_);
        perf_.styleDiffMs += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - diffT0).count();
        if (change.layoutAffecting) {
            layoutDirty_ = true;
            elem->markLayoutDirty();
        }

        // Nothing inherited moved ⇒ no descendant's computed style can differ,
        // so the recursion below stops here unless a descendant is dirty on its
        // own account.
        passedDownChanged = change.inherited;

        // Keep the custom-property set this element hands its children on the
        // same pointer when its contents did not change, so the descendants'
        // pointer comparison above stays quiet across an unrelated re-resolve.
        computed.stableChildVarsFrom(elem->computedStyle());
        elem->setComputedStyle(std::move(computed));

        // CSS transitions: apply interpolated overrides after setting style
        if (transitionManager_) {
            transitionManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        // CSS animations: apply keyframe overrides
        if (animationManager_) {
            animationManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        // Web Animations (element.animate): script animations sit above both
        // CSS transitions and CSS animations in composite order.
        if (webAnimationManager_) {
            webAnimationManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        // ::before / ::after generated content is resolved in a separate pass
        // (resolveGeneratedContent) once all styles are known — counter() and
        // the quote keywords depend on scopes and nesting that only make sense
        // in tree order. Note this element for that pass: its pseudo-elements
        // are the only ones that can have changed, since a pseudo's rules are
        // matched against its originating element and inherit from its style.
        restyled_.push_back(elem);

        elem->clearDirty();
    }

    // Recurse into children. They must re-resolve when a selector input changed
    // at or above this element (their rule set may differ) or when an inherited
    // value this element hands down actually changed. Otherwise they keep the
    // style they have — a child that is dirty on its own account still resolves,
    // the recursion always walks the tree.
    //
    // Unless the page forces `inherit` on a property that does not normally
    // inherit (`border: inherit`): that ties a descendant's value to a parent
    // property the inherited-value diff above never looks at, so give up the
    // scoping and re-resolve the subtree the way we always did.
    const bool childForce =
        needsResolve && (selDirty || passedDownChanged || forcedInherit_);
    // The hover scope carries all the way down: `.row:hover .cell .label` names
    // a subject several levels below the element whose :hover flipped, and the
    // levels in between re-match nothing themselves.
    const bool childHoverForce = hoverDirty;

    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == NodeType::Element) {
            resolveStylesRecursive(static_cast<Element*>(child), &elem->computedStyle(),
                                   childForce, selDirty, childHoverForce);
        }
    }

    // Recurse into shadow DOM children
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == NodeType::Element) {
                resolveStylesRecursive(static_cast<Element*>(child), &elem->computedStyle(),
                                       childForce, selDirty, childHoverForce);
            }
        }
    }
}

} // namespace bro::dom
