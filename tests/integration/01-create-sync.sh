#!/bin/bash
# Create a single-parity array, build parity, put filesystems on it and check
# the data survives a stop/start cycle and a read-only parity check.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

harness_init
disks_create 3

array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"
assert_nmdstat mdNumDisks 3

array_mount
sum1=$(write_file 1 testfile 100)
sum2=$(write_file 2 testfile 20)

quiet nmd status || fail "status reports an unhealthy array"
nmd status -o json | python3 -m json.tool >/dev/null || fail "status -o json is not valid JSON"
[ -n "$(nmd status -o terse)" ] || fail "status -o terse printed nothing"
[ -n "$(nmd status -o prometheus)" ] || fail "status -o prometheus printed nothing"
summary_status "healthy array"
assert_fails "set label refused while started" quiet nmd set label nonraid-test

# Stop/start cycle, data must come back from disk rather than page cache
array_umount
array_stop

log "set label"
assert_fails "set label rejects an invalid label" quiet nmd set label "not valid!"
quiet nmd set label nonraid-test || fail "nmdctl set label failed"
assert_nmdstat sbLabel nonraid-test

array_start
array_mount
assert_file 1 testfile "$sum1"
assert_file 2 testfile "$sum2"
assert_nmdstat sbLabel nonraid-test

sync_run NOCORRECT
assert_nmdstat sbSyncErrs 0

log "passed"
