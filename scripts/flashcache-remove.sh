#!/bin/bash
# Remove FlashCache from the repo before committing.
# FlashCache is not upstreamed — this script strips it for clean commits.
#
# Usage: ./scripts/flashcache-remove.sh

set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ -d "$REPO_ROOT/src/flashcache" ]; then
    rm -rf "$REPO_ROOT/src/flashcache"
    echo "Removed src/flashcache/"
else
    echo "src/flashcache/ not present — nothing to remove."
fi
