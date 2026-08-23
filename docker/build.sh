#!/usr/bin/env bash
# Container entrypoint: builds every target in build.yaml and copies the
# resulting .uf2 files to /output. Mirrors the GitHub Actions workflow
# (build-user-config.yml) exactly:
#
#   - the repo is a ZMK module (zephyr/module.yml), so the config dir is
#     isolated and the repo root is passed as -DZMK_EXTRA_MODULES
#   - west init/update/zephyr-export run once into the /build-env volume
set -euo pipefail

REPO="${REPO:-/repo}"
OUT="${OUT:-/output}"
ENV_DIR="/build-env"

if [ ! -f "${REPO}/config/west.yml" ]; then
    echo "ERROR: ${REPO}/config/west.yml not found. Mount the mi_corne repo at /repo." >&2
    exit 1
fi

# Refresh the isolated config copy from the mounted repo (picks up keymap and
# .conf changes). west.yml lives here too, so we can detect manifest changes.
mkdir -p "${ENV_DIR}/config"
cp -R "${REPO}/config/." "${ENV_DIR}/config/"

MANIFEST_MARKER="${ENV_DIR}/.west.yml.sha256"
new_hash="$(sha256sum "${REPO}/config/west.yml" | cut -d' ' -f1)"

if [ ! -f "${ENV_DIR}/.west/config" ]; then
    echo "== Initializing west workspace (first run) =="
    ( cd "${ENV_DIR}" && west init -l "${ENV_DIR}/config" )
    ( cd "${ENV_DIR}" && west update --fetch-opt=--filter=tree:0 )
    echo "${new_hash}" > "${MANIFEST_MARKER}"
elif [ -f "${MANIFEST_MARKER}" ] && [ "$(cat "${MANIFEST_MARKER}")" != "${new_hash}" ]; then
    echo "== config/west.yml changed, updating west workspace =="
    ( cd "${ENV_DIR}" && west update --fetch-opt=--filter=tree:0 )
    echo "${new_hash}" > "${MANIFEST_MARKER}"
fi

# The exported Zephyr CMake package lives in $HOME of the container, which is
# ephemeral; re-export on every run so fresh build directories can configure.
( cd "${ENV_DIR}" && west zephyr-export )

build_one() {
    local artifact="$1" board="$2" shield="$3" snippet="$4" cmake_args="$5"
    local build_dir="${ENV_DIR}/build-${artifact}"

    local west_args=()
    local cmake=(-DZMK_CONFIG="${ENV_DIR}/config" -DZMK_EXTRA_MODULES="${REPO}")
    if [ -n "${shield}" ]; then
        cmake+=(-DSHIELD="${shield}")
    fi
    if [ -n "${snippet}" ]; then
        west_args+=(-S "${snippet}")
    fi
    if [ -n "${cmake_args}" ]; then
        # shellcheck disable=SC2206
        cmake+=(${cmake_args})
    fi

    echo ""
    echo "===== Building ${artifact} (board=${board}, shield=${shield:-none}) ====="
    ( cd "${ENV_DIR}" && west build -s zmk/app -d "${build_dir}" -b "${board}" "${west_args[@]}" -- "${cmake[@]}" )

    mkdir -p "${OUT}"
    if [ -f "${build_dir}/zephyr/zmk.uf2" ]; then
        cp "${build_dir}/zephyr/zmk.uf2" "${OUT}/${artifact}.uf2"
        echo "OK -> ${OUT}/${artifact}.uf2"
    elif [ -f "${build_dir}/zephyr/zmk.bin" ]; then
        cp "${build_dir}/zephyr/zmk.bin" "${OUT}/${artifact}.bin"
        echo "OK (no uf2) -> ${OUT}/${artifact}.bin"
    else
        echo "ERROR: no zmk.uf2/zmk.bin produced for ${artifact}" >&2
        return 1
    fi
}

# Matrix mirrored from build.yaml
build_one "eyelash_corne_left" "eyelash_corne_left" "nice_view_custom" "" \
    "-DCONFIG_ZMK_SPLIT=y -DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n"
build_one "eyelash_corne_right" "eyelash_corne_right" "nice_view" "" \
    "-DCONFIG_ZMK_SPLIT=y -DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n"
build_one "eyelash_corne_dongle" "seeeduino_xiao_ble" "eyelash_corne_dongle" "studio-rpc-usb-uart" \
    "-DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_STUDIO_LOCKING=n -DCONFIG_BT_MAX_CONN=7 -DCONFIG_BT_MAX_PAIRED=7 -DCONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=2 -DCONFIG_ZMK_BACKLIGHT=n"
build_one "settings_reset_xiao_ble" "seeeduino_xiao_ble" "settings_reset" "" ""
build_one "settings_reset_eyelash_corne" "eyelash_corne_left" "settings_reset" "" ""

echo ""
echo "===== Build complete. Artifacts in ${OUT} ====="
ls -lh "${OUT}"
