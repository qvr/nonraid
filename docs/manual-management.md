# Manual Array Management (Driver Interface Reference)

This document explains how to operate the NonRAID array driver directly without the `nmdctl` management tool. This is useful for:
- Troubleshooting unusual states
- Developing / testing new driver behavior
- Understanding what `nmdctl` actually performs under the hood

The driver itself provides no automation whatsoever, and for example array member disks need to be imported manually every time the driver is loaded, in the correct slots and with correct parameters.

> [!WARNING]
> Manual operation is error‑prone. A wrong command can invalidate parity or put the array into an inconsistent state. Always have backups.

## Superblock file
Array state is kept in a superblock file, that is stored outside the array and read by the driver when the driver module is loaded.

> [!IMPORTANT]
> Superblock filename must be given as kernel module parameter: `modprobe nonraid super=/nonraid.dat`

Superblock file contains the array configuration, including which disk id is assigned to which slot, and what their state was at the time of last superblock update.

If the superblock file is lost, the array can be recreated with the original import parameters (and you are even able to skip parity reconstruction if you are sure there has been no data changes), but superblock file is needed if you have had a disk failure and need to start the array with a missing disk.

Guessing from the official docs, the "New Config" operation in the UnRAID system is probably basically reloading the driver with a new, empty superblock file, allowing you to start again in the NEW_ARRAY state and import the existing disks in whatever order and configuration you want.
 - Manually this is done by reloading the driver with a different superblock parameter: `modprobe -r nonraid && modprobe nonraid super=/new_nonraid.dat` (or moving the old superblock file out of the way before reloading the driver)
 - By default NEW_ARRAY always marks parity disks invalid forcing parity reconstruction, so the "Parity is valid" UI option in "New config" basically does  `echo "set invalidslot 98" > /proc/nmdcmd` before starting the array, which causes the driver to not consider parity disks as invalid when creating an array, and thus skipping needing reconstruction.
   - It should go without saying, but this is a very error-prone operation when done completely manually without an UI double checking everything, so be careful!

## nmdcmd
The driver is managed via procfs interface `/proc/nmdcmd`, see [docs/nmdcmd.8](nmdcmd.8) for "man page". The documentation is LLM generated from the driver source code, but seems to be mostly correct.

> [!TIP]
> You need to write commands to the `/proc/nmdcmd` file as root, so either do it from root shell or use `sudo sh -c 'echo "command" > /proc/nmdcmd'` (or `echo "command"|sudo tee /proc/nmdcmd`)
>
> Driver by default logs commands to dmesg, to see more output from the commands you can run: `echo "set md_trace 2" > /proc/nmdcmd`

## nmdstat
The driver provides an procfs status interface file: `/proc/nmdstat`, which is used to check the current state of the array and its members. See [docs/nmdstat.5](nmdstat.5) for LLM generated "man page".

It is a text file that lists the status of each nmd device, including the disks assigned to it, their sizes, and their states. If you are using this with any data you care about, you should have monitoring for the different array and disk states in this file.

## Creating a new array
1. Import disks (parity is not required, but as a whole example here).
The disks must be partitioned (for example `sgdisk -o -a 8 -n 1:32K:0 /dev/sdX`), and the size needs to be the number of sectors divisible by 8, and then presented in 1k blocks. (Usually this means dividing the number of sectors by 2, as a sector is usually 512 bytes.)
Check docs/nmdcmd.8 for details on the params.
```
echo "import 0 sdd1 0 10000000 0 VBOX_HARDDISK_VBb584354a-302d68db" > /proc/nmdcmd
echo "import 1 sdb1 0 10000000 0 VBOX_HARDDISK_VB4f18929e-8de91109" > /proc/nmdcmd
echo "import 2 sdc1 0 10000000 0 VBOX_HARDDISK_VB9c8a60bc-22209161" > /proc/nmdcmd
```
The driver does not care about on which sector partitions start, as long as they are valid and the size is correct.

2. Check that all devices are correct in `/proc/nmdstat`, and mdState is `NEW_ARRAY`, and then start the array:
```
echo "start NEW_ARRAY" > /proc/nmdcmd
```

3. The nmd devices should now be available for creating FS and then mounting
```
/dev/nmd1p1 - slot 1
/dev/nmd2p1 - slot 2
```
The driver does not actually care about what filesystem is used, but you should probably stick with the UnRAID supported filesystems: XFS, BTRFS or ZFS.

4. mdState should now be STARTED, but the parity disk is not yet constructed, so it has an `rdevStatus` of `DISK_INVALID`. Trigger a parity reconstruction:
```
echo "check" > /proc/nmdcmd
```

5. Check that the driver was able to create the superblock file (`sbName` in the `mdstat` output). If the superblock file does not exist, or sbName is empty, then the array will be recreated on the next boot and parity needs to be reconstructed.

### Starting existing array after boot
1. Load the driver with the correct superblock file:
```
modprobe nonraid super=/nonraid.dat
```
Once the driver is loaded, `/proc/nmdstat` can be used to check which drive ID should go to which slot. (This could be used to automate the imports.)

2. Import all array member disks, with same import parameters as when creating the array. (Some of the parameters are only used for the initial creation, so do not really matter when importing existing arrays.)
```
echo "import 0 sdd1 0 10000000 0 VBOX_HARDDISK_VBb584354a-302d68db" > /proc/nmdcmd
echo "import 1 sdb1 0 10000000 0 VBOX_HARDDISK_VB4f18929e-8de91109" > /proc/nmdcmd
echo "import 2 sdc1 0 10000000 0 VBOX_HARDDISK_VB9c8a60bc-22209161" > /proc/nmdcmd
```

3. Check that all devices are correct in `/proc/nmdstat`, and then start the array: `echo "start" > /proc/nmdcmd`
   - If you missed importing any member, a custom patch to the driver will prevent the array from starting, as without it the kernel would have crashed

> [!TIP]
> If you want to start the array with a missing disk, the slot must still be imported with empty device name and other parameters (unassigning a disk):
>  ```
> echo "import 2 '' 0 0 0 ''" > /proc/nmdcmd
> ```
> and then array needs to started with the DISABLE_DISK option to run in degraded mode (missing disk simulated from parity and other data disks)

## Driver bugs / "features"
As the driver is not intended to be used manually, normally the UnRAID UI makes sure it doesn't do things which cause driver issues, but manually these are easy to run into:
* Unassigning same slot twice increases disk missing counter twice, causing the array to enter TOO_MANY_MISSING_DISKS state - this requires driver reload to reset: `modprobe -r nonraid && modprobe nonraid super=/nonraid.dat` or `nmdctl reload`
* Same goes for many other internal state counters - best practise seems to be to always reload the driver after a single array operation

`nmdctl` tries to handle these driver issues either automatically or by warning the user to reload the driver if an inconsistent state is detected.
