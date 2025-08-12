# NonRAID - unRAID storage array compatible kernel driver

![nmdctl status screenshot](images/status_screenshot.png?raw=true "NonRAID nmdctl")

NonRAID is a fork of the unRAID system's open-source `md_unraid` kernel driver for supported kernels, but targeting primarily Ubuntu 24.04 LTS, and Debian 12/13, enabling UnRAID-style storage arrays with parity protection outside of the commercial UnRAID system.

Unlike in UnRAID, where the driver replaces the kernel's standard `md` driver, the NonRAID driver has been separated into it's own kernel module (`md_nonraid`). This allows it to be easily added as a DKMS module on Ubuntu and Debian based systems, without needing to patch the kernel or replace the standard `md` driver. We do however replace the standard `raid6_pq` module with our patched version, as the upstream driver depends on those patches for the parity calculations.

While this is a fork, we try to keep the changes to driver minimal to make syncs with upstream easier. The driver currently has patches to rebrand and separate the module from `md`, and a couple of patches to prevent kernel crashes if starting the array without importing all disks first or importing in the "wrong" order.

> [!WARNING]
> :radioactive: This is an experimental project, the driver has not really been tested outside of virtualized dev environment and data loss is a possibility.
> This is intended for DIY enthusiasts who want to try out the disk array tech without installing UnRAID.
>
> Use at your own risk, and always have backups!


## Kernel support matrix

| Kernel range  | NonRAID module branch | Upstream base | Tested distros | Notes |
| ------------- | --------------------- | ------------- | -------------- | ------ |
| 6.1 - 6.4 | [nonraid-6.1](https://github.com/qvr/nonraid/tree/nonraid-6.1) | unRAID 6.12.15 (6.1.126-Unraid) | Debian 12 | Contains fixes backported from 6.6 branch |
| 6.5 - 6.8 | [nonraid-6.6](https://github.com/qvr/nonraid/tree/nonraid-6.6) | unRAID 7.0.1 (6.6.78-Unraid) | Ubuntu 24.04 LTS GA kernel | No functional difference to 6.12 branch |
| 6.11 - 6.14 | [nonraid-6.12](https://github.com/qvr/nonraid/tree/nonraid-6.12) | unRAID 7.1.2 (6.12.24-Unraid) | Ubuntu 24.04 LTS HWE kernel, Debian 13 | |

The supported kernel version ranges might be inaccurate, the driver has been tested to work on **Ubuntu 24.04 LTS** GA kernel (6.8.0) and HWE kernels (6.11 and 6.14), on **Debian 12** (6.1) and on **Debian 13** (6.12). Note that kernel versions 6.9 and 6.10 are not supported. You can report other distributions and kernel versions that work in the [discussions](https://github.com/qvr/nonraid/discussions).

> [!NOTE]
> Ubuntu 24.04 LTS HWE kernel users should be aware that future HWE kernel version changes might include kernel ABI changes that could cause the driver to stop working until an update is released.

## Installation

For Ubuntu/Debian based systems, you can install NonRAID through the PPA repository or by downloading the packages directly.

### Option 1: Install from PPA

The [PPA repository](https://launchpad.net/~qvr/+archive/ubuntu/nonraid) provides an easy way to install and update NonRAID packages:

```bash
# Add the NonRAID PPA
sudo add-apt-repository ppa:qvr/nonraid
sudo apt update

# Install prerequisites and NonRAID packages
sudo apt install linux-headers-$(uname -r) nonraid-dkms nonraid-tools
```

> [!TIP]
> This PPA has been tested to work on Ubuntu 24.04 LTS and Debian 12/13, though on Debian you need to manually add the repository and the signing key.

### Option 2: Install from GitHub Releases

1. Download the latest packages from separate releases:
   - [DKMS kernel module package](https://github.com/qvr/nonraid/releases?q=nonraid+dkms)
   - [Management tools package](https://github.com/qvr/nonraid/releases?q=nonraid+tools)

2. Install the prerequisites and both packages:
```bash
# Install prerequisites
sudo apt install dkms linux-headers-$(uname -r) build-essential

# Install the DKMS module and management tools
sudo apt install ./nonraid-dkms_*.deb ./nonraid-tools_*.deb
```

### Option 3: Fully manual installation from repository source
For other distributions, or if you want to build the DKMS module manually, you can clone the repository and build the DKMS module from source, and copy the management tool from [tools/nmdctl](tools/nmdctl).

### Post-installation steps

```bash
# Verify the DKMS module installation
sudo dkms status

# Update the initramfs to include the patched raid6_pq module
sudo update-initramfs -u -k all
```

> [!NOTE]
> Updating the initramfs is needed to make sure the new `raid6_pq` module is used, as otherwise the unpatched module gets loaded by other modules depending on it during initramfs, at least on Ubuntu 24.04. Future kernel upgrades should automatically rebuild the DKMS module, and update the initramfs.

Reboot your system. After rebooting, you can start using NonRAID by creating a new array with the command:

```bash
sudo nmdctl create
```

This [nmdctl](#array-management) command will load the NonRAID driver module and guide you through array creation. Once the array is created, the included [systemd service](tools/systemd/nonraid.service) will automatically start the array and mount the disks on subsequent system boots.

> [!TIP]
> `/nonraid.dat` is the default location for the superblock file used by the `nmdctl` tool. The superblock file contains the array configuration and is stored outside of the array disks. You can specify a different superblock file location with the `-s` option, as explained in the "Using a custom superblock file location" section below.

## Array Management

The command line [nmdctl tool](tools/nmdctl) handles common NonRAID array operations, making it easier to manage the array without using the [raw driver interface](#manual-management-using-driver-interface).

### Display array status

Displays the status of the array and individual disks. Displays detected filesystems, mountpoints and filesystem usage. Drive ID's are also displayed if global `--verbose` option is set.
```bash
sudo nmdctl status
```
Exits with an error code if there are any issues with the array, so this can be used as a simple monitoring in a cronjob. (Global `--no-color` option disables `nmdctl` colored output, making it more suitable for cron emails.)

### Create a new array (interactive)

This assumes that the disks are already partitioned - the largest (unused) partition will be shown as an option to add to the array.

You can partition the disks with the command `sudo sgdisk -o -a 8 -n 1:32K:0 /dev/sdX` (this will create a new partition table on the disk, so be careful to use the correct disk).
```bash
sudo nmdctl create
```
Once the array is started, the nmd devices will be available as `/dev/nmdXp1`, where `X` is the slot number. They can then be formatted with your desired filesystem (XFS, BTRFS, ZFS, etc.) and mounted (manually, or with `nmdctl mount`).
> [!IMPORTANT]
> If you are using ZFS, make sure there is no service (like `zfs-import-cache.service`) which can automatically import the ZFS pool(s) from raw `/dev/sdX` devices on boot, as this will cause the NonRAID parity to **silently** become invalid requiring a corrective parity check (`nmdctl check`). `nmdctl mount` imports ZFS pools with the option `cachefile=none` to avoid this particular `zfs-import-cache` issue.
>
> This of course also applies to any other filesystem too, they should never be directly mounted from the raw `/dev/sdX` devices.
>
> One way to avoid this is to use LUKS encryption on the NonRAID disks, which prevents OS services from detecting the filesystems on raw devices at all.

#### "New Config" mode
If you want to change the array topology, like adding or removing disks, you can use the "New Config" mode. This is similar to the UnRAID "New Config" operation, and it allows you to start with a fresh array configuration without needing to recreate the configuration completely from scratch. If a disk to be removed disk is first zeroed **properly**, then you can optionally mark the parity valid for the newly created config to skip the parity reconstruction.
```bash
sudo nmdctl create
```
This will detect an existing array configuration, and prompt you to confirm that you want to create a new array configuration. Old superblock file will be renamed to `/nonraid.dat.bak` (default path), and a new superblock file will be created. When assigning disks to slots, the old slot assignment is given as an option.

### Start/stop the array

Starting an array commits all configuration changes like array creation, disk unassignments, additions or replacements to the array. Automatically imports all disks to the array.
```bash
sudo nmdctl start/stop
```
> [!NOTE]
> If you are trying to start/import an existing UnRAID array, and you get an warning about size mismatch between detected and configured partition size, do not continue the import, but open an issue with details.

### Import all disks to the array without starting

Useful if you want to add a new disk which needs to be done before starting the array.
```bash
sudo nmdctl import
```

### Add a new disk (interactive)

Disk must already be partitioned as with `create`, and the disk must not already be assigned to the array. Only one disk can be added at a time.
```bash
sudo nmdctl add
```

### Replace a disk (interactive)

Replaces a disk in the specified already unassigned slot with a new disk. The new disk must not already be assigned to the array, and it must be partitioned as described above.
```bash
sudo nmdctl replace SLOT
```

### Unassign a disk from a slot

Unassigns a disk from the specified slot, effectively removing it from the array. Disk contents will be emulated from parity and other data disks when the array is started.
```bash
sudo nmdctl unassign SLOT
```

### Mount all data disks

Mounts all detected unmounted filesystems to `MOUNTPREFIX` (default `/mnt/diskN`). LUKS devices are opened with a key-file (global `--keyfile` option, default `/etc/nonraid/luks-keyfile`).
```bash
sudo nmdctl mount [MOUNTPREFIX]
```

### Unmount all data disks

Unmounts all detected mounted filesystems. LUKS devices are closed after filesystem unmount.
```bash
sudo nmdctl unmount
```

### Start/stop a parity check

This will also start reconstruction or clear operations depending on the array state, user confirmation is required if a normal parity check is not being started. In unattended mode (`-u`), the check will default to check only mode (`NOCORRECT`).
```bash
sudo nmdctl check/nocheck OPTION
```
Where `OPTION` can be:
- `CORRECT` - start a corrective parity check, this is the default if no option is given
- `NOCORRECT` - start a check-only parity check, this is the default in unattended mode
- `RESUME` - resume a previously started parity check
- `CANCEL` - (for `nocheck`) cancel a running parity check
- `PAUSE` - (for `nocheck`) pause a running parity check

### Set array settings

Used to modify array settings, like enabling "turbo write mode" (`md_write_method`) or changing the debug level (`md_trace`). The command will display all available settings if no setting is specified. Empty value will change the setting to its default value.
```bash
sudo nmdctl set SETTING VALUE
```

### Reload the nonraid module

Reloads the driver module with the specified superblock path. This is can be used to recover from error states or when changing superblock files.
```bash
sudo nmdctl reload
```
This command effectively does `modprobe -r nonraid && modprobe nonraid super=/nonraid.dat` and is sometimes necessary to reset the driver's internal state after operations like unassigning disks or initial array creation.

### Using a custom superblock file location
Commands will load the driver module automatically if it is not loaded already, and the tool defaults to using `/nonraid.dat` as the superblock file path. To use a different location:
```bash
sudo nmdctl -s /path/to/superblock.dat reload
```

For more details, run `sudo nmdctl --help`

## Manual Management (Using Driver Interface)
The driver provides no automation what so ever, and for example array member disks need to be imported manually every time the driver is loaded, in the correct slots and with correct parameters.

It's important to understand that many things related to automatic detection etc mentioned in the UnRAID storage management docs: https://docs.unraid.net/unraid-os/manual/storage-management are handled by the commercial UnRAID system, and not by the array driver component.

While the `nmdctl` tool now automates many tasks, understanding the underlying driver interface is still valuable for troubleshooting or advanced usage.

<details>

<summary>Expand for Driver Interface Details</summary>

### Superblock file
Array state is kept in a superblock file, that is stored outside the array and read by the driver when the driver module is loaded.

> **Important**
>
> Superblock filename must be given as kernel module parameter: `modprobe nonraid super=/nonraid.dat`

Superblock file contains the array configuration, including which disk id is assigned to which slot, and what their state was at the time of last superblock update.

If the superblock file is lost, the array can be recreated with the original import parameters (and you are even able to skip parity reconstruction if you are sure there has been no data changes), but superblock file is needed if you have had a disk failure and need to start the array with a missing disk.

Guessing from the official docs, the "New Config" operation in the UnRAID system is probably basically reloading the driver with a new, empty superblock file, allowing you to start again in the NEW_ARRAY state and import the existing disks in whatever order and configuration you want.
 - Manually this is done by reloading the driver with a different superblock parameter: `modprobe -r nonraid && modprobe nonraid super=/new_nonraid.dat` (or moving the old superblock file out of the way before reloading the driver)
 - By default NEW_ARRAY always marks parity disks invalid forcing parity reconstruction, so the "Parity is valid" UI option in "New config" basically does  `echo "set invalidslot 98" > /proc/nmdcmd` before starting the array, which causes the driver to not consider parity disks as invalid when creating an array, and thus skipping needing reconstruction.
   - It should go without saying, but this is a very error-prone operation when done completely manually without an UI double checking everything, so be careful!

### nmdcmd
The driver is managed via procfs interface `/proc/nmdcmd`, see [docs/nmdcmd.8](docs/nmdcmd.8) for "man page". The documentation is LLM generated from the driver source code, but seems to be mostly correct.

> **Tip**
>
> You need to write commands to the `/proc/nmdcmd` file as root, so either do it from root shell or use `sudo sh -c 'echo "command" > /proc/nmdcmd'` (or `echo "command"|sudo tee /proc/nmdcmd`)
>
> Driver by default logs commands to dmesg, to see more output from the commands you can run: `echo "set md_trace 2" > /proc/nmdcmd`

### nmdstat
The driver provides an procfs status interface file: `/proc/nmdstat`, which is used to check the current state of the array and its members. See [docs/nmdstat.5](docs/nmdstat.5) for LLM generated "man page".

It is a text file that lists the status of each nmd device, including the disks assigned to it, their sizes, and their states. If you are using this with any data you care about, you should have monitoring for the different array and disk states in this file.

### Creating a new array
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

> **Tip**
>
> If you want to start the array with a missing disk, the slot must still be imported with empty device name and other parameters (unassigning a disk):
>  ```
> echo "import 2 '' 0 0 0 ''" > /proc/nmdcmd
> ```
> and then array needs to started with the DISABLE_DISK option to run in degraded mode (missing disk simulated from parity and other data disks)

### Driver bugs / "features"
As the driver is not intended to be used manually, normally the UnRAID UI makes sure it doesn't do things which cause driver issues, but manually these are easy to run into:
* Unassigning same slot twice increases disk missing counter twice, causing the array to enter TOO_MANY_MISSING_DISKS state - this requires driver reload to reset: `modprobe -r nonraid && modprobe nonraid super=/nonraid.dat` or `nmdctl reload`
* Same goes for many other internal state counters - best practise seems to be to always reload the driver after a single array operation

`nmdctl` tries to handle these driver issues either automatically or by warning the user to reload the driver if an inconsistent state is detected

</details>

## Caveats
- This is a forked open-source `md_unraid` driver for DIY enthusiasts interested in the technology behind UnRAID storage arrays. While the `nmdctl` tool now provides a semi-user-friendly interface for common array operations (creating arrays, adding/replacing disks, mounting filesystems, etc.), this is still primarily aimed at advanced users comfortable with Linux command line.
- The commercial UnRAID system offers a much more comprehensive solution with a polished web UI, plugins ecosystem, and virtualization features that this project doesn't aim to replicate.
- You'll still need to manually handle some aspects like creating filesystems on the nmd devices and setting up mergerfs if you want to combine multiple disks into a single mount point.
- While the `nmdctl` tool handles many error conditions, the underlying driver can still be finicky. Custom patches are used to avoid most common crashes (like trying to start the array without importing all disks).
- If you encounter issues, please use the [project discussions](https://github.com/qvr/nonraid/discussions) for support rather than UnRAID forums, as this is an independent project.
- Driver should be able to handle disks from an actual UnRAID system, but I have never used or installed UnRAID so I don't actually know
  - Other way around, moving array created on this to UnRAID _probably_ does not work, unless the disks have been partitioned exactly as UnRAID system expects them to be - if you can try it out, please report back!
- This was done out of interest and for fun - no guarantees on how quickly upstream changes are synced, or if they are done at all
- For a complete, polished storage solution with support, you might want to still consider getting UnRAID

## Plans
- **IF** we decide to diverge further from the upstream, the module should be fairly simple to modify to build on multiple kernel versions, so that we dont have to ship multiple versions of the module code for different kernel versions (and we would be able to support 6.9 and 6.10 kernels too) - currently not planned though

## License
This project is licensed under the GNU General Public License v2.0 (GPL-2.0) - the same license as the Linux kernel, and the `md_unraid` driver itself. See [LICENSE](LICENSE) for the full license text.

Individual Linux kernel source files (under `raid6_pq/`, `md_nonraid`, or the upstream changes tracking branch `upstream`) may have a different license, or be provided under a dual license, but the overall Linux Kernel is GPL-2.0 licensed, with their syscall exception. (See Linux Kernel `Documentation/process/license-rules.rst` for details on kernel licensing rules.)

## Disclaimer
Unraid is a trademark of Lime Technology, Inc. This project is not affiliated with Lime Technology, Inc. in any way.
