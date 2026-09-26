#!/bin/bash
# Add a new data disk to an existing array: the driver clears it on start
# (without touching parity), after which it can be formatted and used.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

harness_init
disks_create 4

array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"
array_mount
sum1=$(write_file 1 testfile 100)
array_umount
array_stop

log "add disk 4 to slot 3"
quiet nmd add --force "$(member 3 4)" || fail "nmdctl add failed"
array_start
sync_run clear
assert_disks_ok 0 1 2 3

mkfs_data 3
array_mount
sum3=$(write_file 3 testfile 50)
summary_status "after adding disk 3"

array_umount
array_stop
array_start
array_mount
assert_file 1 testfile "$sum1"
assert_file 3 testfile "$sum3"

sync_run NOCORRECT
assert_nmdstat sbSyncErrs 0

log "passed"
