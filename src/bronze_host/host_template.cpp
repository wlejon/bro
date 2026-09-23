#include "bronze_host/host_template.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/document.h"
#include "dom/element.h"

namespace bro::bronze_host {

// `template.content`: the inert DocumentFragment the parser (and a template's
// innerHTML setter, Element::setInnerHTML) fills. A template made by
// createElement has none until something reads or writes it, so the first
// read mints an empty one; after that it is the same fragment every time.
void decorateTemplateProto(ObjectBuilder& b) {
    b.accessor("content",
               [](Value self_, std::span<const Value>) -> Value {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::undefined();
                   if (st->el->tagName() != "TEMPLATE" && st->el->tagName() != "template")
                       return ev::undefined();

                   dom::Element* frag = st->el->templateContent();
                   if (!frag) {
                       dom::Document* doc = st->el->document();
                       if (doc) {
                           frag = doc->createElement("#DOCUMENT-FRAGMENT");
                       } else {
                           frag = new dom::Element("#DOCUMENT-FRAGMENT");
                       }
                       st->el->setTemplateContent(frag);
                   }
                   return hostNodeValue(frag);
               },
               nullptr);
}

}  // namespace bro::bronze_host
