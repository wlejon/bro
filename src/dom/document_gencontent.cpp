// Generated content (::before / ::after): counters, counters(), attr(), quotes.
// The pass that runs after style resolution, the counter/quote state it threads
// through the tree, and the content-value tokenizer that consumes it.
//
// Split out of document.cpp, which had grown past the size this repo keeps its
// translation units to. It shares the computed-style diff in
// document_internal.h with the restyle pass, because a pseudo-element whose
// content or box moved owes a layout for the same reasons a real element does.

#include "dom/document.h"
#include "dom/document_internal.h"
#include "layout/element_ref_adapter.h"
#include "css/cascade.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::dom {

namespace {

// A live CSS counter instance. `depth` is the tree depth of the element whose
// counter-reset created it; scope closes when traversal returns to a shallower
// or equal depth on a following element (see resolveGeneratedContentRecursive).
struct CounterEntry { long value = 0; int depth = 0; };

// Split a `counter-reset` / `counter-increment` value into (name, number)
// pairs. Numbers are optional (default `dflt`): "item" -> {item, dflt};
// "a 3 b" -> {a,3},{b,dflt-for-b? no: b gets dflt}. A number binds to the
// preceding name.
std::vector<std::pair<std::string,long>> parseCounterOps(const std::string& v, long dflt) {
    std::vector<std::pair<std::string,long>> out;
    std::istringstream ss(v);
    std::string tok;
    while (ss >> tok) {
        // Is tok a (possibly signed) integer?
        char* end = nullptr;
        long n = std::strtol(tok.c_str(), &end, 10);
        if (end != tok.c_str() && *end == '\0') {
            if (!out.empty()) out.back().second = n;   // number for preceding name
        } else if (tok != "none") {
            out.push_back({tok, dflt});
        }
    }
    return out;
}

// Parse the `quotes` property into open/close pairs. Empty or auto/none ->
// the English default (curly double then single). Value form:
//   "open1" "close1" "open2" "close2" ...
std::vector<std::pair<std::string,std::string>> parseQuotes(const std::string& v) {
    std::vector<std::string> strs;
    size_t i = 0;
    while (i < v.size()) {
        char c = v[i];
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            std::string s;
            while (j < v.size() && v[j] != c) { s += v[j]; ++j; }
            strs.push_back(s);
            i = (j < v.size()) ? j + 1 : j;
        } else {
            ++i;
        }
    }
    std::vector<std::pair<std::string,std::string>> pairs;
    for (size_t k = 0; k + 1 < strs.size(); k += 2) pairs.push_back({strs[k], strs[k+1]});
    if (pairs.empty()) {
        // English default: U+201C/U+201D, then U+2018/U+2019 (UTF-8 bytes).
        pairs.push_back({"\xE2\x80\x9C", "\xE2\x80\x9D"});
        pairs.push_back({"\xE2\x80\x98", "\xE2\x80\x99"});
    }
    return pairs;
}

} // namespace

struct Document::GenContentState {
    std::unordered_map<std::string, std::vector<CounterEntry>> counters;
    int quoteDepth = 0;

    // Pop every counter instance created deeper than `depth` — those scopes
    // closed once traversal returned to this level.
    void popDeeperThan(int depth) {
        for (auto& [name, stack] : counters) {
            while (!stack.empty() && stack.back().depth > depth) stack.pop_back();
        }
    }
    long counterValue(const std::string& name) const {
        auto it = counters.find(name);
        if (it == counters.end() || it->second.empty()) return 0;
        return it->second.back().value;
    }
    std::string countersValue(const std::string& name, const std::string& sep) const {
        auto it = counters.find(name);
        if (it == counters.end() || it->second.empty()) return "";
        std::string out;
        for (size_t i = 0; i < it->second.size(); ++i) {
            if (i) out += sep;
            out += std::to_string(it->second[i].value);
        }
        return out;
    }
    void reset(const std::string& name, long value, int depth) {
        auto& stack = counters[name];
        while (!stack.empty() && stack.back().depth >= depth) stack.pop_back();
        stack.push_back({value, depth});
    }
    void increment(const std::string& name, long value, int) {
        auto& stack = counters[name];
        // Incrementing a counter that no counter-reset created acts as though it
        // had been reset to 0 on the ROOT element (CSS 2.1 §12.4.3) — so the
        // implicit instance belongs to the root scope, at depth 0. Creating it at
        // the incrementing element's depth instead would scope it to that
        // element: `li::before { counter-increment: item }` increments at the
        // pseudo's depth (one deeper than the <li>), and popDeeperThan would drop
        // it the moment traversal reached the next <li> — restarting every marker
        // at 1.
        if (stack.empty()) stack.push_back({0, 0});
        stack.back().value += value;
    }
};

void Document::resolveGeneratedContent() {
    if (!documentElement_) return;

    // No ::before/::after rule anywhere ⇒ no element can carry generated
    // content, and none ever did, so there is nothing to resolve or clear.
    if (!cascade_.hasPseudoElementRules("before") &&
        !cascade_.hasPseudoElementRules("after")) {
        return;
    }

    // counter()/counters()/quotes read state that accumulates across the whole
    // document in tree order, so they leave us no choice but to walk all of it.
    if (statefulGenContent_) {
        GenContentState st;
        resolveGeneratedContentRecursive(documentElement_, 0, st);
        return;
    }

    // Otherwise a pseudo-element is a pure function of its originating element:
    // its rules are matched against that element, and it inherits from that
    // element's computed style. So only an element whose style was re-resolved
    // this pass can have different generated content — every other element
    // keeps the pseudo it already has.
    //
    // This is the same assumption resolveStylesRecursive already makes for real
    // elements (it re-resolves only dirty elements and their forced subtrees),
    // so the two passes now agree on what "could have changed" means. Walking
    // the whole document here instead cost ~3.5 ms per hover on a 391-row list:
    // an O(document) price paid on every pointer move, for generated content
    // that most pages don't have at all.
    //
    // The very first pass re-resolves every element, so a document that does
    // use counters or quotes is guaranteed to trip the flag below on its way
    // through — and then re-runs in document order, which is what makes this
    // safe to decide from the elements we happen to be visiting.
    GenContentState st;   // counter state goes unread on this path
    for (auto* elem : restyled_) {
        const auto& style = elem->computedStyle();
        auto dispIt = style.find("display");
        if (dispIt != style.end() && dispIt->second == "none") {
            elem->clearPseudos();
            continue;
        }
        applyPseudo(elem, "before", 0, st);
        applyPseudo(elem, "after", 0, st);
    }

    // A stateful value turned up while we were resolving locally, so the values
    // just written may have the wrong counter/quote state. Redo the pass the
    // slow, correct way — and, the flag being sticky, every pass after it.
    if (statefulGenContent_) {
        GenContentState full;
        resolveGeneratedContentRecursive(documentElement_, 0, full);
    }
}

// Apply an element's counter-reset then counter-increment declarations at the
// given depth. Shared by real elements and pseudo-elements.
void Document::applyCounterOps(Element* elem, const htmlayout::css::ComputedStyle& style,
                               int depth, Document::GenContentState& st) {
    auto rit = style.find("counter-reset");
    if (rit != style.end() && rit->second != "none" && !rit->second.empty()) {
        for (auto& [name, val] : parseCounterOps(rit->second, 0))
            st.reset(name, val, depth);
    }
    auto iit = style.find("counter-increment");
    if (iit != style.end() && iit->second != "none" && !iit->second.empty()) {
        for (auto& [name, val] : parseCounterOps(iit->second, 1))
            st.increment(name, val, depth);
    }
    (void)elem;
}

void Document::resolveGeneratedContentRecursive(Element* elem, int depth, GenContentState& st) {
    // Close counter scopes from any preceding cousin subtree deeper than us.
    st.popDeeperThan(depth);

    const auto& style = elem->computedStyle();
    auto dispIt = style.find("display");
    bool isNone = (dispIt != style.end() && dispIt->second == "none");

    // The element's own counter operations (reset before increment).
    if (!isNone) applyCounterOps(elem, style, depth, st);

    // display:none paints nothing, pseudo-elements included. Otherwise leave the
    // existing pseudo state alone — applyPseudo diffs against it to decide
    // whether the box moved, so clearing it up front would make every pseudo
    // look brand new and promote a layout on every restyle.
    if (isNone) elem->clearPseudos();

    // ::before is the element's first child — resolve it (and its counter ops)
    // after the element's own increment so counter() sees the post-increment
    // value, matching the "first child" model.
    if (!isNone) applyPseudo(elem, "before", depth + 1, st);

    // Recurse into real children in document order.
    if (!isNone) {
        for (auto* child : elem->childNodes()) {
            if (child->nodeType() == NodeType::Element)
                resolveGeneratedContentRecursive(static_cast<Element*>(child), depth + 1, st);
        }
        if (elem->hasShadow()) {
            auto* sr = elem->shadowRoot();
            for (auto* child : sr->childNodes()) {
                if (child->nodeType() == NodeType::Element)
                    resolveGeneratedContentRecursive(static_cast<Element*>(child), depth + 1, st);
            }
        }
    }

    // ::after is the last child — resolve after children so it sees their
    // counter state (and the correct close-quote nesting depth).
    if (!isNone) applyPseudo(elem, "after", depth + 1, st);
}

void Document::applyPseudo(Element* elem, const char* which, int depth, GenContentState& st) {
    // A pseudo-element that stops matching has to be torn back down, so this
    // owns both directions. The layout tree syncs its synthetic pseudo box in
    // ensurePseudo() during layout, which means an appearing or disappearing
    // pseudo is a GEOMETRY change: without the promotions below, a
    // `:hover::before { content: "..." }` would restyle paint-only, skip layout
    // entirely, and never actually show up (or, once shown, never go away).
    auto dropPseudo = [&] {
        if (!elem->hasPseudo(which)) return;
        elem->clearPseudo(which);
        layoutDirty_ = true;
        elem->markLayoutDirty();
    };

    // Cheapest possible miss: a sheet with no ::after rule shouldn't cost an
    // adapter allocation per element just to be told nothing matched.
    if (!cascade_.hasPseudoElementRules(which)) { dropPseudo(); return; }
    auto* adapter = layout::ElementRefAdapter::getOrCreate(elem);
    perf_.pseudoResolves++;
    auto pseudoStyle = cascade_.resolvePseudo(*adapter, which, elem->computedStyle());
    resolveColorSchemeValues(pseudoStyle);
    auto cIt = pseudoStyle.find("content");
    if (cIt == pseudoStyle.end()) { dropPseudo(); return; }
    const std::string& raw = cIt->second;
    if (raw.empty() || raw == "normal" || raw == "none") { dropPseudo(); return; }

    // This element really does generate counter/quote content, so the document
    // needs the stateful document-order pass from here on (see
    // resolveGeneratedContent). Latched on the RESOLVED value, not on the
    // stylesheet: the UA sheet's `q::before { content: open-quote }` means every
    // document has such a rule, but only one with an actual <q> in it pays.
    if (!statefulGenContent_ && htmlayout::css::Cascade::contentIsStateful(raw))
        statefulGenContent_ = true;

    // A pseudo-element can itself carry counter-reset / counter-increment; it
    // acts as a child of its originating element, so apply at depth.
    applyCounterOps(elem, pseudoStyle, depth, st);

    // Quotes come from the pseudo's (inherited) `quotes` property.
    auto qIt = pseudoStyle.find("quotes");
    std::string quotesVal = (qIt != pseudoStyle.end()) ? qIt->second : std::string();
    if (quotesVal == "auto" || quotesVal == "none") quotesVal.clear();
    auto quotePairs = parseQuotes(quotesVal);

    // Tokenize the content value into: quoted strings, counter()/counters(),
    // attr(), and the quote keywords. Whitespace between components is a token
    // separator and contributes no text (only string literals do).
    std::string out;
    size_t i = 0;
    const size_t n = raw.size();
    auto quoteIndex = [&](int d) -> size_t {
        if (d < 0) d = 0;
        return std::min<size_t>(static_cast<size_t>(d), quotePairs.size() - 1);
    };
    while (i < n) {
        char c = raw[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { ++i; continue; }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && raw[j] != c) {
                if (raw[j] == '\\' && j + 1 < n) { out += raw[j+1]; j += 2; }
                else { out += raw[j]; ++j; }
            }
            i = (j < n) ? j + 1 : j;
            continue;
        }
        // Read an identifier / function token up to a delimiter.
        size_t j = i;
        while (j < n && raw[j] != ' ' && raw[j] != '\t' && raw[j] != '\n' &&
               raw[j] != '\r' && raw[j] != '\f' && raw[j] != '(') ++j;
        std::string tok = raw.substr(i, j - i);
        if (j < n && raw[j] == '(') {
            // Function: capture the parenthesized argument text.
            size_t k = j + 1;
            int depthP = 1;
            std::string args;
            while (k < n && depthP > 0) {
                if (raw[k] == '(') { ++depthP; args += raw[k]; }
                else if (raw[k] == ')') { --depthP; if (depthP > 0) args += raw[k]; }
                else args += raw[k];
                ++k;
            }
            i = k;
            if (tok == "attr") {
                std::string an = args;
                // trim
                size_t a = an.find_first_not_of(" \t");
                size_t b = an.find_last_not_of(" \t");
                if (a != std::string::npos) an = an.substr(a, b - a + 1);
                out += elem->getAttribute(an);
            } else if (tok == "counter") {
                // counter( name [, style] ) — style ignored (decimal).
                std::string name = args;
                auto comma = name.find(',');
                if (comma != std::string::npos) name = name.substr(0, comma);
                size_t a = name.find_first_not_of(" \t");
                size_t b = name.find_last_not_of(" \t");
                if (a != std::string::npos) name = name.substr(a, b - a + 1);
                out += std::to_string(st.counterValue(name));
            } else if (tok == "counters") {
                // counters( name, sep [, style] )
                std::string name, sep;
                auto comma = args.find(',');
                if (comma != std::string::npos) {
                    name = args.substr(0, comma);
                    std::string rest = args.substr(comma + 1);
                    // sep is the first quoted string in rest.
                    size_t q = rest.find_first_of("\"'");
                    if (q != std::string::npos) {
                        char qc = rest[q];
                        size_t e = rest.find(qc, q + 1);
                        if (e != std::string::npos) sep = rest.substr(q + 1, e - q - 1);
                    }
                } else {
                    name = args;
                }
                size_t a = name.find_first_not_of(" \t");
                size_t b = name.find_last_not_of(" \t");
                if (a != std::string::npos) name = name.substr(a, b - a + 1);
                out += st.countersValue(name, sep);
            }
            // url(...) and other functions contribute no text.
            continue;
        }
        i = j;
        if (tok == "open-quote") {
            out += quotePairs[quoteIndex(st.quoteDepth)].first;
            ++st.quoteDepth;
        } else if (tok == "close-quote") {
            if (st.quoteDepth > 0) --st.quoteDepth;
            out += quotePairs[quoteIndex(st.quoteDepth)].second;
        } else if (tok == "no-open-quote") {
            ++st.quoteDepth;
        } else if (tok == "no-close-quote") {
            if (st.quoteDepth > 0) --st.quoteDepth;
        }
        // Bare idents (e.g. `normal`) contribute nothing.
    }

    // Same paint-only test the real elements get: a pseudo whose text changed —
    // or appeared — has moved geometry and needs a layout; one that only changed
    // colour has not, and stays on the cheap paint-only path.
    if (!elem->hasPseudo(which) || elem->pseudoContent(which) != out ||
        classifyStyleChange(elem->pseudoStyle(which), pseudoStyle,
                            /*wantLayout=*/true).layoutAffecting) {
        layoutDirty_ = true;
        elem->markLayoutDirty();
    }

    elem->setPseudo(which, std::move(out), std::move(pseudoStyle));
}

} // namespace bro::dom
