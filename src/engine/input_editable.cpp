#include "engine/engine.h"
#include "engine/input_common.h"
#include "engine/input_editable.h"
#include "platform/clipboard.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/text_node.h"
#include "engine/replaced_elements.h"
#include "layout/control_text.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/key_handle_result.h"
#include "util/time.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace bro::engine {

EditUndoScope::EditUndoScope(DomUndoHistories* hist, dom::Document* doc,
                             dom::Element* host, dom::TextNode* tn,
                             DomUndoStack::Kind kind)
    : hist_(hist), doc_(doc), host_(host), tn_(tn), kind_(kind) {
    if (!hist_ || !host_ || !doc_) return;
    selBefore_ = DomUndoStack::selectionOf(doc_, host_);
    if (tn_) beforeData_ = tn_->data();
    else beforeHTML_ = host_->innerHTML();
}

void EditUndoScope::commit() {
    if (!hist_ || !host_ || !doc_) return;
    const auto selAfter = DomUndoStack::selectionOf(doc_, host_);
    const double now = util::currentTimeMs();
    auto& stack = hist_->forHost(doc_, host_);
    if (tn_) {
        stack.recordTextEdit(host_, tn_, beforeData_, tn_->data(),
                             selBefore_, selAfter, kind_, now);
    } else {
        stack.recordStructural(host_, beforeHTML_, host_->innerHTML(),
                               selBefore_, selAfter, now);
    }
}

namespace {

int indexInParent(bro::dom::Node* n) {
    auto* p = n ? n->parentNode() : nullptr;
    if (!p) return -1;
    const auto& kids = p->childNodes();
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i] == n) return static_cast<int>(i);
    return -1;
}

bro::dom::Node* deepestLast(bro::dom::Node* n) {
    while (n && !n->childNodes().empty()) n = n->childNodes().back();
    return n;
}

bro::dom::Node* deepestFirst(bro::dom::Node* n) {
    while (n && !n->childNodes().empty()) n = n->childNodes().front();
    return n;
}

bool isBrElement(bro::dom::Node* n) {
    return n && n->nodeType() == bro::dom::NodeType::Element &&
           static_cast<bro::dom::Element*>(n)->tagName() == "BR";
}

bro::dom::Node* prevLeafWithin(bro::dom::Node* n, bro::dom::Element* host) {
    while (n && n != host) {
        const int i = indexInParent(n);
        auto* p = n->parentNode();
        if (i > 0) return deepestLast(p->childNodes()[i - 1]);
        n = p;
    }
    return nullptr;
}

bro::dom::Node* nextLeafWithin(bro::dom::Node* n, bro::dom::Element* host) {
    while (n && n != host) {
        const int i = indexInParent(n);
        auto* p = n->parentNode();
        if (i >= 0 && i + 1 < static_cast<int>(p->childNodes().size()))
            return deepestFirst(p->childNodes()[i + 1]);
        n = p;
    }
    return nullptr;
}

struct EditDeleteTarget {
    bro::dom::TextNode* text = nullptr;
    int start = 0;
    int end = 0;
    bro::dom::Node* remove = nullptr;
};

EditDeleteTarget resolveDeleteTarget(bro::dom::Node* node, int offset,
                                     bro::dom::Element* host, int dir) {
    EditDeleteTarget out;
    if (!node || !host) return out;

    auto spliceAt = [&](bro::dom::TextNode* t, int off) {
        const std::string& d = t->data();
        out.text = t;
        if (dir < 0) { out.start = bro::layout::utf8Prev(d, off); out.end = off; }
        else         { out.start = off; out.end = bro::layout::utf8Next(d, off); }
    };

    bro::dom::Node* leaf = nullptr;
    if (node->nodeType() == bro::dom::NodeType::Text) {
        auto* t = static_cast<bro::dom::TextNode*>(node);
        const int len = static_cast<int>(t->length());
        offset = std::clamp(offset, 0, len);
        if (dir < 0 ? offset > 0 : offset < len) { spliceAt(t, offset); return out; }
        leaf = (dir < 0) ? prevLeafWithin(t, host) : nextLeafWithin(t, host);
    } else {
        const auto& kids = node->childNodes();
        offset = std::clamp(offset, 0, static_cast<int>(kids.size()));
        if (dir < 0)
            leaf = (offset > 0) ? deepestLast(kids[offset - 1])
                                : prevLeafWithin(node, host);
        else
            leaf = (offset < static_cast<int>(kids.size()))
                       ? deepestFirst(kids[offset])
                       : nextLeafWithin(node, host);
    }

    while (leaf) {
        if (leaf->nodeType() == bro::dom::NodeType::Text) {
            auto* t = static_cast<bro::dom::TextNode*>(leaf);
            const int len = static_cast<int>(t->length());
            if (len > 0) { spliceAt(t, dir < 0 ? len : 0); return out; }
        } else if (isBrElement(leaf)) {
            out.remove = leaf;
            return out;
        }
        leaf = (dir < 0) ? prevLeafWithin(leaf, host) : nextLeafWithin(leaf, host);
    }
    return out;
}

void pruneAndMerge(bro::dom::Element* host,
                   bro::dom::Node*& caretNode, int& caretOff) {
    if (!host || !caretNode) return;

    if (caretNode->nodeType() == bro::dom::NodeType::Text &&
        static_cast<bro::dom::TextNode*>(caretNode)->length() == 0 &&
        caretNode->parentNode() && caretNode->parentNode() != host) {
        auto* p = caretNode->parentNode();
        p->removeChild(caretNode);
        caretNode = p;
        caretOff = static_cast<int>(p->childNodes().size());
    }
    while (caretNode != host && caretNode->childNodes().empty() &&
           caretNode->nodeType() == bro::dom::NodeType::Element &&
           caretNode->parentNode()) {
        auto* p = caretNode->parentNode();
        const int idx = indexInParent(caretNode);
        p->removeChild(caretNode);
        caretNode = p;
        caretOff = idx < 0 ? 0 : idx;
        if (p == host) break;
    }

    if (caretNode->nodeType() != bro::dom::NodeType::Element) return;
    auto& kids = caretNode->childNodes();
    if (caretOff <= 0 || caretOff >= static_cast<int>(kids.size())) return;
    auto* left = kids[caretOff - 1];
    auto* right = kids[caretOff];
    if (left->nodeType() != bro::dom::NodeType::Text ||
        right->nodeType() != bro::dom::NodeType::Text) return;
    auto* lt = static_cast<bro::dom::TextNode*>(left);
    auto* rt = static_cast<bro::dom::TextNode*>(right);
    const int join = static_cast<int>(lt->length());
    lt->appendData(rt->data());
    caretNode->removeChild(rt);
    caretNode = lt;
    caretOff = join;
}

constexpr char kNbspUtf8[] = "\xc2\xa0";

bool isRebalanceSpace(const std::string& s, size_t i, size_t& len) {
    if (s[i] == ' ') { len = 1; return true; }
    if (i + 1 < s.size() &&
        static_cast<unsigned char>(s[i]) == 0xC2 &&
        static_cast<unsigned char>(s[i + 1]) == 0xA0) { len = 2; return true; }
    return false;
}

bool rebalanceWhitespace(std::string& data, int& caret) {
    std::string out;
    out.reserve(data.size());
    int newCaret = caret;

    size_t i = 0;
    while (i < data.size()) {
        size_t len = 0;
        if (!isRebalanceSpace(data, i, len)) {
            if (static_cast<int>(i) == caret) newCaret = static_cast<int>(out.size());
            out += data[i];
            ++i;
            continue;
        }
        std::vector<size_t> srcOffsets;
        size_t j = i;
        while (j < data.size()) {
            size_t l = 0;
            if (!isRebalanceSpace(data, j, l)) break;
            srcOffsets.push_back(j);
            j += l;
        }
        const size_t n = srcOffsets.size();
        const bool atStart = (i == 0);
        const bool atEnd   = (j == data.size());

        for (size_t k = 0; k < n; ++k) {
            bool nbsp = (k % 2 == 0);
            if (k + 1 == n && atEnd) nbsp = true;
            if (n == 1 && !atStart && !atEnd) nbsp = false;
            if (static_cast<int>(srcOffsets[k]) == caret)
                newCaret = static_cast<int>(out.size());
            if (nbsp) out += kNbspUtf8; else out += ' ';
        }
        if (static_cast<int>(j) == caret) newCaret = static_cast<int>(out.size());
        i = j;
    }
    if (static_cast<int>(data.size()) == caret) newCaret = static_cast<int>(out.size());

    bool changed = (out != data);
    if (changed) { data = std::move(out); caret = newCaret; }
    return changed;
}

bool collapsesWhitespace(bro::dom::Node* textNode) {
    for (bro::dom::Node* n = textNode; n; n = n->parentNode()) {
        if (n->nodeType() != bro::dom::NodeType::Element) continue;
        const auto& style = static_cast<bro::dom::Element*>(n)->computedStyle();
        auto it = style.find("white-space");
        if (it == style.end()) continue;
        return it->second == "normal" || it->second == "nowrap";
    }
    return true;
}

void rebalanceAfterEdit(bro::dom::Document* doc,
                        bro::dom::TextNode* tn, int caret) {
    if (!tn || !collapsesWhitespace(tn)) return;
    std::string data = tn->data();
    int newCaret = caret;
    if (!rebalanceWhitespace(data, newCaret)) return;
    tn->setData(data);
    doc->selection()->collapse(tn, newCaret);
}

struct EditCaret {
    bro::dom::Selection* sel = nullptr;
    bro::dom::Node* node = nullptr;
    int offset = 0;
    bro::dom::TextNode* text = nullptr;
    bro::dom::Element* host = nullptr;
    explicit operator bool() const { return sel && node && host; }
};

EditCaret editCaretOf(bro::dom::Document* doc) {
    EditCaret c;
    if (!doc) return c;
    auto* sel = doc->selection();
    if (!sel || sel->rangeCount() == 0) return c;
    auto* focusN = sel->focusNode();
    if (!focusN || !inEditableHost(focusN)) return c;
    c.sel = sel;
    c.node = focusN;
    c.offset = sel->focusOffset();
    c.text = (focusN->nodeType() == bro::dom::NodeType::Text)
                 ? static_cast<bro::dom::TextNode*>(focusN) : nullptr;
    c.host = editableHostOf(focusN);
    return c;
}

std::string normalizeCommand(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name)
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(ch))));
    return out;
}

} // namespace

dom::TextNode* selectionCaretTextPosition(dom::Document* doc,
                                         int& off, bool& created) {
    off = 0;
    created = false;
    if (!doc) return nullptr;
    auto* sel = doc->selection();
    if (!sel || sel->rangeCount() == 0) return nullptr;
    auto* range = sel->getRangeAt(0);
    if (!range) return nullptr;

    dom::Node* caretNode = nullptr;
    int caretOff = 0;
    if (!range->collapsed()) {
        deleteRangeContents(doc, *range, caretNode, caretOff);
    } else {
        caretNode = range->startContainer();
        caretOff = range->startOffset();
    }
    if (!caretNode) return nullptr;

    if (caretNode->nodeType() == dom::NodeType::Text) {
        off = caretOff;
        return static_cast<dom::TextNode*>(caretNode);
    }
    if (caretNode->nodeType() == dom::NodeType::Element) {
        auto* el = static_cast<dom::Element*>(caretNode);
        auto* tn = doc->createTextNode("");
        auto& kids = el->childNodes();
        if (caretOff >= static_cast<int>(kids.size())) {
            el->appendChild(tn);
        } else {
            el->insertBefore(tn, kids[caretOff]);
        }
        created = true;
        return tn;
    }
    return nullptr;
}

void selectionInsertText(dom::Document* doc, const std::string& text) {
    int off = 0;
    bool created = false;
    auto* tn = selectionCaretTextPosition(doc, off, created);
    if (!tn) return;
    tn->insertData(static_cast<size_t>(off), text);
    const int caret = off + static_cast<int>(text.size());
    doc->selection()->collapse(tn, caret);
    rebalanceAfterEdit(doc, tn, caret);
}

bool Engine::editDeleteAtCaret(bool backward) {
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    const std::string inputType =
        backward ? "deleteContentBackward" : "deleteContentForward";
    runEditableMutation(document_.get(), c.node, inputType, "",
        [&] {
            auto* range = c.sel->getRangeAt(0);
            if (!range) return;
            if (!range->collapsed()) {
                EditUndoScope undo(&editUndo_, document_.get(), c.host,
                                   nullptr, DomUndoStack::Kind::Discrete);
                dom::Node* after = nullptr; int afterOff = 0;
                deleteRangeContents(document_.get(), *range, after, afterOff);
                if (after) c.sel->collapse(after, afterOff);
                undo.commit();
                return;
            }

            const EditDeleteTarget target =
                resolveDeleteTarget(c.node, c.offset, c.host, backward ? -1 : 1);
            if (!target.text && !target.remove) return;

            const bool sameNodeSplice =
                target.text && target.text == c.text &&
                static_cast<int>(target.text->length()) >
                    (target.end - target.start);
            EditUndoScope undo(
                &editUndo_, document_.get(), c.host,
                sameNodeSplice ? target.text : nullptr,
                sameNodeSplice
                    ? (backward ? DomUndoStack::Kind::Backspace
                                : DomUndoStack::Kind::DeleteForward)
                    : DomUndoStack::Kind::Discrete);

            dom::Node* caretNode = nullptr;
            int caretOff = 0;
            if (target.remove) {
                auto* p = target.remove->parentNode();
                const int idx = indexInParent(target.remove);
                if (!p) return;
                p->removeChild(target.remove);
                caretNode = p;
                caretOff = idx < 0 ? 0 : idx;
            } else {
                target.text->deleteData(target.start,
                                        target.end - target.start);
                caretNode = target.text;
                caretOff = target.start;
            }
            if (!sameNodeSplice)
                pruneAndMerge(c.host, caretNode, caretOff);
            if (caretNode) c.sel->collapse(caretNode, caretOff);
            undo.commit();
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editInsertLineBreak() {
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    runEditableMutation(document_.get(), c.node,
        "insertLineBreak", "\n",
        [&] {
            EditUndoScope undo(&editUndo_, document_.get(), c.host,
                               nullptr, DomUndoStack::Kind::Discrete);
            auto* range = c.sel->getRangeAt(0);
            if (!range) return;
            if (!range->collapsed()) {
                dom::Node* after = nullptr; int afterOff = 0;
                deleteRangeContents(document_.get(), *range, after, afterOff);
                if (after) c.sel->collapse(after, afterOff);
            }
            auto* br = document_->createElement("BR");
            range = c.sel->getRangeAt(0);
            if (range) range->insertNode(br);
            if (br->parentNode()) {
                auto* p = br->parentNode();
                const auto& kids = p->childNodes();
                for (size_t i = 0; i < kids.size(); ++i) {
                    if (kids[i] == br) {
                        c.sel->collapse(p, static_cast<int>(i + 1));
                        break;
                    }
                }
            }
            undo.commit();
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editInsertTextAtSelection(const std::string& text) {
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    runEditableMutation(document_.get(), c.node,
        "insertText", text,
        [&] {
            auto* r = c.sel->getRangeAt(0);
            if (!r) return;
            if (!r->collapsed()) {
                EditUndoScope undo(&editUndo_, document_.get(), c.host,
                                   nullptr, DomUndoStack::Kind::Discrete);
                selectionInsertText(document_.get(), text);
                undo.commit();
                return;
            }
            int off = 0;
            bool created = false;
            auto* tn = selectionCaretTextPosition(document_.get(), off, created);
            if (!tn) return;
            EditUndoScope undo(&editUndo_, document_.get(), c.host, tn,
                               DomUndoStack::Kind::Typing);
            tn->insertData(static_cast<size_t>(off), text);
            const int caret = off + static_cast<int>(text.size());
            document_->selection()->collapse(tn, caret);
            rebalanceAfterEdit(document_.get(), tn, caret);
            undo.commit();
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editHistoryStep(bool redo) {
    // A focused <input>/<textarea> keeps its own history (typing and
    // setRangeText both record into it); execCommand('undo'/'redo') steps
    // that one, as primary+Z / primary+Y do.
    if (dom::Element* active = document_ ? document_->activeElement() : nullptr) {
        layout::KeyHandleResult r;
        const int keycode = redo ? SDLK_Y : SDLK_Z;
        if (auto* in = getElInput(active); in && in->isFocused())
            r = in->handleKeyDown(active, keycode, SDL_KMOD_CTRL);
        else if (auto* ta = getElTextarea(active); ta && ta->isFocused())
            r = ta->handleKeyDown(active, keycode, SDL_KMOD_CTRL);
        else
            r.handled = false;
        if (r.handled) {
            const bool stepped = r.dispatchInput;
            applyKeyResult(active, r);
            return stepped;
        }
    }
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    auto* stack = editUndo_.find(c.host);
    if (!stack) return false;
    if (redo ? !stack->canRedo() : !stack->canUndo()) return false;
    const char* inputType = redo ? "historyRedo" : "historyUndo";
    runEditableMutation(document_.get(), c.node, inputType, "",
        [&] {
            if (redo) stack->redo(document_.get(), c.host);
            else stack->undo(document_.get(), c.host);
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editSelectAll() {
    if (!document_) return false;
    auto* sel = document_->selection();
    if (!sel) return false;
    dom::Node* host = (sel->rangeCount() > 0) ? sel->focusNode() : nullptr;
    while (host && host->nodeType() != dom::NodeType::Element)
        host = host->parentNode();
    auto* el = static_cast<dom::Element*>(host);
    while (el && !el->hasAttribute("contenteditable"))
        el = el->parentElement();
    dom::Node* target = el ? static_cast<dom::Node*>(el)
                           : static_cast<dom::Node*>(document_->body());
    if (!target) return false;
    sel->selectAllChildren(target);
    uiDirty_ = true;
    return true;
}

bool Engine::queryCommandSupported(const std::string& name) const {
    const std::string cmd = normalizeCommand(name);
    return cmd == "inserttext" || cmd == "insertlinebreak" ||
           cmd == "insertparagraph" || cmd == "delete" ||
           cmd == "forwarddelete" || cmd == "undo" || cmd == "redo" ||
           cmd == "selectall" || cmd == "copy" || cmd == "cut" ||
           cmd == "paste";
}

bool Engine::queryCommandEnabled(const std::string& name) {
    if (!queryCommandSupported(name)) return false;
    const std::string cmd = normalizeCommand(name);
    if (cmd == "selectall") return document_ && document_->body() != nullptr;
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    if (cmd == "undo" || cmd == "redo") {
        auto* stack = editUndo_.find(c.host);
        if (!stack) return false;
        return (cmd == "redo") ? stack->canRedo() : stack->canUndo();
    }
    if (cmd == "copy" || cmd == "cut") return !c.sel->isCollapsed();
    return true;
}

bool Engine::queryCommandState(const std::string& /*name*/) const {
    return false;
}

std::string Engine::queryCommandValue(const std::string& /*name*/) const {
    return "";
}

bool Engine::execCommand(const std::string& name, bool /*showUI*/,
                         const std::string& value) {
    if (!document_) return false;
    const std::string cmd = normalizeCommand(name);

    if (cmd == "inserttext") {
        if (value.empty()) return editCaretOf(document_.get()) ? true : false;
        return editInsertTextAtSelection(value);
    }
    if (cmd == "insertlinebreak" || cmd == "insertparagraph")
        return editInsertLineBreak();
    if (cmd == "delete") return editDeleteAtCaret(/*backward=*/true);
    if (cmd == "forwarddelete") return editDeleteAtCaret(/*backward=*/false);
    if (cmd == "undo") return editHistoryStep(/*redo=*/false);
    if (cmd == "redo") return editHistoryStep(/*redo=*/true);
    if (cmd == "selectall") return editSelectAll();
    if (cmd == "copy") {
        const std::string text = simulateCopy();
        if (text.empty()) return false;
        return platform::setClipboardText(text);
    }
    if (cmd == "cut") {
        bool stored = false;
        const std::string text = simulateCut([&](const std::string& t) {
            stored = platform::setClipboardText(t);
            return stored;
        });
        return !text.empty() && stored;
    }
    if (cmd == "paste") {
        const std::string text = platform::getClipboardText();
        if (text.empty()) return false;
        if (!editCaretOf(document_.get())) return false;
        simulatePaste(text);
        return true;
    }
    return false;
}

} // namespace bro::engine
