#!/usr/bin/env bash
# Verify the shared library's exported symbols exactly match the ABI whitelist.
#
# Usage: scripts/check_abi_exports.sh [path-to-shared-lib]
#   Default library: build/libclogx.so (or build/libclogx.0.dylib on macOS).
#
# Checks in both directions:
#   1. every symbol in clogx.map's global block is exported by the library;
#   2. every symbol the library exports is listed in clogx.map;
#   3. clogx.exports (macOS) matches the clogx.map global block exactly.
set -euo pipefail

LIB="${1:-}"
if [ -z "$LIB" ]; then
    if [ -f build/libclogx.so ]; then
        LIB=build/libclogx.so
    elif [ -f build/libclogx.0.dylib ]; then
        LIB=build/libclogx.0.dylib
    else
        echo "FAIL: no shared library found (pass a path as arg 1)" >&2
        exit 1
    fi
fi
MAP="clogx.map"

# Symbol names listed in the version script's global block.
map_symbols() {
    sed -n '/^CLOGX_/,/^};/p' "$MAP" \
        | grep -E '^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*;' \
        | sed 's/;.*//; s/^[[:space:]]*//'
}

# Text symbols actually exported by the library, sans version/@ and leading _.
exported_symbols() {
    if [ "$(uname -s)" = "Darwin" ]; then
        nm -gU "$LIB" | awk '$2 == "T" {sub(/^_/, "", $3); print $3}'
    else
        nm -D --defined-only "$LIB" | awk '$2 == "T" {sub(/@@.*/, "", $3); print $3}'
    fi
}

fail=0

# Direction 1: whitelisted but missing from the library.
while read -r sym; do
    [ -z "$sym" ] && continue
    if ! exported_symbols | grep -qx "$sym"; then
        echo "FAIL: '$sym' listed in $MAP but NOT exported by $LIB" >&2
        fail=1
    fi
done < <(map_symbols)

# Direction 2: exported but not whitelisted (accidental leak).
while read -r sym; do
    [ -z "$sym" ] && continue
    if ! map_symbols | grep -qx "$sym"; then
        echo "FAIL: '$sym' exported by $LIB but NOT listed in $MAP" >&2
        fail=1
    fi
done < <(exported_symbols)

# macOS export list must match the version script's global block.
if [ -f clogx.exports ]; then
    if ! diff -q <(map_symbols) clogx.exports >/dev/null 2>&1; then
        echo "FAIL: clogx.exports differs from the clogx.map global block" >&2
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "ABI export check FAILED" >&2
    exit 1
fi
echo "OK: ABI exports match ($(map_symbols | wc -l | tr -d ' ') symbols)"
