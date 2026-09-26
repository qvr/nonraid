#!/bin/bash
# Shared helpers for the NonRAID driver integration tests.
#
# Sourced by every test script. Each test gets a private work directory
# (loop-backed disk images, superblock, mount points), loads the driver with
# that superblock, and tears everything down again on exit. See README.md.
#
# shellcheck disable=SC2034  # some variables are only used by the test scripts

set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TEST_DIR/../.." && pwd)"
TEST_NAME="$(basename "$0" .sh)"

NMDCTL="${NMDCTL:-$REPO_ROOT/tools/nmdctl}"
DISK_MB="${NONRAID_TEST_DISK_MB:-512}"       # xfs needs >= 300MB
SYNC_TIMEOUT="${NONRAID_TEST_SYNC_TIMEOUT:-120}"
KEEP_ON_FAIL="${NONRAID_TEST_KEEP:-0}"
TMP_BASE="${NONRAID_TEST_TMPDIR:-/var/tmp}"

# by-id symlink prefix. nmdctl matches disk IDs as a substring of by-id names,
# so the numeric suffix is zero-padded to keep e.g. -1 from matching -10.
BYID_PREFIX="nonraid-test-"

WORKDIR=""
SUPERBLOCK=""
MOUNT_PREFIX=""
declare -a DISKS=()      # DISKS[n] = loop device of test disk n (1-based)

# --- output ---------------------------------------------------------------

log()  { echo "==> [$TEST_NAME] $*"; }
info() { echo "    $*"; }

fail() {
    echo "FAIL: [$TEST_NAME] $*" >&2
    dump_state >&2
    exit 1
}

dump_state() {
    echo "--- /proc/nmdstat (summary) ---"
    if [ -r /proc/nmdstat ]; then
        grep -E '^(sb|md)[A-Z]|^(diskId|diskState|rdevName|rdevStatus)\.' /proc/nmdstat || true
    else
        echo "(not available)"
    fi
    echo "--- dmesg (last 30 lines) ---"
    dmesg 2>/dev/null | tail -n 30 || true
}

# Append markdown to the GitHub Actions job summary, if running in Actions.
summary() {
    [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
    echo "$*" >> "$GITHUB_STEP_SUMMARY"
}

# Record `nmdctl status` output in the job summary under a heading.
summary_status() {
    local title="$1" out
    out=$(nmd status 2>&1) || true
    echo "$out"
    summary "#### $TEST_NAME: $title"
    summary '```'
    summary "$out"
    summary '```'
}

# --- driver access --------------------------------------------------------

# Run a command, only showing its output if it fails
quiet() {
    local out rc=0
    out=$("$@" 2>&1) || rc=$?
    [ "$rc" -eq 0 ] || echo "$out"
    return "$rc"
}

# nmdctl, always against this test's superblock
nmd() {
    bash "$NMDCTL" --no-color -s "$SUPERBLOCK" "$@"
}

# Print a single /proc/nmdstat value
nmdstat() {
    awk -v k="$1" 'index($0, k "=") == 1 { print substr($0, length(k) + 2); exit }' /proc/nmdstat
}

module_loaded() {
    grep -q '^md_nonraid ' /proc/modules
}

load_module() {
    modprobe md-nonraid super="$SUPERBLOCK" || fail "modprobe md-nonraid failed"
    [ -r /proc/nmdstat ] || fail "/proc/nmdstat missing after module load"
    [ "$(nmdstat sbName)" = "$SUPERBLOCK" ] || fail "driver loaded with unexpected superblock '$(nmdstat sbName)'"
}

unload_module() {
    if module_loaded; then
        modprobe -r md_nonraid || return 1
    fi
    modprobe -r nonraid6_pq 2>/dev/null || true
}

# The driver is a singleton: the tests have to unload/reload it, so refuse to
# run if it is currently driving somebody else's started array.
check_not_foreign_array() {
    module_loaded || return 0
    [ -r /proc/nmdstat ] || return 0
    local sb state
    sb=$(nmdstat sbName)
    state=$(nmdstat mdState)
    case "$sb" in
        "$TMP_BASE"/nonraid-test.*) return 0 ;;
    esac
    if [ "$state" = "STARTED" ]; then
        echo "Refusing to run: nonraid driver has a STARTED array using superblock '$sb'." >&2
        echo "These tests unload and reload the driver; stop that array first." >&2
        exit 1
    fi
}

# --- setup / teardown -----------------------------------------------------

require_cmds() {
    local c missing=0
    for c in "$@"; do
        command -v "$c" >/dev/null 2>&1 || { echo "Missing required command: $c" >&2; missing=1; }
    done
    [ "$missing" -eq 0 ] || exit 1
}

# Must be called first by every test.
harness_init() {
    [ "$EUID" -eq 0 ] || { echo "Please run as root" >&2; exit 1; }
    require_cmds losetup sgdisk truncate udevadm blockdev mkfs.xfs md5sum modprobe
    [ -f "$NMDCTL" ] || { echo "nmdctl not found at $NMDCTL" >&2; exit 1; }

    check_not_foreign_array

    # Never block on an nmdctl confirmation prompt
    exec < /dev/null

    WORKDIR=$(mktemp -d "$TMP_BASE/nonraid-test.XXXXXX")
    SUPERBLOCK="$WORKDIR/nonraid.dat"
    MOUNT_PREFIX="$WORKDIR/mnt/disk"
    trap harness_teardown EXIT

    log "workdir: $WORKDIR"
    unload_module || fail "could not unload the already loaded nonraid module"
    load_module
}

harness_teardown() {
    local rc=$?
    set +e
    trap - EXIT

    if [ "$rc" -ne 0 ] && [ "$KEEP_ON_FAIL" = "1" ]; then
        echo "NONRAID_TEST_KEEP=1: leaving $WORKDIR, loop devices and driver state in place" >&2
        exit "$rc"
    fi

    log "teardown"
    if [ -r /proc/nmdstat ] && [ "$(nmdstat sbName)" = "$SUPERBLOCK" ]; then
        if [ "$(nmdstat mdState)" = "STARTED" ]; then
            nmd -u check CANCEL >/dev/null 2>&1
            nmd -u umount >/dev/null 2>&1
            nmd -u stop >/dev/null 2>&1
        fi
        unload_module || echo "warning: could not unload nonraid module" >&2
    fi

    # Anything still mounted below the workdir (e.g. after a failed umount)
    if [ -n "$WORKDIR" ]; then
        findmnt -rn -o TARGET | grep -F "$WORKDIR/" | sort -r | while read -r m; do
            umount "$m"
        done
    fi

    local n
    for n in "${!DISKS[@]}"; do
        rm -f "/dev/disk/by-id/$(disk_id "$n")"
        losetup -d "${DISKS[$n]}" 2>/dev/null
    done

    [ -n "$WORKDIR" ] && rm -rf "$WORKDIR"
    exit "$rc"
}

# --- test disks -----------------------------------------------------------

disk_id()   { printf '%s%02d' "$BYID_PREFIX" "$1"; }
disk_part() { echo "${DISKS[$1]}p1"; }

# disks_create COUNT [MB]: create COUNT more partitioned loop disks, numbered
# on from the existing ones (1..COUNT on the first call), DISK_MB in size by
# default
disks_create() {
    local count="$1" mb="${2:-$DISK_MB}" first n img loop
    first=$((${#DISKS[@]} + 1))
    for n in $(seq "$first" $((first + count - 1))); do
        img="$WORKDIR/d$n.img"
        truncate -s "${mb}M" "$img"
        loop=$(losetup -fP --show "$img")
        DISKS[n]="$loop"
        sgdisk -o -a 8 -n 1:32K:0 "$loop" >/dev/null
        ln -sf "$loop" "/dev/disk/by-id/$(disk_id "$n")"
    done
    udevadm settle
    for n in $(seq "$first" $((first + count - 1))); do
        [ -b "$(disk_part "$n")" ] || fail "partition $(disk_part "$n") did not appear"
        info "disk $n: ${DISKS[n]} ($(disk_id "$n"), ${mb}MB)"
    done
}

# member SLOT N: nmdctl SLOT:DEV:ID layout argument for test disk N
member() {
    echo "$1:$(disk_part "$2"):$(disk_id "$2")"
}

# --- assertions -----------------------------------------------------------

assert_eq() {
    local actual="$1" expected="$2" what="$3"
    [ "$actual" = "$expected" ] || fail "$what: expected '$expected', got '$actual'"
    info "ok: $what = $actual"
}

assert_nmdstat() {
    assert_eq "$(nmdstat "$1")" "$2" "$1"
}

# assert_disks_ok SLOT...: every listed slot has rdevStatus DISK_OK
assert_disks_ok() {
    local s
    for s in "$@"; do
        assert_nmdstat "rdevStatus.$s" DISK_OK
    done
}

# Expect a command to fail
assert_fails() {
    local what="$1"; shift
    if "$@"; then
        fail "$what: command unexpectedly succeeded: $*"
    fi
    info "ok: $what (failed as expected)"
}

# --- array operations -----------------------------------------------------

# Run `nmdctl -u check OPTION` and wait until the resulting resync operation
# has finished, then check it exited cleanly.
#
# `check` only wakes the driver's recovery thread, so mdResync can still read 0
# right after the command returns. The recovery thread updates the superblock
# (bumping sbEvents) both when it starts and when it finishes, so wait for two
# events before trusting mdResync=0.
sync_run() {
    local option="$1" events waited=0
    events=$(nmdstat sbEvents)
    log "check $option"
    nmd -u check "$option" || fail "nmdctl check $option failed"
    while :; do
        if [ "$(nmdstat sbEvents)" -ge $((events + 2)) ] && [ "$(nmdstat mdResync)" = "0" ]; then
            break
        fi
        [ "$waited" -ge "$SYNC_TIMEOUT" ] && fail "timeout after ${SYNC_TIMEOUT}s waiting for check $option"
        sleep 1
        waited=$((waited + 1))
    done
    info "sync finished in ~${waited}s: action=$(nmdstat mdResyncAction) errs=$(nmdstat sbSyncErrs)"
    assert_nmdstat sbSyncExit 0
}

array_start() {
    log "start ${1:-}"
    quiet nmd -u start "$@" || fail "nmdctl start ${1:-} failed"
    assert_nmdstat mdState STARTED
}

array_stop() {
    log "stop"
    quiet nmd -u stop || fail "nmdctl stop failed"
    assert_nmdstat mdState STOPPED
}

array_reload() {
    log "reload"
    quiet nmd reload || fail "nmdctl reload failed"
}

# array_unassign SLOT: unassign a disk (array must be stopped)
array_unassign() {
    local status
    log "unassign slot $1"
    # unassign ends by printing status, which exits non-zero for the now
    # degraded array, so check the result from the driver instead
    nmd unassign "$1" <<< "y" >/dev/null || true
    status=$(nmdstat "rdevStatus.$1")
    case "$status" in
        DISK_NP_*) info "ok: rdevStatus.$1 = $status" ;;
        *) fail "slot $1 not unassigned: rdevStatus.$1=$status" ;;
    esac
}

array_mount() {
    log "mount"
    nmd -u mount "$MOUNT_PREFIX" || fail "nmdctl mount failed"
}

array_umount() {
    log "umount"
    nmd -u umount || fail "nmdctl umount failed"
}

# mkfs_data SLOT...: format data slots with xfs
mkfs_data() {
    local s
    for s in "$@"; do
        log "mkfs.xfs /dev/nmd${s}p1"
        mkfs.xfs -q -f "/dev/nmd${s}p1" || fail "mkfs.xfs /dev/nmd${s}p1 failed"
    done
}

mountpoint_of() { echo "${MOUNT_PREFIX}$1"; }

# write_file SLOT NAME MB: write random data to a mounted data disk, prints md5
write_file() {
    local f
    f="$(mountpoint_of "$1")/$2"
    dd if=/dev/urandom of="$f" bs=1M count="$3" status=none conv=fsync || fail "writing $f failed"
    md5sum < "$f" | cut -d' ' -f1
}

# assert_file SLOT NAME MD5
assert_file() {
    local f sum
    f="$(mountpoint_of "$1")/$2"
    [ -f "$f" ] || fail "$f missing"
    sum=$(md5sum < "$f" | cut -d' ' -f1)
    assert_eq "$sum" "$3" "md5 of disk$1/$2"
}

# array_new_synced MEMBER...: create an array from `member` layout arguments,
# start it, build parity and format the data slots with xfs. Leaves the array
# started and unmounted.
#   array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"
array_new_synced() {
    local -a slots=() data_slots=()
    local m s
    for m in "$@"; do
        s="${m%%:*}"
        case "$s" in
            P) slots+=(0) ;;
            Q) slots+=(29) ;;
            *) slots+=("$s"); data_slots+=("$s") ;;
        esac
    done

    log "create $*"
    quiet nmd create --force "$@" || fail "nmdctl create failed"
    assert_nmdstat mdState NEW_ARRAY
    array_start new_array
    sync_run recon
    assert_nmdstat sbSyncErrs 0

    # The driver keeps stale mdNum* counters after the initial sync; restart
    # with a fresh module to get a clean state (same as nmdctl stop without -u).
    array_reload
    array_start
    assert_disks_ok "${slots[@]}"

    mkfs_data "${data_slots[@]}"
}
