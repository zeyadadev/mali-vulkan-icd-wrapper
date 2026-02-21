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
INSTALL_BUILD_DEPS="${XSERVER_INSTALL_BUILD_DEPS:-false}"
INSTALL_SYSTEM_XWAYLAND="${XSERVER_INSTALL_SYSTEM:-false}"
REBOOT_AFTER_INSTALL="${XSERVER_REBOOT_AFTER_INSTALL:-false}"
INTERACTIVE_MODE="${XSERVER_INTERACTIVE:-auto}"
SYSTEM_XWAYLAND_PATH="${XSERVER_SYSTEM_XWAYLAND_PATH:-/usr/bin/Xwayland}"
SYSTEM_XWAYLAND_BACKUP_PATH="${XSERVER_SYSTEM_XWAYLAND_BACKUP_PATH:-/usr/bin/Xwayland.orig}"

is_true() {
    case "${1,,}" in
        1|true|yes|on) return 0 ;;
        *) return 1 ;;
    esac
}

normalize_bool_var() {
    local var_name="$1"
    if is_true "${!var_name}"; then
        printf -v "${var_name}" "true"
    else
        printf -v "${var_name}" "false"
    fi
}

is_interactive_enabled() {
    case "${INTERACTIVE_MODE,,}" in
        auto)
            [[ -t 0 && -t 1 ]]
            ;;
        1|true|yes|on)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

prompt_yes_no() {
    local question="$1"
    local default_value="${2:-}"
    local prompt_suffix=""
    local answer=""

    if is_true "${default_value}"; then
        prompt_suffix="[Y/n]"
    elif [[ -n "${default_value}" ]]; then
        prompt_suffix="[y/N]"
    else
        prompt_suffix="[y/n]"
    fi

    while true; do
        read -r -p "${question} ${prompt_suffix} " answer
        answer="${answer,,}"

        if [[ -z "${answer}" ]]; then
            answer="${default_value,,}"
        fi

        case "${answer}" in
            y|yes|1|true|on) return 0 ;;
            n|no|0|false|off) return 1 ;;
        esac

        echo "Please answer yes or no."
    done
}

configure_interactive_options_if_requested() {
    if ! is_interactive_enabled; then
        return
    fi

    echo
    echo "Interactive mode enabled."
    echo "This script will:"
    echo "  - clone/reset/clean ${XSERVER_DIR}"
    echo "  - apply patches from ${PATCH_DIR}"
    echo "  - build/install into ${PREFIX_DIR}"
    echo

    if ! prompt_yes_no "Continue with this run?" "true"; then
        echo "Aborted by user."
        exit 0
    fi

    if prompt_yes_no "Install build dependencies before building?" "${INSTALL_BUILD_DEPS}"; then
        INSTALL_BUILD_DEPS="true"
    else
        INSTALL_BUILD_DEPS="false"
    fi

    if prompt_yes_no "Install patched Xwayland to ${SYSTEM_XWAYLAND_PATH}?" "${INSTALL_SYSTEM_XWAYLAND}"; then
        INSTALL_SYSTEM_XWAYLAND="true"
    else
        INSTALL_SYSTEM_XWAYLAND="false"
    fi

    if is_true "${INSTALL_SYSTEM_XWAYLAND}"; then
        if prompt_yes_no "Reboot automatically after successful install?" "${REBOOT_AFTER_INSTALL}"; then
            REBOOT_AFTER_INSTALL="true"
        else
            REBOOT_AFTER_INSTALL="false"
        fi
    else
        REBOOT_AFTER_INSTALL="false"
    fi

    echo
    echo "Selected options:"
    echo "  install dependencies : ${INSTALL_BUILD_DEPS}"
    echo "  system install       : ${INSTALL_SYSTEM_XWAYLAND}"
    echo "  reboot after install : ${REBOOT_AFTER_INSTALL}"
    echo

    if ! prompt_yes_no "Proceed? WARNING: ${XSERVER_DIR} will be reset/cleaned." "true"; then
        echo "Aborted by user."
        exit 0
    fi
}

run_as_root() {
    if [[ "${EUID}" -eq 0 ]]; then
        "$@"
        return
    fi

    if command -v sudo >/dev/null 2>&1; then
        sudo "$@"
        return
    fi

    echo "This step requires root privileges, but sudo is not available." >&2
    exit 1
}

install_build_dependencies_if_requested() {
    if ! is_true "${INSTALL_BUILD_DEPS}"; then
        return
    fi

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "Dependency auto-install currently supports apt-get systems only." >&2
        exit 1
    fi

    local pkgs=(
        git
        meson
        ninja-build
        ccache
        pkg-config
        libxkbfile-dev
        libxshmfence-dev
        libxfont-dev
        libfontenc-dev
        libxcvt-dev
        libwayland-dev
        wayland-protocols
        libdrm-dev
        libepoxy-dev
    )

    if is_true "${SECURE_RPC}"; then
        pkgs+=(libtirpc-dev)
    fi

    echo "Installing build dependencies via apt-get"
    run_as_root apt-get update
    run_as_root apt-get install -y "${pkgs[@]}"
}

install_system_xwayland_if_requested() {
    if ! is_true "${INSTALL_SYSTEM_XWAYLAND}"; then
        return
    fi

    local built_xwayland="${PREFIX_DIR}/bin/Xwayland"
    if [[ ! -x "${built_xwayland}" ]]; then
        echo "Built Xwayland not found at ${built_xwayland}" >&2
        exit 1
    fi

    if [[ -f "${SYSTEM_XWAYLAND_PATH}" && ! -e "${SYSTEM_XWAYLAND_BACKUP_PATH}" ]]; then
        echo "Creating backup: ${SYSTEM_XWAYLAND_PATH} -> ${SYSTEM_XWAYLAND_BACKUP_PATH}"
        run_as_root cp "${SYSTEM_XWAYLAND_PATH}" "${SYSTEM_XWAYLAND_BACKUP_PATH}"
    fi

    echo "Installing patched Xwayland to ${SYSTEM_XWAYLAND_PATH}"
    run_as_root install -m 0755 "${built_xwayland}" "${SYSTEM_XWAYLAND_PATH}"
}

reboot_if_requested() {
    if ! is_true "${REBOOT_AFTER_INSTALL}"; then
        return
    fi

    if is_interactive_enabled; then
        if ! prompt_yes_no "Reboot now?" "true"; then
            echo "Skipping reboot."
            return
        fi
    fi

    echo "Reboot requested by XSERVER_REBOOT_AFTER_INSTALL=1"
    run_as_root systemctl reboot
}

normalize_bool_var "SECURE_RPC"
normalize_bool_var "INSTALL_BUILD_DEPS"
normalize_bool_var "INSTALL_SYSTEM_XWAYLAND"
normalize_bool_var "REBOOT_AFTER_INSTALL"
configure_interactive_options_if_requested
install_build_dependencies_if_requested

for cmd in git meson; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "Required tool missing: ${cmd}" >&2
        exit 1
    fi
done

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
install_system_xwayland_if_requested

echo
echo "Done. Patched binary:"
echo "  ${PREFIX_DIR}/bin/Xwayland"
if is_true "${INSTALL_SYSTEM_XWAYLAND}"; then
    echo "Installed system binary:"
    echo "  ${SYSTEM_XWAYLAND_PATH}"
fi
if is_true "${REBOOT_AFTER_INSTALL}"; then
    echo "System reboot requested."
fi

reboot_if_requested
