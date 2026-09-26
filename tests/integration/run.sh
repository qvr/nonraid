#!/bin/bash
# Run NonRAID driver integration tests.
#
#   sudo tests/integration/run.sh              # all tests
#   sudo tests/integration/run.sh 03 parity    # tests whose name matches
#
# Each test runs as its own process with its own disks and superblock, see
# README.md. After the tests, the kernel log and taint flags are checked for
# warnings, oopses and driver I/O errors logged during the run. Exits non-zero
# if any test or the kernel log check failed.
set -uo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_CHECK="${NONRAID_TEST_KERNEL_CHECK:-1}"

# Kernel log lines that mean something went wrong, whichever test caused them
KMSG_PATTERNS=(
    # generic kernel problems
    'WARNING:' 'BUG:' 'kernel BUG' 'Oops' 'Call Trace:' 'general protection fault'
    'UBSAN:' 'KASAN:' 'blocked for more than' 'soft lockup' 'hard LOCKUP'
    'possible circular locking' 'possible recursive locking'
    # driver errors (md_unraid.c / unraid.c)
    'nmd: disk[0-9]+ (read|write) error' 'lock_bdev error' 'nmd: invalid superblock'
    'nmd: nonraid_run: failed' 'nonraid: failed to run' "nmd: bug:"
    'read_file: (read error|error closing)' 'write_file: '
    # errors on the array devices or their loop backing devices
    'I/O error, dev (nmd|loop)' 'XFS \(nmd[^)]*\): .*([Cc]orrupt|error)'
)

# Taint flags that mean something went wrong: M(4) machine check, B(5) bad
# page, D(7) oops/BUG, W(9) warning, L(14) soft lockup. O(12) and E(13) are
# set by loading the out-of-tree, unsigned driver itself.
TAINT_MASK=$(( (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 14) ))

tests=()
for t in "$TEST_DIR"/[0-9][0-9]-*.sh; do
    name="$(basename "$t" .sh)"
    if [ $# -gt 0 ]; then
        match=0
        for pattern in "$@"; do
            [[ "$name" == *"$pattern"* ]] && match=1
        done
        [ "$match" -eq 1 ] || continue
    fi
    tests+=("$t")
done

if [ ${#tests[@]} -eq 0 ]; then
    echo "No tests matched: $*" >&2
    exit 1
fi

in_gha() { [ -n "${GITHUB_ACTIONS:-}" ]; }

summary() {
    [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
    echo "$*" >> "$GITHUB_STEP_SUMMARY"
}

# Write a marker into the kernel log, so the check below only looks at this
# run and can tell which test a message came from
kmsg_mark() {
    [ "$KERNEL_CHECK" = "1" ] && echo "nonraid-test: $*" > /dev/kmsg
}

if [ "$KERNEL_CHECK" = "1" ] && ! { [ -w /dev/kmsg ] && dmesg >/dev/null 2>&1; }; then
    echo "Cannot write /dev/kmsg or read dmesg (not root?), skipping kernel log check" >&2
    KERNEL_CHECK=0
fi

run_id="$$-$(date +%s)"
taint_before=$(cat /proc/sys/kernel/tainted)
kmsg_mark "run $run_id"

summary "## NonRAID integration tests"
summary ""

declare -a results=()
failed=0
for t in "${tests[@]}"; do
    name="$(basename "$t" .sh)"
    in_gha && echo "::group::$name"
    echo "=== RUN $name"
    kmsg_mark "begin $name"
    start=$SECONDS
    bash "$t"
    rc=$?
    elapsed=$((SECONDS - start))
    kmsg_mark "end $name"
    in_gha && echo "::endgroup::"
    if [ "$rc" -eq 0 ]; then
        echo "=== PASS $name (${elapsed}s)"
        results+=("| ✅ | $name | ${elapsed}s |")
    else
        echo "=== FAIL $name (${elapsed}s, exit $rc)"
        in_gha && echo "::error title=Integration test failed::$name (exit $rc)"
        results+=("| ❌ | $name | ${elapsed}s |")
        failed=$((failed + 1))
    fi
done

summary ""
summary "| | Test | Time |"
summary "|---|---|---|"
for r in "${results[@]}"; do
    summary "$r"
done

echo
printf '%s\n' "${results[@]}" | sed 's/^| //; s/ |$//; s/ | /  /g'
echo
echo "${#tests[@]} tests, $failed failed"

kernel_failed=0
if [ "$KERNEL_CHECK" = "1" ]; then
    echo
    echo "=== Kernel log check"
    pattern=$(IFS='|'; echo "${KMSG_PATTERNS[*]}")
    # Lines logged after this run's marker, prefixed with the test running then
    # (pattern via ENVIRON: awk -v would process the backslash escapes)
    findings=$(dmesg | RE="$pattern" awk -v run="nonraid-test: run $run_id" '
        index($0, run) { seen = 1; test = "run.sh"; next }
        !seen { next }
        /nonraid-test: begin / { test = $NF; next }
        /nonraid-test: end / { test = "run.sh"; next }
        $0 ~ ENVIRON["RE"] { print test ": " $0 }')

    taint_after=$(cat /proc/sys/kernel/tainted)
    new_taint=$(( taint_after & ~taint_before & TAINT_MASK ))

    if [ -n "$findings" ]; then
        kernel_failed=1
        echo "Kernel log has warnings or errors from this run:"
        echo "$findings"
        in_gha && echo "::error title=Kernel log::$(echo "$findings" | wc -l) warning/error lines in the kernel log, see the run.sh output"
        summary ""
        summary "### ❌ Kernel log warnings/errors"
        summary '```'
        summary "$findings"
        summary '```'
    fi
    if [ "$new_taint" -ne 0 ]; then
        kernel_failed=1
        echo "Kernel got tainted during the run: tainted $taint_before -> $taint_after (new problem bits: $new_taint)"
        in_gha && echo "::error title=Kernel taint::kernel tainted during the run ($taint_before -> $taint_after)"
        summary ""
        summary "### ❌ Kernel tainted during the run: $taint_before → $taint_after"
    fi
    [ "$kernel_failed" -eq 0 ] && echo "No kernel warnings or errors"
fi

[ "$failed" -eq 0 ] && [ "$kernel_failed" -eq 0 ]
