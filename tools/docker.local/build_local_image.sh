#!/usr/bin/env bash
set -euo pipefail

# Build Linux artifacts inside a local Ubuntu tool image via bind mount,
# then package a local runtime image with a minimal temporary build context.
#
# Usage:
#   ./tools/docker.local/build_local_image.sh [image_tag] [platform]
#
# Example:
#   ./tools/docker.local/build_local_image.sh zlmk-local-verify linux/arm64
#
# Optional env vars (shared host/container paths):
#   USE_DATA_LAYOUT  (default: 0; set to 1 to use legacy /data defaults)
#   MOUNT_AUTH_DIR    (default: /opt/media/auth)
#   MOUNT_LOG_DIR     (default: /opt/media/bin/log)
#   MOUNT_RECORD_DIR  (default: /opt/media/bin/www/record)
#
# Legacy /data layout examples (use these only when Docker Desktop has shared /data):
#   MOUNT_AUTH_DIR=/data/agi7/tinynvr/auth
#   MOUNT_LOG_DIR=/data/agi7/zlmediakit/log
#   MOUNT_RECORD_DIR=/data/agi7/zlmediakit/record

IMAGE_TAG="${1:-zlmmediakit}"
PLATFORM="${2:-linux/arm64}"
MODEL="Release"
DOCKER_BIN="${DOCKER_BIN:-docker}"
UBUNTU_IMAGE="ubuntu:24.04"
BUILD_IMAGE="zlmmediakit-buildtools:ubuntu24.04"
USE_DATA_LAYOUT="${USE_DATA_LAYOUT:-0}"

LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${LOCAL_DIR}/../.." && pwd)"
LINUX_BUILD_DIR_REL="tools/docker.local/build.linux.${MODEL}"
LOCAL_RELEASE_DIR_REL="tools/docker.local/release/linux/${MODEL}"
TMP_CONTEXT="${LOCAL_DIR}/.docker-local-context-${MODEL}"

if [[ "${USE_DATA_LAYOUT}" == "1" ]]; then
  DEFAULT_MOUNT_AUTH_DIR="/data/agi7/tinynvr/auth"
  DEFAULT_MOUNT_LOG_DIR="/data/agi7/zlmediakit/log"
  DEFAULT_MOUNT_RECORD_DIR="/data/agi7/zlmediakit/record"
else
  DEFAULT_MOUNT_AUTH_DIR="/opt/media/auth"
  DEFAULT_MOUNT_LOG_DIR="/opt/media/bin/log"
  DEFAULT_MOUNT_RECORD_DIR="/opt/media/bin/www/record"
fi

MOUNT_AUTH_DIR="${MOUNT_AUTH_DIR:-${DEFAULT_MOUNT_AUTH_DIR}}"
MOUNT_LOG_DIR="${MOUNT_LOG_DIR:-${DEFAULT_MOUNT_LOG_DIR}}"
MOUNT_RECORD_DIR="${MOUNT_RECORD_DIR:-${DEFAULT_MOUNT_RECORD_DIR}}"

cleanup() {
  rm -rf "${TMP_CONTEXT}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

ensure_dir() {
  local d="$1"
  if mkdir -p "${d}" >/dev/null 2>&1; then
    return
  fi

  if command -v sudo >/dev/null 2>&1; then
    echo "Creating mount directory with sudo: ${d}"
    if sudo mkdir -p "${d}" >/dev/null 2>&1; then
      return
    fi
  fi

  echo "Failed to create mount directory: ${d}"
  echo "Please run with sufficient permissions or override mount path env vars."
  exit 1
}

ensure_dir_permissions() {
  local d="$1"
  if chmod 0777 "${d}" >/dev/null 2>&1; then
    return
  fi

  if command -v sudo >/dev/null 2>&1; then
    echo "Adjusting directory permissions with sudo: ${d}"
    if sudo chmod 0777 "${d}" >/dev/null 2>&1; then
      return
    fi
  fi

  echo "Failed to set permissions on directory: ${d}"
  echo "Please run chmod 0777 '${d}' manually."
  exit 1
}

ensure_auth_file_permissions() {
  local d="$1"
  if [[ ! -d "${d}" ]]; then
    return
  fi

  if find "${d}" -maxdepth 1 -type f -exec chmod 0666 {} + >/dev/null 2>&1; then
    return
  fi

  if command -v sudo >/dev/null 2>&1; then
    echo "Adjusting auth file permissions with sudo: ${d}"
    if sudo find "${d}" -maxdepth 1 -type f -exec chmod 0666 {} + >/dev/null 2>&1; then
      return
    fi
  fi

  echo "Failed to set auth file permissions under: ${d}"
  echo "Please run chmod 0666 '${d}'/* manually if needed."
  exit 1
}

cd "${ROOT_DIR}"

if ! command -v "${DOCKER_BIN}" >/dev/null 2>&1; then
  echo "Docker CLI not found in PATH."
  echo "Please ensure Docker Desktop is installed and 'docker' is available in your shell."
  echo "You can also set DOCKER_BIN explicitly, e.g.:"
  echo "  DOCKER_BIN=/usr/local/bin/docker ./tools/docker.local/build_local_image.sh"
  exit 1
fi

if ! "${DOCKER_BIN}" image inspect "${UBUNTU_IMAGE}" >/dev/null 2>&1; then
  echo "Base image not found locally: ${UBUNTU_IMAGE}"
  echo "Please import or pull it first:"
  echo "  docker pull ${UBUNTU_IMAGE}"
  exit 1
fi

if ! "${DOCKER_BIN}" image inspect "${BUILD_IMAGE}" >/dev/null 2>&1 || \
  ! "${DOCKER_BIN}" run --rm --entrypoint /bin/bash "${BUILD_IMAGE}" -lc 'cmake --version >/dev/null 2>&1 && make --version >/dev/null 2>&1 && g++ --version >/dev/null 2>&1 && python3-config --includes >/dev/null 2>&1 && test -f /usr/include/python3.12/Python.h && ping -V >/dev/null 2>&1 && netstat -h >/dev/null 2>&1'; then
  echo "Build tools image missing or incomplete. Building: ${BUILD_IMAGE}"
  "${DOCKER_BIN}" build \
    --pull=false \
    --platform "${PLATFORM}" \
    -f "${LOCAL_DIR}/Dockerfile.buildtools" \
    -t "${BUILD_IMAGE}" \
    "${LOCAL_DIR}"
fi

if [[ "${EUID}" -eq 0 ]]; then
  echo "Warning: running with sudo may use a different Docker context/image store on macOS."
  echo "Prefer running without sudo if Docker Desktop is installed for current user."
fi

echo "[1/4] Build Linux binary in container via bind mount (${PLATFORM}, MODEL=${MODEL})..."
"${DOCKER_BIN}" run --rm \
  --platform "${PLATFORM}" \
  -v "${ROOT_DIR}:/workspace" \
  -w /workspace \
  "${BUILD_IMAGE}" \
  /bin/bash -lc "set -euo pipefail; mkdir -p '/workspace/${LINUX_BUILD_DIR_REL}' '/workspace/${LOCAL_RELEASE_DIR_REL}' && cd '/workspace/${LINUX_BUILD_DIR_REL}' && cmake -DENABLE_PYTHON=true -DCMAKE_BUILD_TYPE='${MODEL}' -DENABLE_WEBRTC=true -DENABLE_FFMPEG=true -DENABLE_TESTS=false -DENABLE_API=false /workspace && make -j \$(nproc) && cp '/workspace/release/linux/${MODEL}/MediaServer' '/workspace/${LOCAL_RELEASE_DIR_REL}/MediaServer' && cp '/workspace/release/linux/${MODEL}/config.ini' '/workspace/${LOCAL_RELEASE_DIR_REL}/config.ini'"

echo "[2/4] Collect required runtime artifacts..."
mkdir -p "${TMP_CONTEXT}/release/linux/${MODEL}"
cp "${LOCAL_RELEASE_DIR_REL}/MediaServer" "${TMP_CONTEXT}/release/linux/${MODEL}/MediaServer"
cp "${LOCAL_RELEASE_DIR_REL}/config.ini" "${TMP_CONTEXT}/release/linux/${MODEL}/config.ini"
cp -R "www" "${TMP_CONTEXT}/www"
cp "default.pem" "${TMP_CONTEXT}/default.pem"

if [[ ! -d "${TMP_CONTEXT}/www/webassist" ]]; then
  echo "Missing required web assets: www/webassist"
  echo "Please ensure the local repository contains webassist files before building."
  exit 1
fi

echo "[3/4] Verify binary format..."
file "${TMP_CONTEXT}/release/linux/${MODEL}/MediaServer"

echo "[4/4] Build local runtime image from minimal context..."
"${DOCKER_BIN}" build \
  --pull=false \
  --platform "${PLATFORM}" \
  -f "${LOCAL_DIR}/Dockerfile.local-runtime" \
  --build-arg "MODEL=${MODEL}" \
  -t "${IMAGE_TAG}" \
  "${TMP_CONTEXT}"

echo "[5/5] Ensure host mount directories exist..."
ensure_dir "${MOUNT_AUTH_DIR}"
ensure_dir "${MOUNT_LOG_DIR}"
ensure_dir "${MOUNT_RECORD_DIR}"
ensure_dir_permissions "${MOUNT_AUTH_DIR}"
ensure_dir_permissions "${MOUNT_LOG_DIR}"
ensure_dir_permissions "${MOUNT_RECORD_DIR}"
ensure_auth_file_permissions "${MOUNT_AUTH_DIR}"

if [[ "$(uname -s)" == "Darwin" ]]; then
  case "${MOUNT_AUTH_DIR}${MOUNT_LOG_DIR}${MOUNT_RECORD_DIR}" in
    *"/opt/"*|*"/data/"*)
      echo "Note: On macOS Docker Desktop, non-/Users mount paths may require File Sharing allowlist."
      echo "If startup fails with 'mounts denied', add those paths in Docker Desktop -> Settings -> Resources -> File Sharing."
      ;;
  esac
fi

echo

echo "Done. Runtime image: ${IMAGE_TAG}"
echo "Mount layout switch USE_DATA_LAYOUT=${USE_DATA_LAYOUT}"
echo "Local build output: ${LINUX_BUILD_DIR_REL}"
echo "Local release output: ${LOCAL_RELEASE_DIR_REL}"
echo "Runtime config source: image built-in /opt/media/conf/config.ini"
echo ""
echo "Run with same host/container mount paths:"
cat <<EOF
docker run --rm -d --name zlmediakit \
  -p 8089:8089 -p 554:554 \
  -v ${MOUNT_AUTH_DIR}:${MOUNT_AUTH_DIR} \
  -v ${MOUNT_LOG_DIR}:${MOUNT_LOG_DIR} \
  -v ${MOUNT_RECORD_DIR}:${MOUNT_RECORD_DIR} \
  ${IMAGE_TAG} \
  /opt/media/bin/MediaServer -s /opt/media/bin/default.pem -c /opt/media/conf/config.ini --log-dir ${MOUNT_LOG_DIR} -l 0
EOF
