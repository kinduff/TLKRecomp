#!/usr/bin/env sh
# Run a command in the Linux release build container (Ubuntu 22.04, glibc 2.35;
# packaging/linux/Containerfile), so the Linux packages run on any current
# distribution and not only on ones as new as the build machine.
#
# usage: scripts/linux_container.sh COMMAND [ARG...]
# e.g.   scripts/linux_container.sh scripts/package_appimage.sh -b build-linux
# The command runs in the repo root.
# Builds the image on first use (podman, or docker). The repo is mounted at
# its own path, so build dirs and paths read the same inside and out; files
# are written as the calling user. ccache keeps its own cache for this
# compiler in ~/.cache/tlkrecomp-ccache-ubuntu22.04.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ $# -gt 0 ] || { sed -n '2,12p' "$0"; exit 2; }
IMAGE=tlkrecomp-linux-build:ubuntu22.04
CCACHE="$HOME/.cache/tlkrecomp-ccache-ubuntu22.04"
if command -v podman >/dev/null 2>&1; then
    ENGINE=podman USER_OPTS="--userns=keep-id"
else
    ENGINE=docker USER_OPTS="--user $(id -u):$(id -g)"
fi

"$ENGINE" build -q -t "$IMAGE" -f "$ROOT/packaging/linux/Containerfile" "$ROOT/packaging/linux" >/dev/null
mkdir -p "$CCACHE"
# shellcheck disable=SC2086 # USER_OPTS is two words for docker
exec "$ENGINE" run --rm $USER_OPTS \
    -v "$ROOT:$ROOT" -w "$ROOT" -v "$CCACHE:/ccache" \
    -e CCACHE_DIR=/ccache -e CCACHE_BASEDIR="$ROOT" -e CCACHE_NOHASHDIR=1 -e HOME=/tmp \
    "$IMAGE" "$@"
