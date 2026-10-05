#pragma once

#include "bronze_host/host_builder.h"

namespace bro::layout { class ElTerminal; }

namespace bro::bronze_host {

// HTMLTerminalElement's prototype members: the child, reads, options, theme
// and metrics (host_element_terminal.cpp), and the view: scrollback,
// selection, search, links, shell commands, clipboard answers
// (host_element_terminal_view.cpp). docs/terminal-api.js is the contract.
void decorateTerminalProto(ObjectBuilder& b);
void decorateTerminalViewProto(ObjectBuilder& b);

// The element's controller, created on first use (null: not a <terminal>).
layout::ElTerminal* hostTerminalControl(Value self, bool create = true);

}  // namespace bro::bronze_host
