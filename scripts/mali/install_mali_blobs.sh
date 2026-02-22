#!/usr/bin/env bash
set -euo pipefail

INTERACTIVE_MODE="${MALI_INSTALL_INTERACTIVE:-auto}"
INSTALL_64BIT="${MALI_INSTALL_64BIT:-true}"
INSTALL_32BIT="${MALI_INSTALL_32BIT:-false}"
REMOVE_CONFLICTING_ICD="${MALI_REMOVE_CONFLICTING_ICD:-true}"
DOWNLOAD_DIR="${MALI_DOWNLOAD_DIR:-/tmp/mali-wrapper-blobs}"
MALI_64_DEB_URL="${MALI_64_DEB_URL:-https://github.com/ginkage/libmali-rockchip/releases/download/v1.9-1-04f8711/libmali-valhall-g610-g24p0-wayland-gbm_1.9-1_arm64.deb}"
MALI_32_BLOB_URL="${MALI_32_BLOB_URL:-https://github.com/ginkage/libmali-rockchip/raw/refs/heads/master/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so}"
MALI_32_TARGET="${MALI_32_TARGET:-/usr/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so}"
MALI_32_SYMLINK="${MALI_32_SYMLINK:-/usr/lib/arm-linux-gnueabihf/libmali.so}"

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
        auto) [[ -t 0 && -t 1 ]] ;;
        1|true|yes|on) return 0 ;;
        *) return 1 ;;
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

configure_interactive_options_if_requested() {
    if ! is_interactive_enabled; then
        return
    fi

    echo
    echo "Interactive mode enabled."
    echo "This script can:"
    echo "  - install 64-bit Mali blob package"
    echo "  - install 32-bit Mali blob + libmali.so symlink"
    echo "  - remove known conflicting Vulkan ICD/layer files"
    echo

    if ! prompt_yes_no "Continue with this run?" "true"; then
        echo "Aborted by user."
        exit 0
    fi

    if prompt_yes_no "Install 64-bit Mali blob package?" "${INSTALL_64BIT}"; then
        INSTALL_64BIT="true"
    else
        INSTALL_64BIT="false"
    fi

    if prompt_yes_no "Install 32-bit Mali blob?" "${INSTALL_32BIT}"; then
        INSTALL_32BIT="true"
    else
        INSTALL_32BIT="false"
    fi

    if prompt_yes_no "Remove known conflicting Vulkan ICD/layer files?" "${REMOVE_CONFLICTING_ICD}"; then
        REMOVE_CONFLICTING_ICD="true"
    else
        REMOVE_CONFLICTING_ICD="false"
    fi

    if ! is_true "${INSTALL_64BIT}" && ! is_true "${INSTALL_32BIT}" && ! is_true "${REMOVE_CONFLICTING_ICD}"; then
        echo "Nothing selected." >&2
        exit 1
    fi

    echo
    echo "Selected options:"
    echo "  install 64-bit blob      : ${INSTALL_64BIT}"
    echo "  install 32-bit blob      : ${INSTALL_32BIT}"
    echo "  remove conflicting files : ${REMOVE_CONFLICTING_ICD}"
    echo "  64-bit package URL       : ${MALI_64_DEB_URL}"
    echo "  32-bit blob URL          : ${MALI_32_BLOB_URL}"
    echo "  32-bit target            : ${MALI_32_TARGET}"
    echo "  32-bit symlink           : ${MALI_32_SYMLINK}"
    echo

    if ! prompt_yes_no "Proceed?" "true"; then
        echo "Aborted by user."
        exit 0
    fi
}

download_file() {
    local url="$1"
    local out_path="$2"
    echo "Downloading ${url}"
    wget -O "${out_path}" "${url}"
}

install_64bit_blob_if_requested() {
    if ! is_true "${INSTALL_64BIT}"; then
        return
    fi

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "apt-get is required for 64-bit package installation." >&2
        exit 1
    fi

    local deb_name="${MALI_64_DEB_URL##*/}"
    local deb_path=""
    deb_name="${deb_name%%\?*}"
    deb_path="${DOWNLOAD_DIR}/${deb_name}"

    mkdir -p "${DOWNLOAD_DIR}"
    download_file "${MALI_64_DEB_URL}" "${deb_path}"

    echo "Installing 64-bit Mali package"
    run_as_root apt-get install -y "${deb_path}"
}

install_32bit_blob_if_requested() {
    if ! is_true "${INSTALL_32BIT}"; then
        return
    fi

    local blob_name="${MALI_32_BLOB_URL##*/}"
    local tmp_blob_path=""
    blob_name="${blob_name%%\?*}"
    tmp_blob_path="${DOWNLOAD_DIR}/${blob_name}"

    mkdir -p "${DOWNLOAD_DIR}"
    download_file "${MALI_32_BLOB_URL}" "${tmp_blob_path}"

    echo "Installing 32-bit Mali blob to ${MALI_32_TARGET}"
    run_as_root install -D -m 0644 "${tmp_blob_path}" "${MALI_32_TARGET}"

    echo "Updating symlink ${MALI_32_SYMLINK} -> ${MALI_32_TARGET}"
    run_as_root ln -sf "${MALI_32_TARGET}" "${MALI_32_SYMLINK}"
}

remove_conflicting_files_if_requested() {
    if ! is_true "${REMOVE_CONFLICTING_ICD}"; then
        return
    fi

    local files=(
        /usr/share/vulkan/icd.d/mali.json
        /etc/vulkan/icd.d/mali.json
        /usr/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
        /etc/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
    )

    echo "Removing known conflicting Vulkan files"
    run_as_root rm -f -- "${files[@]}"
}

print_final_summary() {
    echo
    echo "Done."

    if is_true "${INSTALL_64BIT}"; then
        if [[ -e /usr/lib/aarch64-linux-gnu/libmali.so ]]; then
            echo "  64-bit libmali: /usr/lib/aarch64-linux-gnu/libmali.so -> $(readlink -f /usr/lib/aarch64-linux-gnu/libmali.so)"
        else
            echo "  WARN: /usr/lib/aarch64-linux-gnu/libmali.so not found after install"
        fi
    fi

    if is_true "${INSTALL_32BIT}"; then
        if [[ -e "${MALI_32_SYMLINK}" ]]; then
            echo "  32-bit libmali: ${MALI_32_SYMLINK} -> $(readlink -f "${MALI_32_SYMLINK}")"
        else
            echo "  WARN: ${MALI_32_SYMLINK} not found after install"
        fi
    fi

    if is_true "${REMOVE_CONFLICTING_ICD}"; then
        echo "  Removed known conflicting Vulkan files (mali.json + legacy WSI layer JSON)."
    fi
}

normalize_bool_var "INSTALL_64BIT"
normalize_bool_var "INSTALL_32BIT"
normalize_bool_var "REMOVE_CONFLICTING_ICD"
configure_interactive_options_if_requested

if ! is_true "${INSTALL_64BIT}" && ! is_true "${INSTALL_32BIT}" && ! is_true "${REMOVE_CONFLICTING_ICD}"; then
    echo "Nothing to do." >&2
    exit 1
fi

if ! command -v wget >/dev/null 2>&1 && { is_true "${INSTALL_64BIT}" || is_true "${INSTALL_32BIT}"; }; then
    echo "wget is required to download Mali blobs." >&2
    exit 1
fi

install_64bit_blob_if_requested
install_32bit_blob_if_requested
remove_conflicting_files_if_requested
print_final_summary
