# NonRAID - unRAID storage array compatible kernel driver

NonRAID is an fork of the unRAID system's open-source `md_unraid` kernel driver for supported kernels, but targeting primarily Ubuntu 24.04 LTS, enabling UnRAID-style storage arrays with parity protection outside of the commercial UnRAID system.

Unlike in UnRAID, where the driver replaces kernels standard `md` driver, the NonRAID driver has been separated into it's own kernel module (`md_nonraid`). This allows it to be easily added as a DKMS module on Ubuntu and Debian based systems, without needing to patch the kernel or replace the standard `md` driver. We do however replace the standard `raid6_pq` module with our patched version, as the upstream driver depends on those patches for the parity calculations.

While this is a fork, we try to keep the changes to driver minimal to make syncs with upstream easier. The driver currently has patches to rebrand and separate the module from `md`, and a single patch to prevent kernel crashes if starting the array without importing all disks first.

> [!WARNING]
> :radioactive: This is an experimental project, the driver has not really been tested outside of virtualized dev environment and data loss is a possibility.
> This is intended for DIY enthusiasts who want to try out the disk array tech without installing UnRAID.
>
> Use at your own risk, and always have backups!

## Installation
This has been tested on Ubuntu 24.04 LTS, but the DKMS driver should work on any kernel version between 6.6 - 6.8.

For Ubuntu/Debian based systems, download the latest kernel module dkms package from releases, and install it:
```
sudo dpkg -i nonraid-dkms_*.deb
sudo update-initramfs -u -k all
```
> [!NOTE]
> Updating the initramfs is needed to make sure the new `raid6_pq` module is used, as otherwise the unpatched module gets loaded by other modules depending on it during initramfs, at least on Ubuntu 24.04. Future kernel upgrades should automatically rebuild the DKMS module, and update the initramfs.

Reboot, and load the nonraid driver (`modprobe nonraid super=/unraid.dat`) and you should have the nonraid driver interface `/proc/nmdcmd` available.

## Array Management
### General notes
The driver provides no automation what so ever, and for example array member disks need to be imported manually every time the driver is loaded, in the correct slots and with correct parameters.

It's important to understand that many things related to automatic detection etc mentioned in the UnRAID storage management docs: https://docs.unraid.net/unraid-os/manual/storage-management are handled by the commercial UnRAID system, and not by the array driver component.

Scripts could be made to automate the array management operations, but as of now they do not exist.

### Superblock file
Array state is kept in a superblock file, that is stored outside the array and read by the driver when the driver module is loaded.

> [!IMPORTANT]
> Superblock filename must be given as kernel module parameter: `modprobe nonraid super=/unraid.dat`

Superblock file contains the array configuration, including which disk id is assigned to which slot, and what their state was at the time of last superblock update.

If the superblock file is lost, the array can be recreated with the original import parameters (and you are even able to skip parity reconstruction if you are sure there has been no data changes), but superblock file is needed if you have had a disk failure and need to start the array with a missing disk.

Guessing from the official docs, the "New Config" operation in the UnRAID system is probably basically reloading the driver with a new, empty superblock file, allowing you to start again in the NEW_ARRAY state and import the existing disks in whatever order and configuration you want.
 - Manually this is done by reloading the driver with a different superblock parameter: `modprobe -r nonraid && modprobe nonraid super=/new_unraid.dat` (or moving the old superblock file out of the way before reloading the driver)
 - By default NEW_ARRAY always marks parity disks invalid forcing parity reconstruction, so the "Parity is valid" UI option in "New config" basically does  `echo "set invalidslot 98" > /proc/nmdcmd` before starting the array, which causes the driver to not consider parity disks as invalid when creating an array, and thus skipping needing reconstruction.
   - It should go without saying, but this is a very error-prone operation when done completely manually without an UI double checking everything, so be careful!

### nmdcmd
Array is managed via procfs `/proc/nmdcmd`, see [docs/nmdcmd.8](docs/nmdcmd.8) for "man page". The documentation is LLM generated from the driver source code, but seems to be mostly correct.

> [!TIP]
> You need to write commands to the `/proc/nmdcmd` file as root, so either do it from root shell or use `sudo sh -c 'echo "command" > /proc/nmdcmd'` (or `echo "command"|sudo tee /proc/nmdcmd`)
>
> Driver by default logs commands to dmesg, to see more output from the commands you can run: `echo "set md_trace 2" > /proc/nmdcmd`

### nmdstat
The driver provides an procfs status interface file: `/proc/nmdstat`, which is used to check the current state of the array and its members. See [docs/nmdstat.5](docs/nmdstat.5) for LLM generated "man page".

It is a text file that lists the status of each nmd device, including the disks assigned to it, their sizes, and their states. If you are using this with any data you care about, you should have monitoring for the different array and disk states in this file.

### Creating a new array
1. Import disks (parity is not required, but as a whole example here).
The disks must be partitioned, and the size (in 1k blocks) parameter in the import must cover the whole partition. Check docs/nmdcmd.8 for details on the params.
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
modprobe nmd super=/unraid.dat
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

### Driver bugs / "features"
As the driver is not intended to be used manually, normally the UnRAID UI makes sure it doesn't do things which cause driver issues, but manually these are easy to run into:
* Unassigning same slot twice increases disk missing counter twice, causing the array to enter TOO_MANY_MISSING_DISKS state - this requires driver reload to reset: `modprobe -r nmd && modprobe nmd super=/unraid.dat`
* Same goes for many other internal state counters - best practise seems to be to always reload the driver after a single array operation

## Caveats
- This is just a forked open-source `md_unraid` driver for those who are interested in DIY - it simply handles the array and parity. You have to manually handle importing disks directly via the driver management interface, starting with the correct parameters, doing necessary reconstructions/checks, monitoring the array state from /proc/nmdstat etc.
- You'll need to manually create filesystems on the nmd devices, and mount them, and add mergerfs yourself to combine them into a single mount point if that is desired
- Driver is pretty finicky, and can easily crash the kernel if commands are given in wrong order or with wrong parameters - some custom patches are used to avoid most common crashes (like trying to start array without importing all disks)
- It is easy to end up in an error state unassigning/reassigning disks, and trying to continue blindly often results in kernel panic - error states can usually be cleared by a driver reload and then trying imports again
- Driver should be able to handle disks from an actual UnRAID system, but I have never used or installed UnRAID so I don't actually know
  - Other way around, moving array created on this to UnRAID probably does NOT work, unless the disks have been partitioned exactly as UnRAID system expects them to be
- This was done out of interest and for fun - no guarantees on how quickly upstream changes are synced, or if they are done at all
- Really, you should probably just get UnRAID

## Plans
- Look into adding support for more kernel versions, currently only 6.6 - 6.8 are supported, upstream has patches for 6.12 which should probably work from 6.11+
- Look into setting up a PPA for the dkms package, making updates easier
- Scripts to automate common array management tasks, like importing disks, starting arrays, etc.

## License
This project is licensed under the GNU General Public License v2.0 (GPL-2.0) - the same license as the Linux kernel, and the `md_unraid` driver itself.

## Disclaimer
Unraid is a trademark of Lime Technology, Inc. This project is not affiliated with Lime Technology, Inc. in any way.
