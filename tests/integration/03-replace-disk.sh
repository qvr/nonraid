#!/bin/bash
# Unassign a data disk, run with its contents emulated from parity, then
# replace it and rebuild the contents onto the replacement disk.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

harness_init
disks_create 4

array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"
array_mount
sum1=$(write_file 1 testfile 100)
sum2=$(write_file 2 testfile 20)
array_umount
array_stop

array_unassign 1

array_start disable_disk
assert_fails "status reports degraded array" nmd status
array_mount
assert_file 1 testfile "$sum1"
assert_file 2 testfile "$sum2"
summary_status "slot 1 emulated from parity"
array_umount
array_stop

array_reload
log "import"
quiet nmd import || fail "nmdctl import failed"
log "replace slot 1 with disk 4"
quiet nmd -u replace -f "$(member 1 4)" || fail "nmdctl replace failed"

array_start recon_disk
sync_run recon
assert_disks_ok 0 1 2

array_mount
assert_file 1 testfile "$sum1"
assert_file 2 testfile "$sum2"
summary_status "slot 1 rebuilt onto disk 4"

# Make sure the rebuilt disk is really read back from disk
array_umount
array_stop
array_start
array_mount
assert_file 1 testfile "$sum1"

sync_run NOCORRECT
assert_nmdstat sbSyncErrs 0

log "passed"
