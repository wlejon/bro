#pragma once

#include <string>
#include <vector>

namespace bro::engine {

/// Evaluates whether an app directory is located in a trusted desktop location
/// and resolves granted privileged shell namespaces.
struct DesktopTrustInfo {
    bool isTrusted = false;
    bool isShell = false;
    std::vector<std::string> grantedPrivileges;

    bool hasPrivilege(const std::string& ns) const {
        if (!isTrusted) return false;
        if (isShell) return true;
        for (const auto& p : grantedPrivileges) {
            if (p == ns || p == "*") return true;
        }
        return false;
    }
};

/// Check if an app directory qualifies as a trusted desktop install location:
/// under a directory bro ships apps in, an OS install prefix, or one named in
/// BRO_TRUSTED_APP_DIR. Never decided by the app's own folder or manifest.
bool isTrustedAppLocation(const std::string& appDir);

/// Evaluates trust and grants requested privileged shell namespaces. An app
/// gets what it asks for (`"shell": true`, `"permissions": [...]`) when it is
/// installed in a trusted location, or when the user's own permissions file
/// grants it to `appId` (app_manifest.h, userPermissionGrants). Asking alone
/// grants nothing.
DesktopTrustInfo evaluateDesktopTrust(const std::string& appDir,
                                      const std::string& appId,
                                      bool requestedShell,
                                      const std::vector<std::string>& requestedPrivileges);

} // namespace bro::engine
