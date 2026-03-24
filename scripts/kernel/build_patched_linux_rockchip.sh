#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PATCH_DIR="${ROOT_DIR}/patches/kernel"
MANAGED_KERNEL_DIR="${ROOT_DIR}/third_party/linux-rockchip"
PATCH_FILE="${KERNEL_PATCH_FILE:-}"
KERNEL_REPO="${KERNEL_REPO:-https://github.com/zeyadadev/linux-rockchip.git}"
KERNEL_BRANCH="${KERNEL_BRANCH:-rk-6.1-rkr6.1}"
KERNEL_DIR="${KERNEL_DIR:-${MANAGED_KERNEL_DIR}}"
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
RESET_TREE_EXPLICIT="false"

if [[ -n "${KERNEL_RESET_TREE+x}" ]]; then
    RESET_TREE_EXPLICIT="true"
fi

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

canonicalize_path() {
    if command -v realpath >/dev/null 2>&1; then
        realpath -m "$1"
        return
    fi

    printf '%s\n' "$1"
}

using_managed_kernel_dir() {
    [[ "$(canonicalize_path "${KERNEL_DIR}")" == "$(canonicalize_path "${MANAGED_KERNEL_DIR}")" ]]
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

    if [[ -d "${KERNEL_DIR}/.git" ]] &&
       ! using_managed_kernel_dir &&
       [[ "${RESET_TREE_EXPLICIT}" != "true" ]]; then
        RESET_TREE="false"
    fi

    if ! is_interactive_enabled; then
        return
    fi

    echo
    echo "Interactive mode enabled."
    echo "This script will:"
    echo "  - clone or refresh ${KERNEL_REPO} into ${KERNEL_DIR}"
    echo "  - reuse an existing clone when ${KERNEL_DIR} already exists"
    echo "  - optionally reset and clean the selected kernel tree"
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

    if prompt_yes_no "Reset and clean the selected kernel tree before applying the patch?" "${RESET_TREE}"; then
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

    echo "Detected existing kernel clone in ${KERNEL_DIR}"

    if ! using_managed_kernel_dir && is_true "${RESET_TREE}" && [[ "${RESET_TREE_EXPLICIT}" != "true" ]]; then
        echo "Using an existing external kernel tree; defaulting KERNEL_RESET_TREE=false to avoid destructive reset."
        echo "Set KERNEL_RESET_TREE=1 explicitly if you want the script to reset and clean this tree."
        RESET_TREE="false"
    fi

    if using_managed_kernel_dir; then
        echo "Refreshing managed kernel clone in ${KERNEL_DIR}"
        git -C "${KERNEL_DIR}" remote set-url origin "${KERNEL_REPO}"
        git -C "${KERNEL_DIR}" fetch origin "${KERNEL_BRANCH}"
    else
        echo "Using existing external kernel tree in ${KERNEL_DIR}; leaving remotes unchanged"
        if git -C "${KERNEL_DIR}" remote get-url origin >/dev/null 2>&1; then
            git -C "${KERNEL_DIR}" fetch origin "${KERNEL_BRANCH}"
        else
            echo "No origin remote configured for ${KERNEL_DIR}; continuing without fetch"
        fi
    fi

    if is_true "${RESET_TREE}"; then
        if ! git -C "${KERNEL_DIR}" rev-parse --verify --quiet "origin/${KERNEL_BRANCH}" >/dev/null; then
            echo "Cannot reset ${KERNEL_DIR}: missing origin/${KERNEL_BRANCH}" >&2
            exit 1
        fi
        git -C "${KERNEL_DIR}" checkout -B "${KERNEL_BRANCH}" "origin/${KERNEL_BRANCH}"
        git -C "${KERNEL_DIR}" reset --hard "origin/${KERNEL_BRANCH}"
        git -C "${KERNEL_DIR}" clean -fdx
        return
    fi

    if git -C "${KERNEL_DIR}" show-ref --verify --quiet "refs/heads/${KERNEL_BRANCH}"; then
        git -C "${KERNEL_DIR}" checkout "${KERNEL_BRANCH}"
    elif git -C "${KERNEL_DIR}" rev-parse --verify --quiet "origin/${KERNEL_BRANCH}" >/dev/null; then
        git -C "${KERNEL_DIR}" checkout -b "${KERNEL_BRANCH}" "origin/${KERNEL_BRANCH}"
    fi

    if git -C "${KERNEL_DIR}" remote get-url origin >/dev/null 2>&1 &&
       git -C "${KERNEL_DIR}" rev-parse --verify --quiet "origin/${KERNEL_BRANCH}" >/dev/null &&
       [[ "$(git -C "${KERNEL_DIR}" rev-parse --abbrev-ref HEAD)" == "${KERNEL_BRANCH}" ]]; then
        git -C "${KERNEL_DIR}" pull --ff-only origin "${KERNEL_BRANCH}"
    fi
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

    if git -C "${KERNEL_DIR}" apply --reverse --check "${RESOLVED_PATCH_FILE}" >/dev/null 2>&1; then
        echo "Kernel patch already present in ${KERNEL_DIR}; skipping git am"
        return
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

    configure_mali_driver
    make -C "${KERNEL_DIR}" ARCH="${KERNEL_ARCH}" olddefconfig
}

configure_mali_driver() {
    local cfg="${KERNEL_DIR}/scripts/config"
    local dot_config="${KERNEL_DIR}/.config"

    case "${KERNEL_MALI_DRIVER,,}" in
        valhall)
            echo "Configuring kernel for Mali Valhall driver (DDK g29p1)"

            # Disable Bifrost driver and all its sub-options
            "${cfg}" --file "${dot_config}" \
                --disable MALI_BIFROST \
                --disable MALI_CSF_SUPPORT \
                --disable MALI_BIFROST_DEVFREQ \
                --disable MALI_BIFROST_GATOR_SUPPORT \
                --disable MALI_BIFROST_ENABLE_TRACE \
                --disable MALI_BIFROST_EXPERT \
                --disable MALI_BIFROST_DEBUG \
                --disable MALI_BIFROST_FENCE_DEBUG \
                --disable MALI_BIFROST_SYSTEM_TRACE \
                --disable MALI_CSF_INCLUDE_FW \
                --disable MALI_TRACE_POWER_GPU_WORK_PERIOD

            # Enable Valhall driver with RK3588 platform support
            "${cfg}" --file "${dot_config}" \
                --enable  MALI_VALHALL \
                --set-str MALI_VALHALL_PLATFORM_NAME "rk" \
                --enable  MALI_VALHALL_REAL_HW \
                --enable  MALI_VALHALL_CSF_SUPPORT \
                --enable  MALI_VALHALL_DEVFREQ \
                --enable  MALI_VALHALL_GATOR_SUPPORT \
                --enable  MALI_VALHALL_ENABLE_TRACE \
                --enable  MALI_VALHALL_TRACE_POWER_GPU_WORK_PERIOD \
                --enable  MALI_CSF_INCLUDE_FW
            ;;
        bifrost)
            echo "Keeping Mali Bifrost driver configuration"
            # Bifrost is the stock Rockchip default; nothing to change
            ;;
        *)
            echo "Unsupported KERNEL_MALI_DRIVER: ${KERNEL_MALI_DRIVER}" >&2
            exit 1
            ;;
    esac
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

detect_installed_kernel_version() {
    local kernel_version=""
    local pkg
    for pkg in "${GENERATED_PACKAGES[@]}"; do
        local base
        base="$(basename "${pkg}")"
        if [[ "${base}" == linux-image-[0-9]* ]]; then
            kernel_version="${base#linux-image-}"
            kernel_version="${kernel_version%%_*}"
            break
        fi
    done

    if [[ -z "${kernel_version}" ]]; then
        local vmlinuz
        vmlinuz="$(ls -t /boot/vmlinuz-*"${KERNEL_LOCALVERSION}" 2>/dev/null | head -1)"
        if [[ -n "${vmlinuz}" ]]; then
            kernel_version="$(basename "${vmlinuz}" | sed 's/^vmlinuz-//')"
        fi
    fi

    printf '%s\n' "${kernel_version}"
}

configure_armbian_boot_if_requested() {
    if ! is_true "${INSTALL_PACKAGES}"; then
        return
    fi

    # Only run on Armbian-style systems that boot via /boot/Image symlink
    if [[ ! -L /boot/Image ]]; then
        return
    fi

    local kernel_version
    kernel_version="$(detect_installed_kernel_version)"
    if [[ -z "${kernel_version}" ]]; then
        echo "Warning: could not detect installed kernel version; skipping boot configuration." >&2
        return
    fi

    local vmlinuz="/boot/vmlinuz-${kernel_version}"
    local initrd="/boot/initrd.img-${kernel_version}"
    local dtb_src="/usr/lib/linux-image-${kernel_version}"
    local dtb_dst="/boot/dtb-${kernel_version}"

    if [[ ! -f "${vmlinuz}" ]]; then
        echo "Warning: ${vmlinuz} not found; skipping boot configuration." >&2
        return
    fi

    # Armbian U-Boot expects a raw ARM64 Image, but bindeb-pkg produces a
    # gzip-compressed vmlinuz.  Decompress in-place when necessary.
    if file "${vmlinuz}" | grep -q "gzip compressed"; then
        echo "Decompressing ${vmlinuz} (Armbian requires uncompressed ARM64 Image)"
        run_as_root mv "${vmlinuz}" "${vmlinuz}.gz"
        run_as_root gunzip "${vmlinuz}.gz"
    fi

    # Copy device-tree blobs into /boot if the deb placed them under /usr/lib
    if [[ -d "${dtb_src}" && ! -d "${dtb_dst}" ]]; then
        echo "Copying device-tree blobs to ${dtb_dst}"
        run_as_root cp -a "${dtb_src}" "${dtb_dst}"
    fi

    echo "Updating Armbian boot symlinks for ${kernel_version}"
    run_as_root ln -sf "vmlinuz-${kernel_version}" /boot/Image

    if [[ -f "${initrd}" ]]; then
        run_as_root ln -sf "initrd.img-${kernel_version}" /boot/initrd.img
    fi

    if [[ -d "${dtb_dst}" ]]; then
        run_as_root ln -sf "dtb-${kernel_version}" /boot/dtb
    fi

    echo "Boot configuration updated:"
    ls -l /boot/Image /boot/initrd.img /boot/dtb 2>/dev/null | sed 's/^/  /'
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
    configure_armbian_boot_if_requested
    reboot_if_requested
}

main "$@"
