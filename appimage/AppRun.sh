#!/bin/bash -e
echo "In AppImage AppRun"
export LD_LIBRARY_PATH="${APPIMAGE_LIBRARY_PATH}:${APPDIR}/usr/lib:${LD_LIBRARY_PATH}"
# Keep the system PATH after our own bin: GTK's image loader (glycin) spawns
# its sandboxed loaders via "bwrap" from PATH, and aborts if it can't find it.
export PATH="$APPDIR/usr/bin:$PATH"
echo "PATH=$PATH"
cd "$APPDIR"
echo "#### after import"
"$APPDIR/usr/bin/RADAE_Gui" "$@"
