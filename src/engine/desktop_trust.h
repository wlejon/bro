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

/// Check if an app directory qualifies as a trusted desktop install location.
bool isTrustedAppLocation(const std::string& appDir, const std::string& projectRoot = {});

/// Evaluates trust and grants requested privileged shell namespaces.
DesktopTrustInfo evaluateDesktopTrust(const std::string& appDir,
                                      const std::string& projectRoot,
                                      bool requestedShell,
                                      const std::vector<std::string>& requestedPrivileges);

} // namespace bro::engine
