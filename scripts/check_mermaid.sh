#!/usr/bin/env bash
# Validate all mermaid diagrams in markdown files using mmdc (mermaid-cli).
# Skips if mmdc or Chrome is not available.
# Usage: ./scripts/check_mermaid.sh

set -euo pipefail

MMDM_FILES=$(find . -name "*.md" \
    -not -path "./build/*" \
    -not -path "./docs/api/*" \
    -not -path "./.git/*" \
    2>/dev/null)

if [ -z "$MMDM_FILES" ]; then
    echo "No markdown files found; skipping mermaid check."
    exit 0
fi

if ! command -v mmdc >/dev/null 2>&1; then
    echo "mmdc (mermaid-cli) not found; skipping mermaid check."
    echo "Install with: npm install -g @mermaid-js/mermaid-cli"
    exit 0
fi

error_count=0
tmp_mmd=$(mktemp /tmp/mermaid_XXXXXX.mmd)
trap 'rm -f "$tmp_mmd"' EXIT

while IFS= read -r md_file; do
    block_idx=0
    # Use NUL as record separator so multi-line blocks are read as one unit
    while IFS= read -r -d '' block || [ -n "$block" ]; do
        [ -z "$block" ] && continue
        block_idx=$((block_idx + 1))
        printf '%s\n' "$block" > "$tmp_mmd"
        if ! mmdc -i "$tmp_mmd" -o /dev/null >/dev/null 2>&1; then
            echo "FAIL: mermaid diagram #$block_idx in $md_file"
            error_count=$((error_count + 1))
        fi
    done < <(awk '
        /^[[:space:]]*```mermaid/ { in_block=1; block=""; next }
        in_block && /^```/ {
            in_block=0
            printf "%s\0", block
            block=""
            next
        }
        in_block { block = block (block=="" ? "" : "\n") $0 }
    ' "$md_file")
done <<< "$MMDM_FILES"

if [ "$error_count" -gt 0 ]; then
    echo "Mermaid check failed: $error_count diagram(s) had errors."
    exit 1
fi

echo "All mermaid diagrams valid."
