#!/usr/bin/env bash
# Calculate C source-level branch coverage using gcov -b.
# Usage: ./scripts/gcov_branch_summary.sh [build_dir] [threshold]

BUILD_DIR="${1:-build}"
THRESHOLD="${2:-75.0}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory '$BUILD_DIR' not found."
    exit 1
fi

GCDA_FILES=$(find "$BUILD_DIR" -maxdepth 1 -name "*.gcda" ! -name "test_*" ! -name "verify_*" 2>/dev/null)

if [ -z "$GCDA_FILES" ]; then
    echo "No core .gcda files found in $BUILD_DIR"
    exit 1
fi

gcov -b $GCDA_FILES 2>/dev/null | awk -v threshold="$THRESHOLD" '
BEGIN {
    cur_file = ""
    total_hit = 0
    total_br = 0
    file_count = 0
    printf "\n=================== GCOV C-SOURCE BRANCH COVERAGE ===================\n"
    printf "%-35s | %-10s | %-8s | %-10s\n", "Source File", "Executed", "Total", "Coverage"
    printf "---------------------------------------------------------------------------\n"
}

{
    line = $0
    if (index(line, "File") == 1) {
        sub(/^File[ \t]*[\x27\x60\x91\x92\xE2\x80\x98\xE2\x80\x99]/, "", line)
        sub(/[\x27\x60\x91\x92\xE2\x80\x98\xE2\x80\x99].*$/, "", line)
        if (line !~ /^include\// && length(line) > 0) {
            cur_file = line
        } else {
            cur_file = ""
        }
        next
    }

    if (cur_file != "" && match(line, /(Branches executed:|执行的分支[：:]?)[ \t]*([0-9\.]+)%[ \t]*(of|\(共有)?[ \t]*([0-9]+)/, m)) {
        pct = m[2] + 0.0
        tot = m[4] + 0.0
        hit = int(pct * tot / 100.0 + 0.5)
        
        files[file_count] = cur_file
        hits[cur_file] = hit
        totals[cur_file] = tot
        pcts[cur_file] = pct
        file_count++
        
        total_hit += hit
        total_br += tot
        cur_file = ""
    }
}

END {
    # Bubble sort file names
    for (i = 0; i < file_count; i++) {
        for (j = i + 1; j < file_count; j++) {
            if (files[i] > files[j]) {
                tmp = files[i]
                files[i] = files[j]
                files[j] = tmp
            }
        }
    }

    for (i = 0; i < file_count; i++) {
        fn = files[i]
        printf "%-35s | %-10d | %-8d | %6.2f%%\n", fn, hits[fn], totals[fn], pcts[fn]
    }

    printf "---------------------------------------------------------------------------\n"
    overall = (total_br > 0) ? (total_hit / total_br * 100.0) : 0.0
    printf "%-35s | %-10d | %-8d | %6.2f%%\n", "OVERALL C-SOURCE BRANCH COVERAGE", total_hit, total_br, overall
    printf "=====================================================================\n\n"

    if (overall < threshold) {
        printf "FAILED: Overall C-source branch coverage %.2f%% is below threshold %.2f%%\n", overall, threshold
        exit 1
    } else {
        printf "PASSED: Overall C-source branch coverage %.2f%% >= %.2f%%\n", overall, threshold
    }
}
'
