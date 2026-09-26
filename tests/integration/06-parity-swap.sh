#!/bin/bash
# Parity swap: a data disk has failed and its replacement is bigger than the
# parity disk. The bigger disk becomes the new parity disk, the old parity disk
# replaces the failed data disk, and the old parity is copied manually onto the
# new parity disk before start. The driver then rebuilds the data disk.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

harness_init
disks_create 3
disks_create 1 $((DISK_MB + 256))  # disk 4: the new, bigger parity disk

array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"
array_mount
sum1=$(write_file 1 testfile 100)
sum2=$(write_file 2 testfile 20)
array_umount
array_stop

# Data disk 1 fails
array_unassign 1
array_start disable_disk
array_mount
assert_file 1 testfile "$sum1"
array_umount
array_stop

# Same as with 05-dual-parity, replace straight after a fresh module load
array_reload
log "replace parity with bigger disk 4"
quiet nmd -u replace -f "$(member P 4)" || fail "nmdctl replace P failed"
assert_nmdstat rdevStatus.0 DISK_WRONG
log "replace slot 1 with the old parity disk 1"
quiet nmd -u replace -f "$(member 1 1)" || fail "nmdctl replace slot 1 failed"
assert_nmdstat mdState SWAP_DSBL

log "copy old parity to new parity disk, zero the rest"
old_bytes=$(blockdev --getsize64 "$(disk_part 1)")
new_bytes=$(blockdev --getsize64 "$(disk_part 4)")
[ "$new_bytes" -gt "$old_bytes" ] || fail "new parity disk is not bigger than the old one"
dd if="$(disk_part 1)" of="$(disk_part 4)" bs=1M oflag=direct status=none \
    || fail "copying parity failed"
dd if=/dev/zero of="$(disk_part 4)" bs=1M oflag=direct,seek_bytes iflag=count_bytes \
    seek="$old_bytes" count=$((new_bytes - old_bytes)) status=none \
    || fail "zeroing the rest of the new parity disk failed"

array_start swap_dsbl
assert_nmdstat rdevStatus.0 DISK_OK
assert_nmdstat diskId.0 "$(disk_id 4)"
sync_run recon
assert_disks_ok 0 1 2
assert_nmdstat diskId.1 "$(disk_id 1)"
summary_status "after parity swap"

array_stop
array_start
array_mount
assert_file 1 testfile "$sum1"
assert_file 2 testfile "$sum2"

# The whole new parity disk, including the zeroed part beyond the old parity
# size, has to be valid parity
sync_run NOCORRECT
assert_nmdstat sbSyncErrs 0

log "passed"
