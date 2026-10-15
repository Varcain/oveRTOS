#!/bin/bash
# Build QEMU for WebAssembly: qemu-system-arm with the MPS2 boards only.
#
# This is the emulator behind the browser demos: it runs the same
# qemu-mps2-an500 firmware as the native qemu-system-arm.  The build
# follows upstream's own recipe (the emsdk-wasm64-cross container and the
# build-wasm64-32bit CI job) on the release tarball, plus the device list
# in config/qemu-wasm/mps2.mak, --disable-debug-info, and the patches in
# config/qemu-wasm/patches/ (applied in name order; each one says why).
#
# The WebAssembly host has no JIT backend upstream, so QEMU runs its TCG
# interpreter (--enable-tcg-interpreter is mandatory).
#
# Usage: build-qemu-wasm.sh <qemu-X.Y.Z.tar.xz> <work-dir> <out-dir>
#
#   <work-dir>  scratch space for the extracted source and the build tree
#   <out-dir>   receives qemu-system-arm.js, qemu-system-arm.wasm and
#               SOURCE.txt (what the binary was built from)
#
# Needs Docker or Podman.  OVE_CONTAINER_ENGINE overrides the choice.

set -euo pipefail

TARBALL="${1:?Usage: $0 <qemu-X.Y.Z.tar.xz> <work-dir> <out-dir>}"
WORK="${2:?Usage: $0 <qemu-X.Y.Z.tar.xz> <work-dir> <out-dir>}"
OUT="${3:?Usage: $0 <qemu-X.Y.Z.tar.xz> <work-dir> <out-dir>}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEVICES="${SCRIPT_DIR}/../qemu-wasm/mps2.mak"
PATCHES=("${SCRIPT_DIR}"/../qemu-wasm/patches/*.patch)
[ -e "${PATCHES[0]}" ] || PATCHES=()

ENGINE="${OVE_CONTAINER_ENGINE:-}"
if [ -z "${ENGINE}" ]; then
    ENGINE="$(command -v docker || command -v podman || true)"
fi
if [ -z "${ENGINE}" ]; then
    echo "[qemu-wasm] ERROR: neither docker nor podman found" >&2
    exit 1
fi

VERSION="$(basename "${TARBALL}" .tar.xz)"
VERSION="${VERSION#qemu-}"
IMAGE="ove-qemu-emsdk:${VERSION}"
CONFIGURE_ARGS="--static --cpu=wasm64 --wasm64-32bit-address-limit \
--enable-tcg-interpreter --target-list=arm-softmmu \
--without-default-devices --with-devices-arm=ove-mps2 \
--disable-tools --disable-docs --disable-debug-info"

mkdir -p "${WORK}" "${OUT}"
WORK="$(cd "${WORK}" && pwd)"
OUT="$(cd "${OUT}" && pwd)"
SRC="${WORK}/src"
BUILD="${WORK}/build"

# Always start from the pristine release, so the patch set applies cleanly
# and the build tree never mixes in an earlier recipe.
echo "[qemu-wasm] extracting $(basename "${TARBALL}")"
rm -rf "${SRC}" "${WORK}/build"
mkdir -p "${SRC}"
tar xf "${TARBALL}" -C "${SRC}" --strip-components=1
for p in "${PATCHES[@]}"; do
    echo "[qemu-wasm] applying $(basename "${p}")"
    patch -d "${SRC}" -p1 --forward --quiet < "${p}"
done
cp "${DEVICES}" "${SRC}/configs/devices/arm-softmmu/ove-mps2.mak"

# The container image holds emsdk plus QEMU's dependencies (glib, pixman,
# libffi, zlib) cross-built for wasm64.  It takes no build context.
echo "[qemu-wasm] building the ${IMAGE} container image"
mkdir -p "${WORK}/empty-context"
"${ENGINE}" build -t "${IMAGE}" \
    -f "${SRC}/tests/docker/dockerfiles/emsdk-wasm64-cross.docker" \
    "${WORK}/empty-context"

# The container runs as root; hand the trees back to the caller on exit.
echo "[qemu-wasm] building qemu-system-arm"
mkdir -p "${BUILD}"
"${ENGINE}" run --rm -v "${SRC}:/qemu" -v "${BUILD}:/build" -w /build "${IMAGE}" \
    bash -c "
        trap 'chown -R $(id -u):$(id -g) /build /qemu' EXIT
        set -e
        if [ ! -f build.ninja ]; then
            emconfigure /qemu/configure ${CONFIGURE_ARGS}
        fi
        emmake make -j\$(nproc)
    "

cp "${BUILD}/qemu-system-arm.js" "${BUILD}/qemu-system-arm.wasm" "${OUT}/"
cat > "${OUT}/SOURCE.txt" <<SOURCE_EOF
qemu-system-arm.js and qemu-system-arm.wasm are QEMU ${VERSION}, licensed
under the GNU GPL version 2 (https://www.qemu.org/).

Built from the upstream release
  https://download.qemu.org/$(basename "${TARBALL}")
  sha256 $(sha256sum "${TARBALL}" | cut -d' ' -f1)
with the patches listed at the end of this file applied, and compiled with
Emscripten using upstream's tests/docker/dockerfiles/emsdk-wasm64-cross.docker
and

  emconfigure ./configure ${CONFIGURE_ARGS}

where configs/devices/arm-softmmu/ove-mps2.mak is:

$(sed 's/^/  /' "${DEVICES}")
SOURCE_EOF
for p in "${PATCHES[@]}"; do
    printf '\n==> patch %s\n\n' "$(basename "${p}")" >> "${OUT}/SOURCE.txt"
    cat "${p}" >> "${OUT}/SOURCE.txt"
done

echo "[qemu-wasm] done: ${OUT}"
ls -l "${OUT}"
