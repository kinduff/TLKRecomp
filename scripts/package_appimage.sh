#!/usr/bin/env sh
# Build the Linux AppImage: dist/TLKRecomp-x86_64.AppImage (no "linux" in the
# name: every AppImage is for Linux, and the AppImage catalog flags it).
#
# usage: scripts/package_appimage.sh [-b builddir]
# Builds tlk_sdl in builddir (default build-pkg/, configured as
# scripts/package.sh does, SDL3 linked in), lays out an AppDir (tlk_sdl and
# assets/ in usr/bin, the notes in usr/share/doc, icon and .desktop) and packs
# it with appimagetool. appimagetool and the AppImage runtime are pinned
# releases, downloaded once into builddir/appimage-tools and checked by
# SHA-256. AppRun puts tlk_sdl.ini, saves and recordings beside the
# .AppImage file, and a ROM there is found. Never packs the ROM or game C.
# The AppImage carries AppStream metainfo (packaging/linux) and update
# information for AppImageUpdate: the latest GitHub release's
# TLKRecomp-x86_64.AppImage.zsync, written next to the AppImage here and
# published with it by .gitea/workflows/release.yml.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build-pkg"
while getopts "b:" opt; do
    case $opt in
        b) BUILD=$OPTARG ;;
        *) sed -n '2,12p' "$0"; exit 2 ;;
    esac
done
case $BUILD in /*) ;; *) BUILD="$PWD/$BUILD" ;; esac   # appimagetool runs from dist/
NAME=TLKRecomp-x86_64
DIST="$ROOT/dist"
APPDIR="$DIST/TLKRecomp.AppDir"
TOOLS="$BUILD/appimage-tools"

fetch() { # url file sha256
    if [ ! -f "$TOOLS/$2" ] || ! echo "$3  $TOOLS/$2" | sha256sum -c --quiet 2>/dev/null; then
        mkdir -p "$TOOLS"
        curl -fsSL -o "$TOOLS/$2" "$1"
        echo "$3  $TOOLS/$2" | sha256sum -c --quiet
    fi
    chmod +x "$TOOLS/$2"
}
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
      appimagetool-x86_64.AppImage ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
      runtime-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON
cmake --build "$BUILD" --target tlk_sdl -j"$(nproc 2>/dev/null || echo 4)"
rm -rf "$APPDIR"
cmake --install "$BUILD" --prefix "$APPDIR/usr"
strip "$APPDIR/usr/bin/tlk_sdl"
if ldd "$APPDIR/usr/bin/tlk_sdl" | grep -qi sdl; then
    echo "tlk_sdl still links a shared SDL" >&2; exit 1
fi

cp "$ROOT/packaging/icon/tlkrecomp.png" "$APPDIR/tlkrecomp.png"
ln -s tlkrecomp.png "$APPDIR/.DirIcon"
# The .desktop file is named after the AppStream id, which its metainfo
# names as the launchable.
APPID=io.github.kinduff.TLKRecomp
cat >"$APPDIR/$APPID.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=TLKRecomp
Comment=The Lion King (SNES), recompiled. Needs your own ROM.
Exec=tlk_sdl
Icon=tlkrecomp
Categories=Game;
Terminal=false
EOF
mkdir -p "$APPDIR/usr/share/applications" "$APPDIR/usr/share/metainfo"
cp "$APPDIR/$APPID.desktop" "$APPDIR/usr/share/applications/"
cp "$ROOT/packaging/linux/$APPID.appdata.xml" "$APPDIR/usr/share/metainfo/"
cat >"$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# Settings, saves and the ROM live beside the .AppImage, like the tarball's
# files beside tlk_sdl. $TLK_SDL_INI or --config still choose another ini.
HERE=$(dirname "$(readlink -f "$0")")
if [ -z "${TLK_SDL_INI:-}" ] && [ -n "${APPIMAGE:-}" ]; then
    TLK_SDL_INI="$(dirname "$APPIMAGE")/tlk_sdl.ini"
    export TLK_SDL_INI
fi
exec "$HERE/usr/bin/tlk_sdl" "$@"
EOF
chmod +x "$APPDIR/AppRun"

if find "$APPDIR" -iname '*.sfc' -o -iname '*.smc' -o -name '*.c' -o -name '*.h' | grep -q .; then
    echo "ROM or source file in AppDir" >&2; exit 1
fi

# Extract-and-run: no FUSE needed on the build machine.
# Run in dist/: appimagetool writes the .zsync into the current directory.
rm -f "$DIST/$NAME.AppImage.zsync"
(cd "$DIST" && APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$TOOLS/appimagetool-x86_64.AppImage" --no-appstream \
    -u "gh-releases-zsync|kinduff|TLKRecomp|latest|$NAME.AppImage.zsync" \
    --runtime-file "$TOOLS/runtime-x86_64" "$APPDIR" "$NAME.AppImage")
[ -s "$DIST/$NAME.AppImage.zsync" ] || { echo "appimagetool wrote no $NAME.AppImage.zsync" >&2; exit 1; }
rm -rf "$APPDIR"
echo "$DIST/$NAME.AppImage"
