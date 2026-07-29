#!/usr/bin/env python3
"""
Calculate C source-level branch coverage using gcov -b.
Parses gcov output for core library .gcda files, filters out system/include headers,
prints a clean summary table, and enforces a minimum branch coverage threshold.
"""
import glob
import os
import re
import subprocess
import sys

def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else 'build'
    threshold = float(sys.argv[2]) if len(sys.argv) > 2 else 75.0

    gcda_files = glob.glob(os.path.join(build_dir, '*.gcda'))
    core_files = [
        f for f in gcda_files
        if not os.path.basename(f).startswith('test_') and not os.path.basename(f).startswith('verify_')
    ]

    if not core_files:
        print(f"No core .gcda files found in {build_dir}")
        sys.exit(1)

    cmd = ['gcov', '-b'] + core_files
    res = subprocess.run(cmd, capture_output=True, text=True, cwd=os.getcwd())
    output = res.stdout + '\n' + res.stderr

    file_stats = []
    current_file = None

    for line in output.splitlines():
        line = line.strip()
        file_match = re.search(r"File\s*['‘]([^'’]+)['’]", line)
        if file_match:
            filepath = file_match.group(1)
            if not filepath.startswith('include/'):
                current_file = filepath
            else:
                current_file = None
            continue

        if current_file:
            br_match = re.search(
                r'(?:Branches executed:|执行的分支[：:]?)\s*([\d\.]+)%\s*(?:of|\(共有)?\s*(\d+)',
                line
            )
            if br_match:
                pct = float(br_match.group(1))
                total = int(br_match.group(2))
                hit = int(round(pct * total / 100.0))
                file_stats.append((current_file, hit, total, pct))
                current_file = None

    if not file_stats:
        print("No C source branch statistics parsed from gcov.")
        sys.exit(1)

    print("\n=================== GCOV C-SOURCE BRANCH COVERAGE ===================")
    print(f"{'Source File':<35} | {'Executed':<10} | {'Total':<8} | {'Coverage':<10}")
    print("-" * 75)

    total_hit = 0
    total_branches = 0
    for fname, hit, total, pct in sorted(file_stats, key=lambda x: x[0]):
        total_hit += hit
        total_branches += total
        print(f"{fname:<35} | {hit:<10} | {total:<8} | {pct:>6.2f}%")

    print("-" * 75)
    overall_pct = (total_hit / total_branches * 100.0) if total_branches > 0 else 0.0
    print(f"{'OVERALL C-SOURCE BRANCH COVERAGE':<35} | {total_hit:<10} | {total_branches:<8} | {overall_pct:>6.2f}%")
    print("=====================================================================\n")

    if overall_pct < threshold:
        print(f"FAILED: Overall C-source branch coverage {overall_pct:.2f}% is below threshold {threshold:.2f}%")
        sys.exit(1)
    else:
        print(f"PASSED: Overall C-source branch coverage {overall_pct:.2f}% >= {threshold:.2f}%")

if __name__ == '__main__':
    main()
