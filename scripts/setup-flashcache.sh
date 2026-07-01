#!/bin/bash
#
# setup-flashcache.sh — Build FlashCache and wire it into the key-spilling module.
#
# This script:
#   1. Builds FlashCache as a static library
#   2. Copies fc_shim.c into the module's flashcache backend directory
#   3. Builds the key-spilling module with backend-flashcache enabled
#   4. Verifies the build succeeded
#
# Usage:
#   ./scripts/setup-flashcache.sh
#
# Prerequisites:
#   - FlashCache source at ~/workspace/FlashCache (or set FLASHCACHE_SRC)
#   - cmake, make, gcc installed
#   - libaio-dev installed (yum install libaio-devel / apt install libaio-dev)
#   - Rust toolchain (cargo) installed
#
# Environment variables (optional):
#   FLASHCACHE_SRC    Path to FlashCache source tree (default: ~/workspace/FlashCache)
#   FC_SHIM_SRC       Path to fc_shim.c (default: ~/workspace/key-spilling/src/key-spilling-module/src/fc_shim.c)
#   SKIP_FC_BUILD     Set to 1 to skip FlashCache build (use existing libflashcache.a)
#   SKIP_MODULE_BUILD Set to 1 to skip module build (just setup files)
#

set -e

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VALKEY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODULE_DIR="${VALKEY_ROOT}/modules/key-spilling"
FC_BACKEND_DIR="${MODULE_DIR}/src/backends/flashcache"

# FlashCache source location — prefer repo-local copy, fall back to workspace
if [ -d "${VALKEY_ROOT}/FlashCache" ]; then
    FLASHCACHE_SRC="${FLASHCACHE_SRC:-${VALKEY_ROOT}/FlashCache}"
else
    FLASHCACHE_SRC="${FLASHCACHE_SRC:-${HOME}/workspace/FlashCache}"
fi

# fc_shim.c source — try multiple locations
FC_SHIM_SRC="${FC_SHIM_SRC:-}"
if [ -z "${FC_SHIM_SRC}" ]; then
    # Try common locations
    for candidate in \
        "${HOME}/workspace/key-spilling/src/key-spilling-module/src/fc_shim.c" \
        "${VALKEY_ROOT}/../key-spilling/src/key-spilling-module/src/fc_shim.c" \
        "${MODULE_DIR}/src/fc_shim.c.bak" \
    ; do
        if [ -f "${candidate}" ]; then
            FC_SHIM_SRC="${candidate}"
            break
        fi
    done
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

check_command() {
    if ! command -v "$1" &>/dev/null; then
        error "$1 is required but not installed. $2"
    fi
}

# ---------------------------------------------------------------------------
# Preflight checks
# ---------------------------------------------------------------------------

echo ""
echo "============================================================"
echo "  FlashCache Setup for Key-Spilling Module"
echo "============================================================"
echo ""
echo "  Valkey root:     ${VALKEY_ROOT}"
echo "  Module dir:      ${MODULE_DIR}"
echo "  FlashCache src:  ${FLASHCACHE_SRC}"
echo "  fc_shim.c:       ${FC_SHIM_SRC:-<not found>}"
echo ""

check_command cmake "Install with: sudo yum install cmake / sudo apt install cmake"
check_command make  "Install with: sudo yum install make / sudo apt install make"
check_command cargo "Install Rust: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"

# Check libaio
if ! ldconfig -p 2>/dev/null | grep -q libaio; then
    if [ ! -f /usr/lib64/libaio.so ] && [ ! -f /usr/lib/aarch64-linux-gnu/libaio.so ]; then
        warn "libaio not found. Install with: sudo yum install libaio-devel"
        warn "Continuing anyway — build may fail at link time."
    fi
fi

# Check FlashCache source
if [ ! -d "${FLASHCACHE_SRC}" ]; then
    error "FlashCache source not found at ${FLASHCACHE_SRC}
  Set FLASHCACHE_SRC=/path/to/FlashCache and re-run."
fi

if [ ! -f "${FLASHCACHE_SRC}/CMakeLists.txt" ]; then
    error "No CMakeLists.txt in ${FLASHCACHE_SRC} — is this the right directory?"
fi

# Check fc_shim.c
if [ -z "${FC_SHIM_SRC}" ] || [ ! -f "${FC_SHIM_SRC}" ]; then
    error "fc_shim.c not found.
  Set FC_SHIM_SRC=/path/to/fc_shim.c and re-run.
  Searched:
    ~/workspace/key-spilling/src/key-spilling-module/src/fc_shim.c
    ${VALKEY_ROOT}/../key-spilling/src/key-spilling-module/src/fc_shim.c"
fi

# ---------------------------------------------------------------------------
# Step 1: Build FlashCache
# ---------------------------------------------------------------------------

if [ "${SKIP_FC_BUILD}" == "1" ]; then
    info "Skipping FlashCache build (SKIP_FC_BUILD=1)"
else
    info "Step 1/4: Building FlashCache..."
    FC_BUILD_DIR="${FLASHCACHE_SRC}/build"
    mkdir -p "${FC_BUILD_DIR}"

    (
        cd "${FC_BUILD_DIR}"
        cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON 2>&1 | tail -5
        make -j$(nproc) 2>&1 | tail -10
    )

    # Verify the static library was produced
    if [ -f "${FC_BUILD_DIR}/libflashcache.a" ]; then
        info "  Built: ${FC_BUILD_DIR}/libflashcache.a ($(du -h ${FC_BUILD_DIR}/libflashcache.a | cut -f1))"
    else
        # Some builds produce it in a subdirectory
        FC_LIB=$(find "${FC_BUILD_DIR}" -name "libflashcache.a" -type f | head -1)
        if [ -n "${FC_LIB}" ]; then
            info "  Built: ${FC_LIB}"
        else
            error "libflashcache.a not found after build. Check build output above."
        fi
    fi
fi

# ---------------------------------------------------------------------------
# Step 2: Create symlink for build.rs to find FlashCache
# ---------------------------------------------------------------------------

info "Step 2/4: Setting up FlashCache symlink for build.rs..."

# build.rs looks for ../flashcache/build relative to the module directory
FC_LINK="${MODULE_DIR}/../flashcache"
if [ -L "${FC_LINK}" ]; then
    rm "${FC_LINK}"
fi
if [ ! -e "${FC_LINK}" ]; then
    ln -sf "${FLASHCACHE_SRC}" "${FC_LINK}"
    info "  Symlinked: ${FC_LINK} -> ${FLASHCACHE_SRC}"
else
    warn "  ${FC_LINK} already exists (not a symlink). build.rs should find it."
fi

# ---------------------------------------------------------------------------
# Step 3: Copy fc_shim.c
# ---------------------------------------------------------------------------

info "Step 3/4: Copying fc_shim.c to backend directory..."

mkdir -p "${FC_BACKEND_DIR}"
cp "${FC_SHIM_SRC}" "${FC_BACKEND_DIR}/fc_shim.c"
info "  Copied: ${FC_SHIM_SRC}"
info "      ->  ${FC_BACKEND_DIR}/fc_shim.c"

# ---------------------------------------------------------------------------
# Step 4: Build the module with FlashCache
# ---------------------------------------------------------------------------

if [ "${SKIP_MODULE_BUILD}" == "1" ]; then
    info "Skipping module build (SKIP_MODULE_BUILD=1)"
else
    info "Step 4/4: Building key-spilling module with FlashCache..."
    (
        cd "${MODULE_DIR}"
        cargo build --release --features backend-flashcache 2>&1 | tail -5
    )

    # Verify
    MODULE_SO="${MODULE_DIR}/target/release/libkey_spilling_module.so"
    if [ -f "${MODULE_SO}" ]; then
        info "  Built: ${MODULE_SO} ($(du -h ${MODULE_SO} | cut -f1))"

        # Check for FlashCache symbols
        FC_SYMS=$(nm "${MODULE_SO}" 2>/dev/null | grep -c "flashcache" || true)
        if [ "${FC_SYMS}" -gt 0 ]; then
            info "  FlashCache symbols present: ${FC_SYMS} references ✓"
        else
            warn "  No FlashCache symbols found — check if feature was enabled"
        fi
    else
        error "Module .so not found after build. Check cargo output above."
    fi
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------

echo ""
echo "============================================================"
echo "  FlashCache Setup Complete"
echo "============================================================"
echo ""
echo "  Module:  ${MODULE_DIR}/target/release/libkey_spilling_module.so"
echo "  Backend: flashcache + rocksdb (both enabled)"
echo ""
echo "  To run locally:"
echo "    fallocate -l 1G /tmp/fc-test.dat"
echo "    ./src/valkey-server \\"
echo "        --loadmodule modules/key-spilling/target/release/libkey_spilling_module.so \\"
echo "        backend=flashcache db_path=/tmp/fc-test.dat db_size_bytes=1073741824 \\"
echo "        num_databases=16 max_in_flight_reads=128 \\"
echo "        --maxmemory 100mb --maxmemory-policy allkeys-lru"
echo ""
echo "  To benchmark locally:"
echo "    python3 perf/local-benchmark/run_benchmark.py --backend flashcache --maxmemory 80mb"
echo ""
echo "  To run on ezBench:"
echo "    cd perf/tiering-flashcache && ./run.sh"
echo ""
