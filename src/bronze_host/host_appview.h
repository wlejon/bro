#pragma once

// <appview src="appdir">: a folder app running inline in its host page, as a
// compartment of its own (host_compartment.h) with its own manifest identity
// and grants. The engine hosts it as it hosts an <iframe> (its own layer,
// input routed in, Engine::syncIframes); what differs is who it is:
//
//   - bro.app inside it is the app's own (id, name, folders), from its
//     bro.json; its grants are its own AppPermissions, settled from that
//     manifest and the grant store, never the host page's.
//   - A prompt for one of its undecided capabilities is a `permissionrequest`
//     event at the element, in the host page: detail {capability, question,
//     what, app}, with event.allow() / event.deny(). Answered in the
//     listener, that is the user's answer; preventDefault() without one
//     means the page asks in its own time (the use is refused meanwhile and
//     the later allow() / deny() is kept); unhandled, bro's own dialog asks.
//   - Its lifecycle is the page's: start(), stop(), reload(), `running`.
//   - It runs in time slices on a fiber of its own (host_compartment.h,
//     runEntry), so it cannot stall the page; an app the runtime stops is a
//     `terminated` event at the element, with the reason.
//   - The page grants it functions: expose(name, fn) appears inside as
//     bro.app.host[name], called across the membrane (host_membrane.h).

#include "bronze_host/host_class.h"

#include <string>

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

struct Compartment;
struct CompartmentSetup;

/// HTMLAppViewElement's members.
void decorateAppViewProto(ObjectBuilder& b);

/// Fill an appview compartment's setup from its folder: identity and grants.
void prepareAppView(engine::Engine& engine, CompartmentSetup& setup);

/// In the new compartment's realm, after its globals: bro.app.host with what
/// the page exposed, and its prompts routed to the page.
void attachAppViewCompartment(Compartment& c);

/// What the runtime tells the page about the app in `c`, as events at its
/// element dispatched at the next frame seam (dispatchAppViewEvents):
///   - terminated {reason, site}: the runtime stopped it (it ran over where
///     its page waits on it, or its fiber ended on a host error). Unless the
///     page cancels it, the stopped view shows an error card (a closed
///     shadow root of the element) until the app runs again.
///   - apperror {error}: an exception of the app's nothing caught. The app
///     runs on, as a web page does; the page may show it.
///   - unresponsive / responsive: a job of the app's has run kNotRespondingMs
///     without finishing (it yields, so the page goes on), and later did.
void noteAppViewTerminated(Compartment& c, const std::string& reason, const std::string& site);
void noteAppViewError(Compartment& c, const std::string& error);
void noteAppViewResponding(Compartment& c, bool responding);
void dispatchAppViewEvents();

}  // namespace bro::bronze_host
