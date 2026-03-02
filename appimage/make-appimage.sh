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
