#pragma once

// Whether a document may load what one of its subresources names (an <img>
// or CSS url(), a font, an @import, a <video>, an <iframe> folder, a
// <script>/<link> fetched over http): the host's access gate (bronze's
// embed/embed_access.h), as bro's layers below the bronze host ask it. A file
// asks to read its path; an http(s) URL asks `net` for the URL. The bronze
// host installs the forwarder (bronze_host/host_access.cpp); until it does,
// and in a process without one, every load is allowed.
//
// A subresource load never asks the user: it is answered from what is
// decided (checkAccessWithoutAsking), so markup cannot raise a prompt, and a
// capability not yet decided is a refusal.

#include <string>

namespace bro::util {

/// Answers "" to allow, or the refusal's sentence: for connecting to the URL
/// `subject` when `net`, else for reading the absolute, lexically normal path
/// `subject`.
using SubresourceGate = std::string (*)(bool net, const std::string& subject);

void setSubresourceGate(SubresourceGate gate);

/// The gate's answer for loading `pathOrUrl`: an http(s) URL is asked as a
/// connection to it, anything else as a read of the path (made absolute and
/// lexically normal here). "" when allowed or when no gate is installed.
std::string subresourceRefusal(const std::string& pathOrUrl);

}  // namespace bro::util
