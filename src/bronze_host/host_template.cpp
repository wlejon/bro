#include "bronze_host/host_template.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/document.h"
#include "dom/element.h"

namespace bro::bronze_host {

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
                       if (st->el->hasAttribute("data-bro-template-html")) {
                           std::string html = st->el->getAttribute("data-bro-template-html");
                           if (doc) doc->parseInnerHTML(frag, html);
                       }
                   }
                   return hostNodeValue(frag);
               },
               nullptr);
}

}  // namespace bro::bronze_host
