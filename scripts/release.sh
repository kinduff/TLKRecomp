#!/usr/bin/env sh
# Build, check and pack the release packages into dist/:
# TLKRecomp-linux-x86_64.tar.gz, TLKRecomp-linux-x86_64.AppImage,
# TLKRecomp-windows-x86_64.{zip,exe} and SHA256SUMS.
#
# usage: scripts/release.sh -r ROM
# Generates the C from ROM, builds tlk_headless and tlk_sdl for Linux
# (build-linux/, in the Ubuntu 22.04 container of scripts/linux_container.sh,
# so the packages need no newer glibc than 2.35) and Windows (build-win/) with
# the settings scripts/package.sh and scripts/package_win.sh use, and compares
# compiled mode with --lle on both (Windows under Wine): attract mode at frames 600 and 3000, and the
# 6987M speedrun tape at frame 6000. Any MISMATCH, WRAM difference or missing
# hash stops it before anything is packed. Then the AppImage and the single
# .exe (under Wine) are run for 600 frames, with no window, and must show the
# same frame as the checks did. .gitea/workflows/release.yml runs this on a
# v* tag.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROM=""
while getopts "r:" opt; do
    case $opt in
        r) ROM=$OPTARG ;;
        *) sed -n '2,16p' "$0"; exit 2 ;;
    esac
done
[ -f "$ROM" ] || { sed -n '2,16p' "$0"; echo "ROM not found: '$ROM'" >&2; exit 2; }
JOBS=$(nproc 2>/dev/null || echo 4)
LINUX="$ROOT/build-linux"
GATE="$LINUX/release-gate"
DIST="$ROOT/dist"

python3 "$ROOT/scripts/generate_snesrecomp.py" --rom "$ROM"

"$ROOT/scripts/linux_container.sh" sh -c "cmake -S '$ROOT' -B '$LINUX' -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DTLK_STATIC_SDL=ON && cmake --build '$LINUX' --target tlk_headless tlk_sdl -j$JOBS"
# Nothing newer than Ubuntu 22.04's glibc (2.35) and libstdc++ (GLIBCXX 3.4.30).
for lib in GLIBC:2.35 GLIBCXX:3.4.30; do
    newest=$(objdump -T "$LINUX/tlk_sdl" | grep -o "${lib%:*}_[0-9.]*" | cut -d_ -f2 | sort -uV | tail -1)
    [ "$(printf '%s\n' "$newest" "${lib#*:}" | sort -V | tail -1)" = "${lib#*:}" ] ||
        { echo "tlk_sdl needs ${lib%:*} $newest, newer than ${lib#*:}" >&2; exit 1; }
done
cmake -S "$ROOT" -B "$ROOT/build-win" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake"
cmake --build "$ROOT/build-win" --target tlk_headless tlk_sdl -j"$JOBS"

rm -rf "$GATE"
mkdir -p "$GATE"
cat >"$GATE/wine_headless" <<EOF
#!/bin/sh
WINEDEBUG=-all exec wine "$ROOT/build-win/tlk_headless.exe" "\$@"
EOF
chmod +x "$GATE/wine_headless"

gate() { # name binary [compare_modes options] frame...
    name=$1 bin=$2; shift 2
    echo "== $name"
    "$ROOT/scripts/compare_modes.sh" -r "$ROM" -b "$bin" -o "$GATE/$name" "$@" | tee "$GATE/$name.txt" || true
    # Each frame needs MATCH, real hashes (not "none") and the same WRAM.
    want=$(($(grep -c . "$GATE/$name.txt") - 1))
    good=$(awk '$2 == "MATCH" && $3 != "none" && $5 == "same"' "$GATE/$name.txt" | wc -l)
    [ "$want" -gt 0 ] && [ "$good" -eq "$want" ] || {
        echo "$name: compiled mode differs from --lle ($good of $want frames good)" >&2; exit 1; }
}
gate linux "$LINUX/tlk_headless" 600 3000
gate linux-6987M "$LINUX/tlk_headless" -p "$ROOT/tapes/6987M.read.tape" 6000
gate windows "$GATE/wine_headless" 600 3000
gate windows-6987M "$GATE/wine_headless" -p "$ROOT/tapes/6987M.read.tape" 6000
# And both builds must agree with each other.
for run in "" -6987M; do
    awk '{print $1, $3, $4}' "$GATE/linux$run.txt" >"$GATE/linux$run.hashes"
    awk '{print $1, $3, $4}' "$GATE/windows$run.txt" >"$GATE/windows$run.hashes"
    cmp "$GATE/linux$run.hashes" "$GATE/windows$run.hashes" ||
        { echo "linux$run and windows$run hashes differ" >&2; exit 1; }
done

"$ROOT/scripts/linux_container.sh" "$ROOT/scripts/package.sh" -b "$LINUX"
"$ROOT/scripts/linux_container.sh" "$ROOT/scripts/package_appimage.sh" -b "$LINUX"
"$ROOT/scripts/package_win.sh" -b "$ROOT/build-win"

# The packed players start and show the frame the checks saw at 600. No
# window (dummy video), no FUSE (extract-and-run) and no AppImageLauncher
# (it would ask to integrate the file).
want=$(awk '$1 == 600 {print $3}' "$GATE/linux.txt")
run_pkg() { # name command...
    name=$1; shift
    got=$(SDL_VIDEO_DRIVER=dummy APPIMAGE_EXTRACT_AND_RUN=1 APPIMAGELAUNCHER_DISABLE=1 WINEDEBUG=-all \
          "$@" --rom "$ROM" --no-audio --frames 600 2>/dev/null | sed -n 's/^frame_sha256=//p' | cut -c1-16)
    echo "$name f600: ${got:-none} (want $want)"
    [ "$got" = "$want" ] || { echo "$name: wrong or no frame" >&2; exit 1; }
}
run_pkg AppImage "$DIST/TLKRecomp-linux-x86_64.AppImage"
run_pkg exe wine "$DIST/TLKRecomp-windows-x86_64.exe"

(cd "$DIST" && sha256sum TLKRecomp-linux-x86_64.tar.gz TLKRecomp-linux-x86_64.AppImage \
    TLKRecomp-windows-x86_64.zip TLKRecomp-windows-x86_64.exe >SHA256SUMS)
cat "$DIST/SHA256SUMS"
