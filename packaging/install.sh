#!/usr/bin/env bash
# Install script for bro runtime, desktop session, and shell resources.
#
# Usage:
#   sudo ./packaging/install.sh [--prefix /usr/local]
#   ./packaging/install.sh --prefix "$HOME/.local"
#   ./packaging/install.sh --uninstall [--prefix /usr/local]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX=""
DESTDIR=""
UNINSTALL=0
BUILD_DIR="${REPO_ROOT}/build"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --destdir)
            DESTDIR="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--prefix PATH] [--destdir PATH] [--build-dir PATH] [--uninstall]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# Default prefix detection: /usr/local for root, ~/.local for non-root
if [[ -z "$PREFIX" ]]; then
    if [[ $EUID -eq 0 ]]; then
        PREFIX="/usr/local"
    else
        PREFIX="$HOME/.local"
    fi
fi

TARGET_PREFIX="${DESTDIR}${PREFIX}"
BIN_DIR="${TARGET_PREFIX}/bin"
SHARE_DIR="${TARGET_PREFIX}/share"
BRO_SHARE="${SHARE_DIR}/bro"
SYSTEM_DIR="${BRO_SHARE}/system"
APPS_DIR="${BRO_SHARE}/apps"
SESSIONS_DIR="${SHARE_DIR}/wayland-sessions"
APPLICATIONS_DIR="${SHARE_DIR}/applications"
ICONS_DIR="${SHARE_DIR}/icons/hicolor/256x256/apps"
SYSTEMD_USER_DIR="${TARGET_PREFIX}/lib/systemd/user"

if [[ "$UNINSTALL" -eq 1 ]]; then
    echo ">>> Uninstalling bro from ${TARGET_PREFIX}..."
    rm -f "${BIN_DIR}/bro" "${BIN_DIR}/bro-headless" "${BIN_DIR}/bro-server"
    rm -rf "${BRO_SHARE}"
    rm -f "${SESSIONS_DIR}/bro.desktop"
    rm -f "${APPLICATIONS_DIR}/bro.desktop"
    rm -f "${ICONS_DIR}/bro.png"
    rm -f "${SYSTEMD_USER_DIR}/bro-session.target"
    echo ">>> Uninstallation complete."
    exit 0
fi

echo ">>> Installing bro desktop to ${TARGET_PREFIX}..."

# Find binaries to install: check build directory or staged directory
BRO_BIN=""
for cand in "${BUILD_DIR}/bro" "${BUILD_DIR}/Release/bro" "${REPO_ROOT}/dist"/*/bro; do
    if [[ -x "$cand" ]]; then
        BRO_BIN="$cand"
        break
    fi
done

if [[ -z "$BRO_BIN" ]]; then
    echo "ERROR: bro executable not found. Build first: cmake --build ${BUILD_DIR}" >&2
    exit 1
fi

SRC_BIN_DIR="$(dirname "$BRO_BIN")"

# 1. Binary directory
mkdir -p "${BIN_DIR}"
for bin in bro bro-headless bro-server; do
    if [[ -x "${SRC_BIN_DIR}/${bin}" ]]; then
        install -m 755 "${SRC_BIN_DIR}/${bin}" "${BIN_DIR}/${bin}"
        echo "  Installed ${BIN_DIR}/${bin}"
    fi
done

# Shared libraries beside binaries (e.g. libbronze_shared.so)
shopt -s nullglob
for lib in "${SRC_BIN_DIR}"/*.so*; do
    install -m 755 "$lib" "${BIN_DIR}/"
    echo "  Installed $(basename "$lib") to ${BIN_DIR}/"
done
shopt -u nullglob

# 2. System and assets directory
mkdir -p "${SYSTEM_DIR}"
if [[ -d "${REPO_ROOT}/system" ]]; then
    cp -a "${REPO_ROOT}/system/." "${SYSTEM_DIR}/"
    echo "  Installed system UI resources to ${SYSTEM_DIR}"
fi

# 3. Trusted shell applications directory
# Desktop trust model grants shell privileges to apps located in $PREFIX/share/bro/apps
mkdir -p "${APPS_DIR}"
chmod 755 "${APPS_DIR}"
echo "  Prepared trusted desktop shell applications directory: ${APPS_DIR}"

# 4. Wayland session entry for display managers (GDM, SDDM, LightDM)
mkdir -p "${SESSIONS_DIR}"
install -m 644 "${SCRIPT_DIR}/linux/wayland-sessions/bro.desktop" "${SESSIONS_DIR}/bro.desktop"
echo "  Registered Wayland session: ${SESSIONS_DIR}/bro.desktop"

# 5. Application menu launcher entry
mkdir -p "${APPLICATIONS_DIR}"
install -m 644 "${SCRIPT_DIR}/linux/applications/bro.desktop" "${APPLICATIONS_DIR}/bro.desktop"
echo "  Installed application entry: ${APPLICATIONS_DIR}/bro.desktop"

# 6. Desktop icon
mkdir -p "${ICONS_DIR}"
if [[ -f "${REPO_ROOT}/system/icon.png" ]]; then
    install -m 644 "${REPO_ROOT}/system/icon.png" "${ICONS_DIR}/bro.png"
    echo "  Installed icon: ${ICONS_DIR}/bro.png"
fi

# 7. Systemd user session target
mkdir -p "${SYSTEMD_USER_DIR}"
install -m 644 "${SCRIPT_DIR}/linux/systemd/user/bro-session.target" "${SYSTEMD_USER_DIR}/bro-session.target"
echo "  Installed systemd user target: ${SYSTEMD_USER_DIR}/bro-session.target"

echo ""
echo ">>> bro desktop installation successfully finished."
echo "    Session: select 'bro' at display manager login or run 'bro --drm' directly from a VT."
