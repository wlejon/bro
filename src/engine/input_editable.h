#pragma once

#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "engine/dom_undo.h"

#include <string>

namespace bro::engine {

class EditUndoScope {
public:
    EditUndoScope(DomUndoHistories* hist, dom::Document* doc,
                  dom::Element* host, dom::TextNode* tn,
                  DomUndoStack::Kind kind);
    void commit();

private:
    DomUndoHistories* hist_;
    dom::Document* doc_;
    dom::Element* host_;
    dom::TextNode* tn_;
    DomUndoStack::Kind kind_;
    std::string beforeData_;
    std::string beforeHTML_;
    DomUndoStack::Sel selBefore_;
};

void selectionInsertText(dom::Document* doc, const std::string& text);
dom::TextNode* selectionCaretTextPosition(dom::Document* doc, int& off, bool& created);

} // namespace bro::engine
