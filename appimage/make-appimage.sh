#!/bin/bash -e

TARGET=${1:-all}

export APPNAME="RADAE_Gui"
export APPEXEC=../build/RADAE_Gui
export APPRUN="AppRun.sh"

DESKTOP_FILE="$APPNAME.desktop"
APPDIR="$APPNAME.AppDir"
BUILDDIR="../"
MACH_ARCH=`uname -m`

export NO_STRIP=1

# Change to the directory where this script is located
cd "$(dirname "$(realpath "$0")")"

if [ -d "$APPDIR" ]; then
    echo "Deleting $APPDIR..."
    rm -rf "$APPDIR"
else
    echo "$APPDIR does not exist."
fi

# uutils coreutils cp (the Rust cp shipped as /usr/bin/cp on newer Ubuntu)
# wrongly fails "cp DIR --archive --parents" with "are the same file", which
# breaks linuxdeploy-plugin-gtk.sh while copying the GLib schemas.  If GNU cp
# is available as gnucp, put it first on PATH as "cp" for this script only.
if command -v gnucp >/dev/null 2>&1 && ! cp --version 2>/dev/null | grep -q "GNU coreutils"; then
    SYSTEM_CP="$(cp --version 2>/dev/null | head -1)"
    CP_SHIM_DIR="$(mktemp -d)"
    trap 'rm -rf "$CP_SHIM_DIR"' EXIT
    ln -s "$(command -v gnucp)" "$CP_SHIM_DIR/cp"
    export PATH="$CP_SHIM_DIR:$PATH"
    echo "Using GNU cp (gnucp) instead of ${SYSTEM_CP:-system cp}"
fi

echo "Bundle dependencies..."
if test -f linuxdeploy-${MACH_ARCH}.AppImage; then
  echo "linuxdeploy exists"
else
    wget -c "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
    wget https://github.com/linuxdeploy/linuxdeploy/releases/latest/download/linuxdeploy-${MACH_ARCH}.AppImage
    chmod +x linuxdeploy-${MACH_ARCH}.AppImage linuxdeploy-plugin-gtk.sh
fi

./linuxdeploy-${MACH_ARCH}.AppImage \
--executable "$APPEXEC" \
--appdir "$APPDIR" \
--icon-file freedv256x256.png \
--custom-apprun=$APPRUN \
--desktop-file $DESKTOP_FILE

# Create the output
./linuxdeploy-${MACH_ARCH}.AppImage \
    --appdir "$APPDIR" \
    --plugin gtk \
    --output appimage

echo "Done"
