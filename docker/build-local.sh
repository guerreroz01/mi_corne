#!/usr/bin/env bash
# Host-side runner for the local ZMK Docker build.
#
# Usage:
#   ./docker/build-local.sh
#
# Outputs the .uf2 files to <repo>/build/ (already gitignored).
# The west workspace is kept in a persistent Docker volume, so the first run
# downloads the ZMK/Zephyr modules and later runs only recompile.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="mi-corne-zmk:latest"
VOLUME="zmk-workspace-mi-corne"

docker build -t "${IMAGE}" "${REPO_ROOT}/docker"
docker volume create "${VOLUME}" >/dev/null
mkdir -p "${REPO_ROOT}/build"

exec docker run --rm \
    --mount type=volume,src="${VOLUME}",dst=/build-env \
    --mount type=bind,src="${REPO_ROOT}",dst=/repo,readonly \
    --mount type=bind,src="${REPO_ROOT}/build",dst=/output \
    "${IMAGE}"
