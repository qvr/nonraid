# NonRAID driver integration tests

End-to-end tests of the NonRAID kernel driver and `nmdctl`, using loop-device
backed disk images. These are run by the
[NonRAID Integration Tests](../../.github/workflows/dkms-integration-tests.yml)
workflow after building the driver with DKMS, and can be run the same way
locally on a machine (or VM) with the driver installed.

```bash
sudo tests/integration/run.sh                # run all tests
sudo tests/integration/run.sh replace 04     # run tests whose name contains "replace" or "04"
sudo tests/integration/03-replace-disk.sh    # run a single test directly
```

> [!WARNING]
> The tests unload and reload the `md-nonraid` module. Don't run them on a
> machine with a real NonRAID array: they refuse to start if the driver has a
> started array with a superblock that isn't theirs, but a stopped array's
> module will be unloaded.

## What the tests do

Each test is a standalone script which, via [lib.sh](lib.sh):

1. creates a private work directory (`/var/tmp/nonraid-test.XXXXXX`) for the
   disk images, superblock and mount points
2. (re)loads `md-nonraid` with the test's own superblock
3. creates sparse disk images, attaches them as partitioned loop devices, and
   symlinks them as `/dev/disk/by-id/nonraid-test-NN` (nmdctl finds array disks
   by ID)
4. drives the array through `nmdctl` and checks the results from
   `/proc/nmdstat` and file checksums
5. tears everything down on exit: stops the array, unloads the module, detaches
   the loop devices and removes the work directory and symlinks

| Test | Covers |
|---|---|
| `01-create-sync` | create, initial parity sync, mkfs/mount, status output formats, label, stop/start, parity check |
| `02-add-disk` | add a new disk, driver clear, use the new disk |
| `03-replace-disk` | unassign, degraded start with emulated disk, replace, rebuild |
| `04-parity-check` | corrupted parity detected by `NOCORRECT` check, fixed by `CORRECT` check |
| `05-dual-parity` | P+Q array, two disks emulated at once, replace and rebuild both |
| `06-parity-swap` | parity swap: bigger disk replaces parity, old parity disk replaces a failed data disk |

## Kernel log check

After the tests, `run.sh` fails the run if the kernel logged warnings or
errors during it (kernel `WARNING`/`BUG`/traces, driver `nmd:` I/O and
superblock errors, XFS corruption on `nmd*`), or if new warning/oops/lockup
taint flags were set. Each test is bracketed with `nonraid-test: begin/end`
markers in the kernel log, so findings are reported per test. The patterns are
`KMSG_PATTERNS` in [run.sh](run.sh).

## Settings

| Variable | Default | |
|---|---|---|
| `NONRAID_TEST_KEEP` | `0` | `1` leaves the failed test's array, loop devices and work directory in place for inspection |
| `NONRAID_TEST_DISK_MB` | `512` | Size of each test disk (xfs needs at least 300MB) |
| `NONRAID_TEST_SYNC_TIMEOUT` | `120` | Seconds to wait for a parity sync/check/clear/rebuild |
| `NONRAID_TEST_TMPDIR` | `/var/tmp` | Where the work directories are created |
| `NONRAID_TEST_KERNEL_CHECK` | `1` | `0` skips the kernel log and taint check in `run.sh` |
| `NMDCTL` | `tools/nmdctl` | nmdctl to test |

`sudo` drops environment variables by default, so pass them with
`sudo env NONRAID_TEST_KEEP=1 tests/integration/run.sh`.

After a kept failure, clean up with `sudo nmdctl -s <workdir>/nonraid.dat stop`,
`sudo modprobe -r md_nonraid`, `sudo losetup -D` (detaches *all* loop devices)
and removing the work directory and the `/dev/disk/by-id/nonraid-test-*` links.

## Writing a test

Add a `NN-name.sh` script (the runner picks up `[0-9][0-9]-*.sh`):

```bash
#!/bin/bash
# What this test covers.
# shellcheck source=tests/integration/lib.sh
. "$(dirname "$0")/lib.sh"

harness_init          # work directory, module load, teardown trap
disks_create 3        # test disks 1..3 (`disks_create 1 1024` adds a 1GB disk 4)

array_new_synced "$(member P 1)" "$(member 1 2)" "$(member 2 3)"
array_mount
sum=$(write_file 1 testfile 10)
...
assert_file 1 testfile "$sum"
log "passed"
```

`member SLOT N` builds an nmdctl `SLOT:DEVICE:ID` argument for test disk `N`.
Use `nmd` to run nmdctl against the test's superblock, `nmdstat KEY` to read
`/proc/nmdstat`, `sync_run OPTION` to run `nmdctl check OPTION` and wait for it
to finish, and `fail` / `assert_*` for checks — see [lib.sh](lib.sh).
Tests are run with `set -euo pipefail`.
