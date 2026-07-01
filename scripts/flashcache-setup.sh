#!/bin/bash
# Set up FlashCache for local development.
# Copies FlashCache source from key-spilling workspace, builds it,
# and wires it up so the module can be built with --features backend-flashcache.
#
# Prerequisites:
#   - cmake3 installed (sudo yum install cmake3)
#   - libaio-dev installed (sudo yum install libaio-devel)
#   - key-spilling workspace at ~/workspace/key-spilling (or set FLASHCACHE_SRC)
#
# Usage: ./scripts/flashcache-setup.sh
#
# After running this script, build the module with FlashCache:
#   cd modules/key-spilling
#   cargo build --release --features backend-flashcache

set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FLASHCACHE_DST="$REPO_ROOT/src/flashcache"

# Source: prefer env var, then key-spilling workspace, then sibling FlashCache
FLASHCACHE_SRC="${FLASHCACHE_SRC:-}"
if [ -z "$FLASHCACHE_SRC" ]; then
    if [ -d "$HOME/workspace/key-spilling/src/flashcache" ]; then
        FLASHCACHE_SRC="$HOME/workspace/key-spilling/src/flashcache"
    elif [ -d "$REPO_ROOT/../FlashCache" ]; then
        FLASHCACHE_SRC="$REPO_ROOT/../FlashCache"
    else
        echo "ERROR: Cannot find FlashCache source."
        echo "Set FLASHCACHE_SRC=/path/to/flashcache or ensure ~/workspace/key-spilling/src/flashcache exists."
        exit 1
    fi
fi

echo "FlashCache source: $FLASHCACHE_SRC"

# Copy if not already present
if [ ! -d "$FLASHCACHE_DST" ]; then
    echo "Copying FlashCache to src/flashcache/..."
    cp -r "$FLASHCACHE_SRC" "$FLASHCACHE_DST"
fi

# Build if libflashcache.a doesn't exist
if [ ! -f "$FLASHCACHE_DST/build/libflashcache.a" ]; then
    echo "Building FlashCache..."
    mkdir -p "$FLASHCACHE_DST/build"
    cd "$FLASHCACHE_DST/build"
    cmake3 .. -DCMAKE_BUILD_TYPE=Release 2>/dev/null || \
        cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    echo "FlashCache built: $FLASHCACHE_DST/build/libflashcache.a"
else
    echo "FlashCache already built: $FLASHCACHE_DST/build/libflashcache.a"
fi

# Verify the module build.rs can find it
echo ""
echo "FlashCache is ready. Build the module with:"
echo "  cd $REPO_ROOT/modules/key-spilling"
echo "  cargo build --release --features backend-flashcache"
