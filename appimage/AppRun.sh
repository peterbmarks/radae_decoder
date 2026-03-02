#!/bin/bash -e
echo "In AppImage AppRun"
export LD_LIBRARY_PATH="${APPIMAGE_LIBRARY_PATH}:${APPDIR}/usr/lib:${LD_LIBRARY_PATH}"
export PATH="$APPDIR/usr/bin"
echo "PATH=$PATH"
cd "$APPDIR"
echo "#### after import"
"$APPDIR/usr/bin/RADAE_Gui" "$@"
