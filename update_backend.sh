#!/bin/bash
#
# update_backend.sh - Update the extern/rade_c submodule to the latest
#                     commit on its main branch.
#
# Usage:
#   ./update_backend.sh
#
# The submodule change is left unstaged so you can rebuild and test
# before committing it.

set -euo pipefail

SUBMODULE=extern/rade_c
BRANCH=main

cd "$(dirname "$0")"

git submodule update --init "$SUBMODULE"

OLD=$(git -C "$SUBMODULE" rev-parse HEAD)

git -C "$SUBMODULE" fetch origin "$BRANCH"
git -C "$SUBMODULE" checkout --quiet "$BRANCH"
git -C "$SUBMODULE" merge --ff-only --quiet "origin/$BRANCH"

NEW=$(git -C "$SUBMODULE" rev-parse HEAD)

if [ "$OLD" = "$NEW" ]; then
    echo "$SUBMODULE is already up to date at ${NEW:0:12}."
    exit 0
fi

echo "Updated $SUBMODULE: ${OLD:0:12} -> ${NEW:0:12}"
echo
git -C "$SUBMODULE" log --oneline "$OLD..$NEW"
echo
echo "To record the update:"
echo "  git add $SUBMODULE && git commit -m \"Update rade_c to ${NEW:0:12}\""
