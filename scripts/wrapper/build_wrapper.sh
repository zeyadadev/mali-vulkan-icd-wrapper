#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_64BIT="${WRAPPER_BUILD_64BIT:-true}"
BUILD_32BIT="${WRAPPER_BUILD_32BIT:-false}"
INSTALL_BUILD_DEPS="${WRAPPER_INSTALL_BUILD_DEPS:-false}"
INSTALL_SYSTEM="${WRAPPER_INSTALL_SYSTEM:-true}"
REBOOT_AFTER_INSTALL="${WRAPPER_REBOOT_AFTER_INSTALL:-false}"
INTERACTIVE_MODE="${WRAPPER_INTERACTIVE:-auto}"
CLEAN_BUILD_DIRS="${WRAPPER_CLEAN:-false}"
CHECK_CONFLICTS="${WRAPPER_CHECK_CONFLICTS:-true}"
REMOVE_CONFLICTS="${WRAPPER_REMOVE_CONFLICTS:-false}"
PRUNE_UNSELECTED_ARCH="${WRAPPER_PRUNE_UNSELECTED_ARCH:-false}"
FORCE_REINSTALL="${WRAPPER_FORCE_REINSTALL:-false}"
CMAKE_BUILD_TYPE="${WRAPPER_BUILD_TYPE:-Release}"
INSTALL_PREFIX="${WRAPPER_INSTALL_PREFIX:-/usr}"
BUILD64_DIR="${WRAPPER_BUILD64_DIR:-${ROOT_DIR}/build64}"
BUILD32_DIR="${WRAPPER_BUILD32_DIR:-${ROOT_DIR}/build32}"
TOOLCHAIN_FILE="${WRAPPER_TOOLCHAIN_FILE:-${ROOT_DIR}/cmake/armhf_toolchain.cmake}"
JOBS="${WRAPPER_JOBS:-$(nproc)}"

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

    local single_arch_selected="false"

    echo
    echo "Interactive mode enabled."
    echo "This script can:"
    echo "  - build the wrapper (64-bit, 32-bit, or both)"
    echo "  - optionally install build dependencies (apt)"
    echo "  - optionally install to ${INSTALL_PREFIX}"
    echo "  - optionally reboot after install"
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

    if prompt_yes_no "Build 64-bit wrapper?" "${BUILD_64BIT}"; then
        BUILD_64BIT="true"
    else
        BUILD_64BIT="false"
    fi

    if prompt_yes_no "Build 32-bit wrapper?" "${BUILD_32BIT}"; then
        BUILD_32BIT="true"
    else
        BUILD_32BIT="false"
    fi

    if [[ "${BUILD_64BIT}" == "false" && "${BUILD_32BIT}" == "false" ]]; then
        echo "At least one architecture must be selected." >&2
        exit 1
    fi

    if { is_true "${BUILD_64BIT}" && ! is_true "${BUILD_32BIT}"; } || \
       { is_true "${BUILD_32BIT}" && ! is_true "${BUILD_64BIT}"; }; then
        single_arch_selected="true"
    fi

    if prompt_yes_no "Clean build directories before configuring?" "${CLEAN_BUILD_DIRS}"; then
        CLEAN_BUILD_DIRS="true"
    else
        CLEAN_BUILD_DIRS="false"
    fi

    if prompt_yes_no "Install built wrapper(s) to ${INSTALL_PREFIX}?" "${INSTALL_SYSTEM}"; then
        INSTALL_SYSTEM="true"
    else
        INSTALL_SYSTEM="false"
    fi

    if is_true "${INSTALL_SYSTEM}"; then
        # Install mode is opinionated: always clean conflicting system state first.
        CHECK_CONFLICTS="true"
        REMOVE_CONFLICTS="true"
        FORCE_REINSTALL="true"

        if is_true "${single_arch_selected}"; then
            if prompt_yes_no "Remove installed wrapper artifacts for unselected architectures?" "${PRUNE_UNSELECTED_ARCH}"; then
                PRUNE_UNSELECTED_ARCH="true"
            else
                PRUNE_UNSELECTED_ARCH="false"
            fi
        else
            PRUNE_UNSELECTED_ARCH="false"
        fi

        if prompt_yes_no "Reboot automatically after successful install?" "${REBOOT_AFTER_INSTALL}"; then
            REBOOT_AFTER_INSTALL="true"
        else
            REBOOT_AFTER_INSTALL="false"
        fi
    else
        CHECK_CONFLICTS="false"
        REMOVE_CONFLICTS="false"
        PRUNE_UNSELECTED_ARCH="false"
        FORCE_REINSTALL="false"
        REBOOT_AFTER_INSTALL="false"
    fi

    echo
    echo "Selected options:"
    echo "  install dependencies : ${INSTALL_BUILD_DEPS}"
    echo "  build 64-bit         : ${BUILD_64BIT}"
    echo "  build 32-bit         : ${BUILD_32BIT}"
    echo "  clean build dirs     : ${CLEAN_BUILD_DIRS}"
    echo "  check conflicts      : ${CHECK_CONFLICTS} (auto when install=true)"
    echo "  remove conflicts     : ${REMOVE_CONFLICTS} (auto when install=true)"
    echo "  prune unselected     : ${PRUNE_UNSELECTED_ARCH}"
    echo "  force reinstall      : ${FORCE_REINSTALL} (auto when install=true)"
    echo "  install wrappers     : ${INSTALL_SYSTEM}"
    echo "  reboot after install : ${REBOOT_AFTER_INSTALL}"
    echo "  install prefix       : ${INSTALL_PREFIX}"
    echo "  build type           : ${CMAKE_BUILD_TYPE}"
    echo

    if ! prompt_yes_no "Proceed?" "true"; then
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

    local base_pkgs=(
        build-essential
        cmake
        pkg-config
        libvulkan-dev
        libwayland-dev
        libx11-dev
        libx11-xcb-dev
        libdrm-dev
        libxcb-shm0-dev
        libxcb-present-dev
        libxcb-sync-dev
        libxrandr-dev
        wayland-protocols
    )

    local cross_pkgs=(
        gcc-arm-linux-gnueabihf
        g++-arm-linux-gnueabihf
        libdrm-dev:armhf
        libwayland-dev:armhf
        libx11-dev:armhf
        libx11-xcb-dev:armhf
        libxcb-shm0-dev:armhf
        libxcb-present-dev:armhf
        libxcb-sync-dev:armhf
        libxrandr-dev:armhf
    )

    echo "Installing wrapper build dependencies via apt-get"
    if is_true "${BUILD_32BIT}"; then
        if ! dpkg --print-foreign-architectures | grep -Fxq "armhf"; then
            echo "Adding armhf architecture for 32-bit dependencies"
            run_as_root dpkg --add-architecture armhf
        fi
    fi

    run_as_root apt-get update
    run_as_root apt-get install -y "${base_pkgs[@]}"

    if is_true "${BUILD_32BIT}"; then
        run_as_root apt-get install -y "${cross_pkgs[@]}"
    fi
}

warn_if_prefix_manifest_mismatch() {
    if is_true "${INSTALL_SYSTEM}" && [[ "${INSTALL_PREFIX}" != "/usr" ]]; then
        echo "WARN: INSTALL_PREFIX is '${INSTALL_PREFIX}', but ICD manifests are hardcoded to /usr paths."
        echo "      Installed manifests may point to /usr/lib/*/libmali_wrapper.so."
    fi
}

warn_if_vulkan_env_overrides_present() {
    if [[ -n "${VK_ICD_FILENAMES:-}" ]]; then
        echo "WARN: VK_ICD_FILENAMES is set and may override installed ICD manifests."
    fi
    if [[ -n "${VK_LAYER_PATH:-}" ]]; then
        echo "WARN: VK_LAYER_PATH is set and may change Vulkan layer behavior."
    fi
    if [[ -n "${VK_INSTANCE_LAYERS:-}" ]]; then
        echo "WARN: VK_INSTANCE_LAYERS is set and may force extra layers."
    fi
}

remove_files_as_root() {
    local files=("$@")
    if [[ "${#files[@]}" -eq 0 ]]; then
        return
    fi
    echo "  removing:"
    printf '    - %s\n' "${files[@]}"
    run_as_root rm -f -- "${files[@]}"
}

check_for_old_or_conflicting_installations() {
    if ! is_true "${CHECK_CONFLICTS}"; then
        return
    fi

    local known_conflicts=()
    local potential_conflicts=()
    local f=""
    local base=""
    local should_remove="false"

    local explicit_conflict_paths=(
        /usr/share/vulkan/icd.d/mali.json
        /etc/vulkan/icd.d/mali.json
        /usr/share/vulkan/icd.d/mali_icd.json
        /etc/vulkan/icd.d/mali_icd.json
        /usr/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
        /etc/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
    )

    for f in "${explicit_conflict_paths[@]}"; do
        if [[ -f "${f}" ]]; then
            known_conflicts+=("${f}")
        fi
    done

    shopt -s nullglob
    for f in /usr/share/vulkan/icd.d/*.json /etc/vulkan/icd.d/*.json; do
        [[ -f "${f}" ]] || continue
        base="$(basename "${f}")"
        case "${base}" in
            mali_icd.aarch64.json|mali_icd.armhf.json)
                continue
                ;;
        esac

        if [[ "${base}" == *mali* ]] || grep -Eqi '"library_path"[[:space:]]*:[[:space:]]*".*libmali' "${f}"; then
            potential_conflicts+=("${f}")
        fi
    done
    shopt -u nullglob

    if [[ "${#known_conflicts[@]}" -eq 0 && "${#potential_conflicts[@]}" -eq 0 ]]; then
        echo "Conflict check: no obvious legacy/conflicting Vulkan files found."
        return
    fi

    echo
    echo "Potential Vulkan install conflicts detected:"
    if [[ "${#known_conflicts[@]}" -gt 0 ]]; then
        echo "  Known conflicting files:"
        printf '    - %s\n' "${known_conflicts[@]}"
    fi
    if [[ "${#potential_conflicts[@]}" -gt 0 ]]; then
        echo "  Additional Mali-related manifests to review:"
        printf '    - %s\n' "${potential_conflicts[@]}"
    fi

    if [[ "${#known_conflicts[@]}" -gt 0 ]]; then
        if is_true "${REMOVE_CONFLICTS}"; then
            should_remove="true"
        elif is_interactive_enabled; then
            if prompt_yes_no "Remove known conflicting files now?" "true"; then
                should_remove="true"
            fi
        fi

        if is_true "${should_remove}"; then
            echo "Removing known conflicting files"
            remove_files_as_root "${known_conflicts[@]}"
        else
            echo "Known conflicting files were not removed."
        fi
    fi
}

prune_unselected_arch_installations_if_requested() {
    if ! is_true "${PRUNE_UNSELECTED_ARCH}"; then
        return
    fi
    if ! is_true "${INSTALL_SYSTEM}"; then
        echo "Skipping prune-unselected step because install is disabled."
        return
    fi

    local to_remove=()

    if is_true "${BUILD_64BIT}" && ! is_true "${BUILD_32BIT}"; then
        to_remove+=(
            "${INSTALL_PREFIX}/lib/arm-linux-gnueabihf/libmali_wrapper.so.1.0.0"
            "${INSTALL_PREFIX}/lib/arm-linux-gnueabihf/libmali_wrapper.so.1"
            "${INSTALL_PREFIX}/lib/arm-linux-gnueabihf/libmali_wrapper.so"
            "${INSTALL_PREFIX}/share/vulkan/icd.d/mali_icd.armhf.json"
        )
    elif is_true "${BUILD_32BIT}" && ! is_true "${BUILD_64BIT}"; then
        to_remove+=(
            "${INSTALL_PREFIX}/lib/aarch64-linux-gnu/libmali_wrapper.so.1.0.0"
            "${INSTALL_PREFIX}/lib/aarch64-linux-gnu/libmali_wrapper.so.1"
            "${INSTALL_PREFIX}/lib/aarch64-linux-gnu/libmali_wrapper.so"
            "${INSTALL_PREFIX}/share/vulkan/icd.d/mali_icd.aarch64.json"
        )
    fi

    if [[ "${#to_remove[@]}" -eq 0 ]]; then
        return
    fi

    echo "Pruning wrapper artifacts for unselected architecture"
    remove_files_as_root "${to_remove[@]}"
}

force_reinstall_selected_arch_if_requested() {
    if ! is_true "${FORCE_REINSTALL}"; then
        return
    fi
    if ! is_true "${INSTALL_SYSTEM}"; then
        echo "Skipping force-reinstall cleanup because install is disabled."
        return
    fi

    local to_remove=()
    if is_true "${BUILD_64BIT}"; then
        to_remove+=(
            "${INSTALL_PREFIX}/lib/aarch64-linux-gnu/libmali_wrapper.so.1.0.0"
            "${INSTALL_PREFIX}/lib/aarch64-linux-gnu/libmali_wrapper.so.1"
            "${INSTALL_PREFIX}/lib/aarch64-linux-gnu/libmali_wrapper.so"
            "${INSTALL_PREFIX}/share/vulkan/icd.d/mali_icd.aarch64.json"
        )
    fi
    if is_true "${BUILD_32BIT}"; then
        to_remove+=(
            "${INSTALL_PREFIX}/lib/arm-linux-gnueabihf/libmali_wrapper.so.1.0.0"
            "${INSTALL_PREFIX}/lib/arm-linux-gnueabihf/libmali_wrapper.so.1"
            "${INSTALL_PREFIX}/lib/arm-linux-gnueabihf/libmali_wrapper.so"
            "${INSTALL_PREFIX}/share/vulkan/icd.d/mali_icd.armhf.json"
        )
    fi

    if [[ "${#to_remove[@]}" -gt 0 ]]; then
        echo "Force reinstall: clearing selected architecture install targets"
        remove_files_as_root "${to_remove[@]}"
    fi
}

reconfigure_dir_if_requested() {
    local dir="$1"
    if is_true "${CLEAN_BUILD_DIRS}" && [[ -d "${dir}" ]]; then
        echo "Cleaning ${dir}"
        rm -rf "${dir}"
    fi
}

configure_and_build_64() {
    reconfigure_dir_if_requested "${BUILD64_DIR}"

    local extra_args=()
    if [[ -n "${WRAPPER_CMAKE_ARGS_64:-}" ]]; then
        # shellcheck disable=SC2206
        extra_args=(${WRAPPER_CMAKE_ARGS_64})
    fi

    echo "Configuring 64-bit wrapper in ${BUILD64_DIR}"
    cmake -S "${ROOT_DIR}" -B "${BUILD64_DIR}" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DBUILD_64BIT=ON \
        -DBUILD_32BIT=OFF \
        "${extra_args[@]}"

    echo "Building 64-bit wrapper"
    cmake --build "${BUILD64_DIR}" -j "${JOBS}"

    if is_true "${INSTALL_SYSTEM}"; then
        echo "Installing 64-bit wrapper"
        run_as_root cmake --install "${BUILD64_DIR}"
    fi
}

configure_and_build_32() {
    if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
        echo "Toolchain file not found: ${TOOLCHAIN_FILE}" >&2
        exit 1
    fi

    reconfigure_dir_if_requested "${BUILD32_DIR}"

    local extra_args=()
    if [[ -n "${WRAPPER_CMAKE_ARGS_32:-}" ]]; then
        # shellcheck disable=SC2206
        extra_args=(${WRAPPER_CMAKE_ARGS_32})
    fi

    echo "Configuring 32-bit wrapper in ${BUILD32_DIR}"
    cmake -S "${ROOT_DIR}" -B "${BUILD32_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DBUILD_64BIT=OFF \
        -DBUILD_32BIT=ON \
        "${extra_args[@]}"

    echo "Building 32-bit wrapper"
    cmake --build "${BUILD32_DIR}" -j "${JOBS}"

    if is_true "${INSTALL_SYSTEM}"; then
        echo "Installing 32-bit wrapper"
        run_as_root cmake --install "${BUILD32_DIR}"
    fi
}

verify_one_arch_install() {
    local arch="$1"
    local build_dir="$2"
    local installed_link="$3"
    local manifest_path="$4"
    local built_lib="${build_dir}/libmali_wrapper.so"
    local installed_real=""

    echo "  ${arch}:"
    if [[ -f "${manifest_path}" ]]; then
        echo "    manifest: ${manifest_path}"
    else
        echo "    WARN: manifest not found at ${manifest_path}"
    fi

    if [[ -L "${installed_link}" || -f "${installed_link}" ]]; then
        installed_real="$(readlink -f "${installed_link}" 2>/dev/null || true)"
        if [[ -n "${installed_real}" ]]; then
            echo "    installed: ${installed_link} -> ${installed_real}"
        else
            echo "    installed: ${installed_link}"
        fi
    else
        echo "    WARN: installed library link missing at ${installed_link}"
        return
    fi

    if [[ -f "${built_lib}" && -n "${installed_real}" && -f "${installed_real}" ]]; then
        if cmp -s "${built_lib}" "${installed_real}"; then
            echo "    verify: installed binary matches current build output"
        else
            echo "    WARN: installed binary differs from current build output"
        fi
    fi
}

post_install_verification() {
    if ! is_true "${INSTALL_SYSTEM}"; then
        return
    fi

    local manifest64="${INSTALL_PREFIX}/share/vulkan/icd.d/mali_icd.aarch64.json"
    local manifest32="${INSTALL_PREFIX}/share/vulkan/icd.d/mali_icd.armhf.json"
    local link64="${INSTALL_PREFIX}/lib/aarch64-linux-gnu/libmali_wrapper.so"
    local link32="${INSTALL_PREFIX}/lib/arm-linux-gnueabihf/libmali_wrapper.so"

    echo
    echo "Post-install verification:"
    if is_true "${BUILD_64BIT}"; then
        verify_one_arch_install "aarch64" "${BUILD64_DIR}" "${link64}" "${manifest64}"
    fi
    if is_true "${BUILD_32BIT}"; then
        verify_one_arch_install "armhf" "${BUILD32_DIR}" "${link32}" "${manifest32}"
    fi
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

    echo "Reboot requested by WRAPPER_REBOOT_AFTER_INSTALL=1"
    run_as_root systemctl reboot
}

normalize_bool_var "BUILD_64BIT"
normalize_bool_var "BUILD_32BIT"
normalize_bool_var "INSTALL_BUILD_DEPS"
normalize_bool_var "INSTALL_SYSTEM"
normalize_bool_var "REBOOT_AFTER_INSTALL"
normalize_bool_var "CLEAN_BUILD_DIRS"
normalize_bool_var "CHECK_CONFLICTS"
normalize_bool_var "REMOVE_CONFLICTS"
normalize_bool_var "PRUNE_UNSELECTED_ARCH"
normalize_bool_var "FORCE_REINSTALL"
configure_interactive_options_if_requested

if [[ "${BUILD_64BIT}" == "false" && "${BUILD_32BIT}" == "false" ]]; then
    echo "Nothing to do: both WRAPPER_BUILD_64BIT and WRAPPER_BUILD_32BIT are disabled." >&2
    exit 1
fi

if is_true "${INSTALL_SYSTEM}"; then
    CHECK_CONFLICTS="true"
    REMOVE_CONFLICTS="true"
    FORCE_REINSTALL="true"
else
    CHECK_CONFLICTS="false"
    REMOVE_CONFLICTS="false"
    PRUNE_UNSELECTED_ARCH="false"
    FORCE_REINSTALL="false"
    REBOOT_AFTER_INSTALL="false"
fi

if is_true "${BUILD_64BIT}" && is_true "${BUILD_32BIT}"; then
    PRUNE_UNSELECTED_ARCH="false"
fi

if ! [[ "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid WRAPPER_JOBS value: ${JOBS}" >&2
    exit 1
fi

for cmd in cmake; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "Required tool missing: ${cmd}" >&2
        exit 1
    fi
done

install_build_dependencies_if_requested
warn_if_prefix_manifest_mismatch
warn_if_vulkan_env_overrides_present
check_for_old_or_conflicting_installations
prune_unselected_arch_installations_if_requested
force_reinstall_selected_arch_if_requested

if is_true "${BUILD_64BIT}"; then
    configure_and_build_64
fi

if is_true "${BUILD_32BIT}"; then
    configure_and_build_32
fi

post_install_verification

echo
echo "Done."
if is_true "${BUILD_64BIT}"; then
    echo "  64-bit build dir: ${BUILD64_DIR}"
fi
if is_true "${BUILD_32BIT}"; then
    echo "  32-bit build dir: ${BUILD32_DIR}"
fi
if is_true "${INSTALL_SYSTEM}"; then
    echo "Installed to prefix: ${INSTALL_PREFIX}"
else
    echo "Install skipped (WRAPPER_INSTALL_SYSTEM=false)"
fi

reboot_if_requested
