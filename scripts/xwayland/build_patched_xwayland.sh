#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PATCH_DIR="${ROOT_DIR}/patches/xwayland"
XSERVER_REPO="${XSERVER_REPO:-https://gitlab.freedesktop.org/xorg/xserver.git}"
XSERVER_TAG="${XSERVER_TAG:-xwayland-23.2.6}"
XSERVER_DIR="${XSERVER_DIR:-${ROOT_DIR}/third_party/xserver}"
BUILD_DIR="${BUILD_DIR:-${XSERVER_DIR}/build-bridge}"
PREFIX_DIR="${PREFIX_DIR:-${XSERVER_DIR}/_install-bridge}"
SECURE_RPC="${XSERVER_SECURE_RPC:-false}"

if [[ ! -d "${PATCH_DIR}" ]]; then
    echo "Patch directory not found: ${PATCH_DIR}" >&2
    exit 1
fi

if ! compgen -G "${PATCH_DIR}/*.patch" >/dev/null; then
    echo "No patches found under ${PATCH_DIR}" >&2
    exit 1
fi

if [[ ! -d "${XSERVER_DIR}/.git" ]]; then
    echo "Cloning xserver into ${XSERVER_DIR}"
    git clone "${XSERVER_REPO}" "${XSERVER_DIR}"
fi

echo "Preparing clean source tree at ${XSERVER_TAG}"
git -C "${XSERVER_DIR}" fetch --tags origin
git -C "${XSERVER_DIR}" checkout "${XSERVER_TAG}"
git -C "${XSERVER_DIR}" reset --hard "${XSERVER_TAG}"
git -C "${XSERVER_DIR}" clean -fd

echo "Applying local patch series"
for patch in "${PATCH_DIR}"/*.patch; do
    echo "  -> $(basename "${patch}")"
    git -C "${XSERVER_DIR}" apply --check "${patch}"
    git -C "${XSERVER_DIR}" apply "${patch}"
done

MESON_EXTRA_ARGS_ARRAY=()
if [[ -n "${MESON_EXTRA_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    MESON_EXTRA_ARGS_ARRAY=(${MESON_EXTRA_ARGS})
fi

echo "Configuring build directory ${BUILD_DIR}"
if [[ -d "${BUILD_DIR}" ]]; then
    meson setup --wipe "${BUILD_DIR}" \
        "${XSERVER_DIR}" \
        -Dprefix="${PREFIX_DIR}" \
        -Dbuildtype=debugoptimized \
        -Dsecure-rpc="${SECURE_RPC}" \
        "${MESON_EXTRA_ARGS_ARRAY[@]}"
else
    meson setup "${BUILD_DIR}" \
        "${XSERVER_DIR}" \
        -Dprefix="${PREFIX_DIR}" \
        -Dbuildtype=debugoptimized \
        -Dsecure-rpc="${SECURE_RPC}" \
        "${MESON_EXTRA_ARGS_ARRAY[@]}"
fi

echo "Building patched Xwayland"
meson compile -C "${BUILD_DIR}"

echo "Installing into ${PREFIX_DIR}"
meson install -C "${BUILD_DIR}"

echo
echo "Done. Patched binary:"
echo "  ${PREFIX_DIR}/bin/Xwayland"
