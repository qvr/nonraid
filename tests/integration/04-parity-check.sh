#!/bin/bash
# Corrupt the parity disk behind the driver's back and check that a read-only
# parity check detects it, a correcting check fixes it, and a final check
# comes back clean.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

harness_init
disks_create 3

array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"
array_mount
sum1=$(write_file 1 testfile 100)
array_umount
array_stop

log "corrupt 4MB of parity"
dd if=/dev/urandom of="$(disk_part 1)" bs=1M seek=64 count=4 oflag=direct conv=notrunc status=none \
    || fail "writing to parity partition failed"

array_start

sync_run NOCORRECT
errs=$(nmdstat sbSyncErrs)
[ "$errs" -gt 0 ] || fail "NOCORRECT check found no sync errors in corrupted parity"
info "ok: NOCORRECT check found $errs sync errors"
summary_status "after NOCORRECT check of corrupted parity"
assert_fails "status reports unhealthy array with uncorrected sync errors" quiet nmd status

sync_run CORRECT
errs=$(nmdstat sbSyncErrs)
[ "$errs" -gt 0 ] || fail "CORRECT check reported no corrected sync errors"
info "ok: CORRECT check corrected $errs sync errors"
quiet nmd status || fail "status reports an unhealthy array after a CORRECT check fixed the sync errors"

sync_run NOCORRECT
assert_nmdstat sbSyncErrs 0
quiet nmd status || fail "status still reports an unhealthy array after a clean check"

array_mount
assert_file 1 testfile "$sum1"

log "passed"
