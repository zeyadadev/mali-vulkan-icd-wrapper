#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PATCH_DIR="${ROOT_DIR}/patches/kernel"
PATCH_FILE="${KERNEL_PATCH_FILE:-}"
KERNEL_REPO="${KERNEL_REPO:-https://github.com/zeyadadev/linux-rockchip.git}"
KERNEL_BRANCH="${KERNEL_BRANCH:-rk-6.1-rkr6.1}"
KERNEL_DIR="${KERNEL_DIR:-${ROOT_DIR}/third_party/linux-rockchip}"
KERNEL_ARCH="${KERNEL_ARCH:-arm64}"
KERNEL_JOBS="${KERNEL_JOBS:-$(nproc)}"
KERNEL_LOCALVERSION="${KERNEL_LOCALVERSION:--low32alias}"
KERNEL_PACKAGE_VERSION="${KERNEL_PACKAGE_VERSION:-1$(date +%Y%m%d%H%M)}"
KERNEL_CONFIG_SOURCE="${KERNEL_CONFIG_SOURCE:-}"
INSTALL_BUILD_DEPS="${KERNEL_INSTALL_BUILD_DEPS:-false}"
RESET_TREE="${KERNEL_RESET_TREE:-true}"
APPLY_PATCH="${KERNEL_APPLY_PATCH:-true}"
INSTALL_PACKAGES="${KERNEL_INSTALL_PACKAGES:-false}"
REBOOT_AFTER_INSTALL="${KERNEL_REBOOT_AFTER_INSTALL:-false}"
INTERACTIVE_MODE="${KERNEL_INTERACTIVE:-auto}"
KERNEL_MALI_DRIVER="${KERNEL_MALI_DRIVER:-valhall}"
BUILD_START_STAMP=""
RESOLVED_PATCH_FILE=""

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

resolve_kernel_patch_file() {
    if [[ -n "${PATCH_FILE}" ]]; then
        printf '%s\n' "${PATCH_FILE}"
        return
    fi

    case "${KERNEL_MALI_DRIVER,,}" in
        valhall)
            printf '%s\n' "${PATCH_DIR}/0001-mali-add-valhall-low32-alias-mapping-support.patch"
            ;;
        bifrost)
            printf '%s\n' "${PATCH_DIR}/0002-mali-add-bifrost-low32-alias-mapping-support.patch"
            ;;
        *)
            echo "Unsupported KERNEL_MALI_DRIVER: ${KERNEL_MALI_DRIVER}. Expected 'valhall' or 'bifrost'." >&2
            exit 1
            ;;
    esac
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
    RESOLVED_PATCH_FILE="$(resolve_kernel_patch_file)"

    if ! is_interactive_enabled; then
        return
    fi

    echo
    echo "Interactive mode enabled."
    echo "This script will:"
    echo "  - clone or refresh ${KERNEL_REPO} into ${KERNEL_DIR}"
    echo "  - optionally reset and clean that managed clone"
    echo "  - target Mali driver ${KERNEL_MALI_DRIVER}"
    echo "  - apply ${RESOLVED_PATCH_FILE}"
    echo "  - seed .config from the current system or KERNEL_CONFIG_SOURCE"
    echo "  - build Debian kernel packages with bindeb-pkg"
    echo "  - optionally install the generated packages"
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

    if prompt_yes_no "Reset and clean the managed kernel clone before applying the patch?" "${RESET_TREE}"; then
        RESET_TREE="true"
    else
        RESET_TREE="false"
    fi

    if prompt_yes_no "Apply the low32 alias patch?" "${APPLY_PATCH}"; then
        APPLY_PATCH="true"
    else
        APPLY_PATCH="false"
    fi

    if prompt_yes_no "Install the generated .deb packages after building?" "${INSTALL_PACKAGES}"; then
        INSTALL_PACKAGES="true"
    else
        INSTALL_PACKAGES="false"
    fi

    if is_true "${INSTALL_PACKAGES}"; then
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
    echo "  reset managed clone  : ${RESET_TREE}"
    echo "  apply patch          : ${APPLY_PATCH}"
    echo "  install packages     : ${INSTALL_PACKAGES}"
    echo "  reboot after install : ${REBOOT_AFTER_INSTALL}"
    echo

    if ! prompt_yes_no "Proceed? WARNING: ${KERNEL_DIR} may be reset and cleaned." "true"; then
        echo "Aborted by user."
        exit 0
    fi
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
        bc
        bison
        build-essential
        cpio
        debhelper
        dpkg-dev
        dwarves
        fakeroot
        flex
        kmod
        libelf-dev
        libssl-dev
        python3
        rsync
    )

    echo "Installing kernel build dependencies via apt-get"
    run_as_root apt-get update
    run_as_root apt-get install -y "${pkgs[@]}"
}

clone_or_refresh_kernel_tree() {
    mkdir -p "$(dirname "${KERNEL_DIR}")"

    if [[ ! -d "${KERNEL_DIR}/.git" ]]; then
        echo "Cloning ${KERNEL_REPO} (${KERNEL_BRANCH}) into ${KERNEL_DIR}"
        git clone --branch "${KERNEL_BRANCH}" --single-branch "${KERNEL_REPO}" "${KERNEL_DIR}"
        return
    fi

    echo "Refreshing existing kernel clone in ${KERNEL_DIR}"
    git -C "${KERNEL_DIR}" remote set-url origin "${KERNEL_REPO}"
    git -C "${KERNEL_DIR}" fetch origin "${KERNEL_BRANCH}"

    if is_true "${RESET_TREE}"; then
        git -C "${KERNEL_DIR}" checkout -B "${KERNEL_BRANCH}" "origin/${KERNEL_BRANCH}"
        git -C "${KERNEL_DIR}" reset --hard "origin/${KERNEL_BRANCH}"
        git -C "${KERNEL_DIR}" clean -fdx
        return
    fi

    if git -C "${KERNEL_DIR}" show-ref --verify --quiet "refs/heads/${KERNEL_BRANCH}"; then
        git -C "${KERNEL_DIR}" checkout "${KERNEL_BRANCH}"
    else
        git -C "${KERNEL_DIR}" checkout -b "${KERNEL_BRANCH}" "origin/${KERNEL_BRANCH}"
    fi
    git -C "${KERNEL_DIR}" pull --ff-only origin "${KERNEL_BRANCH}"
}

apply_kernel_patch_if_requested() {
    if ! is_true "${APPLY_PATCH}"; then
        return
    fi

    if [[ -z "${RESOLVED_PATCH_FILE}" ]]; then
        RESOLVED_PATCH_FILE="$(resolve_kernel_patch_file)"
    fi

    if [[ ! -f "${RESOLVED_PATCH_FILE}" ]]; then
        echo "Kernel patch file not found: ${RESOLVED_PATCH_FILE}" >&2
        exit 1
    fi

    echo "Applying kernel patch ${RESOLVED_PATCH_FILE}"
    git -C "${KERNEL_DIR}" am --3way "${RESOLVED_PATCH_FILE}"
}

resolve_kernel_config_source() {
    if [[ -n "${KERNEL_CONFIG_SOURCE}" ]]; then
        if [[ ! -f "${KERNEL_CONFIG_SOURCE}" ]]; then
            echo "Configured KERNEL_CONFIG_SOURCE does not exist: ${KERNEL_CONFIG_SOURCE}" >&2
            exit 1
        fi
        printf '%s\n' "${KERNEL_CONFIG_SOURCE}"
        return
    fi

    local boot_config="/boot/config-$(uname -r)"
    if [[ -f "${boot_config}" ]]; then
        printf '%s\n' "${boot_config}"
        return
    fi

    if [[ -r "/proc/config.gz" ]]; then
        printf '%s\n' "/proc/config.gz"
        return
    fi

    echo "Unable to find a kernel config source. Set KERNEL_CONFIG_SOURCE explicitly." >&2
    exit 1
}

seed_kernel_config() {
    local config_source
    config_source="$(resolve_kernel_config_source)"

    echo "Seeding kernel config from ${config_source}"
    if [[ "${config_source}" == "/proc/config.gz" ]]; then
        gzip -dc "${config_source}" > "${KERNEL_DIR}/.config"
    else
        cp "${config_source}" "${KERNEL_DIR}/.config"
    fi

    make -C "${KERNEL_DIR}" ARCH="${KERNEL_ARCH}" olddefconfig
}

build_kernel_packages() {
    echo "Building Debian kernel packages"
    BUILD_START_STAMP="$(mktemp)"

    make -C "${KERNEL_DIR}" \
        ARCH="${KERNEL_ARCH}" \
        LOCALVERSION="${KERNEL_LOCALVERSION}" \
        KDEB_PKGVERSION="${KERNEL_PACKAGE_VERSION}" \
        -j"${KERNEL_JOBS}" \
        bindeb-pkg
}

collect_generated_packages() {
    local package_dir
    package_dir="$(dirname "${KERNEL_DIR}")"

    mapfile -t GENERATED_PACKAGES < <(
        find "${package_dir}" -maxdepth 1 -type f -name "*.deb" -newer "${BUILD_START_STAMP}" | sort
    )

    if [[ "${#GENERATED_PACKAGES[@]}" -eq 0 ]]; then
        echo "No Debian packages were produced in ${package_dir}" >&2
        exit 1
    fi

    echo "Generated packages:"
    printf '  %s\n' "${GENERATED_PACKAGES[@]}"
}

install_generated_packages_if_requested() {
    if ! is_true "${INSTALL_PACKAGES}"; then
        return
    fi

    echo "Installing generated Debian packages"
    run_as_root dpkg -i "${GENERATED_PACKAGES[@]}"
}

reboot_if_requested() {
    if ! is_true "${REBOOT_AFTER_INSTALL}"; then
        return
    fi

    echo "Rebooting in 5 seconds"
    sleep 5
    run_as_root reboot
}

main() {
    trap '[[ -n "${BUILD_START_STAMP}" && -e "${BUILD_START_STAMP}" ]] && rm -f "${BUILD_START_STAMP}"' EXIT

    normalize_bool_var INSTALL_BUILD_DEPS
    normalize_bool_var RESET_TREE
    normalize_bool_var APPLY_PATCH
    normalize_bool_var INSTALL_PACKAGES
    normalize_bool_var REBOOT_AFTER_INSTALL

    if [[ "${KERNEL_ARCH}" != "arm64" ]]; then
        echo "This helper currently expects an arm64 kernel tree." >&2
        exit 1
    fi

    configure_interactive_options_if_requested
    install_build_dependencies_if_requested
    clone_or_refresh_kernel_tree
    apply_kernel_patch_if_requested
    seed_kernel_config
    build_kernel_packages
    collect_generated_packages
    install_generated_packages_if_requested
    reboot_if_requested
}

main "$@"
