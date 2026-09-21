#pragma once

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "dom/event.h"

#include <string>

namespace bro::bronze_host {

const HostClass& mouseEventHostClass();
const HostClass& keyboardEventHostClass();
const HostClass& wheelEventHostClass();
const HostClass& focusEventHostClass();
const HostClass& customEventHostClass();

void installDomEventTypes();

int legacyKeyCodeFor(const std::string& code, const std::string& key);
void populateMouseEvent(ObjectBuilder& b, dom::Event& e);
void populateKeyboardEvent(ObjectBuilder& b, dom::Event& e);
void populateInputAndFormEvents(ObjectBuilder& b, dom::Event& e);
void populateClipboardEvent(ObjectBuilder& b, dom::Event& e);

}  // namespace bro::bronze_host
