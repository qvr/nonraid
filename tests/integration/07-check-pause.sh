#!/bin/bash
# Pause a parity check and check that status reports it as paused, then resume
# it and check it finishes cleanly. Also checks the status of a pending disk
# clear, and json status output without an array.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

# A check of the default sized disks finishes in well under a second, bigger
# (sparse) disks give time to pause it
PAUSE_DISK_MB=4096

# json_get KEY...: print a value from JSON on stdin, e.g. json_get resync paused
json_get() {
    python3 -c 'import json, sys
v = json.load(sys.stdin)
for k in sys.argv[1:]:
    v = v[k]
print(json.dumps(v))' "$@"
}

harness_init
disks_create 3 "$PAUSE_DISK_MB"
disks_create 1 "$PAUSE_DISK_MB"

array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"

log "check NOCORRECT, pause once running"
nmd -u check NOCORRECT >/dev/null || fail "nmdctl check NOCORRECT failed"
waited=0
until [ "$(nmdstat mdResync)" != "0" ]; do
    [ "$waited" -ge 200 ] && fail "check did not start"
    sleep 0.05
    waited=$((waited + 1))
done
quiet nmd -u check PAUSE || fail "nmdctl check PAUSE failed"

assert_nmdstat mdResync 0
pos=$(nmdstat mdResyncPos)
size=$(nmdstat mdResyncSize)
if [ "$pos" -eq 0 ] || [ "$pos" -ge "$size" ]; then
    fail "check not paused mid-way: mdResyncPos=$pos mdResyncSize=$size"
fi
info "ok: paused at $pos / $size"

json=$(nmd status -o json) || true
assert_eq "$(json_get resync active <<< "$json")" true "json resync.active while paused"
assert_eq "$(json_get resync paused <<< "$json")" true "json resync.paused while paused"
out=$(nmd status) || true
[[ "$out" == *"(PAUSED)"* ]] || fail "status does not show the check as paused"
summary_status "paused parity check"

sync_run RESUME
assert_nmdstat sbSyncErrs 0
json=$(nmd status -o json) || true
assert_eq "$(json_get resync active <<< "$json")" false "json resync.active after resume"
assert_eq "$(json_get resync paused <<< "$json")" false "json resync.paused after resume"
quiet nmd status || fail "status reports an unhealthy array after the resumed check"

array_stop
log "add disk 4 to slot 3"
quiet nmd add --force "$(member 3 4)" || fail "nmdctl add failed"
array_start
json=$(nmd status -o json) || true
assert_eq "$(json_get resync pending <<< "$json")" true "json resync.pending with a new disk"
size_gb=$(json_get resync size_gb <<< "$json")
[ "$size_gb" -gt 0 ] || fail "pending clear reported with size_gb=$size_gb"
info "ok: pending clear size_gb = $size_gb"

array_stop
unload_module || fail "could not unload the nonraid module"
log "status -o json without an array"
rc=0
json=$(bash "$NMDCTL" --no-color -s "$WORKDIR/missing.dat" status -o json) || rc=$?
[ "$rc" -ne 0 ] || fail "status -o json without an array exited with 0"
json_get error <<< "$json" >/dev/null || fail "status -o json without an array is not a JSON error: $json"
info "ok: $json"

log "passed"
