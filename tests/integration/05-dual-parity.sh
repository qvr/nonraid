#!/bin/bash
# Dual parity (P+Q): lose two data disks at once, run with both emulated, then
# replace both and rebuild them.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

harness_init
disks_create 7

array_new_synced "$(member P 1)" "$(member Q 2)" "$(member 1 3)" "$(member 2 4)" "$(member 3 5)"
array_mount
sum1=$(write_file 1 testfile 60)
sum2=$(write_file 2 testfile 60)
sum3=$(write_file 3 testfile 60)
array_umount
array_stop

array_unassign 1
array_unassign 2

array_start disable_disk
array_mount
assert_file 1 testfile "$sum1"
assert_file 2 testfile "$sum2"
assert_file 3 testfile "$sum3"
summary_status "slots 1 and 2 emulated from P+Q"
array_umount
array_stop

# Replace straight after a fresh module load, without `nmdctl import` first:
# the driver does not reset its per-slot counters when a slot is imported
# again, so importing the two empty slots and then replacing them counts four
# invalid disks (ERROR:TOO_MANY_MISSING_DISKS). start imports the rest.
array_reload
log "replace slots 1 and 2 with disks 6 and 7"
quiet nmd -u replace -f "$(member 1 6)" || fail "nmdctl replace slot 1 failed"
quiet nmd -u replace -f "$(member 2 7)" || fail "nmdctl replace slot 2 failed"

array_start recon_disk
sync_run recon
assert_disks_ok 0 29 1 2 3

array_stop
array_start
array_mount
assert_file 1 testfile "$sum1"
assert_file 2 testfile "$sum2"
assert_file 3 testfile "$sum3"

sync_run NOCORRECT
assert_nmdstat sbSyncErrs 0

log "passed"
