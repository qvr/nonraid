# NonRAID - unRAID storage array compatible kernel driver

![nmdctl status screenshot](images/status_screenshot.png?raw=true "NonRAID nmdctl")

NonRAID is a fork of the unRAID system's open-source `md_unraid` kernel driver for supported kernels, but targeting primarily Ubuntu 24.04 LTS, and Debian 12/13, enabling UnRAID-style storage arrays with parity protection outside of the commercial UnRAID system.

### How it works

NonRAID driver takes multiple block devices (drives) and turns them into a storage array where 1-2 devices serve as parity disks. These store calculated parity data that, together with the remaining data drives, can reconstruct any single (or dual, with two parity drives) drive failure. The rest are data disks, and each will have an independent filesystem for user data.

When the array starts, it creates new virtual block devices (`/dev/nmdXp1`) for each data disk with real-time parity protection. Each data disk can have different filesystems - you can format one with XFS, another with BTRFS, have LUKS encryption with ZFS on one, etc.

You can add drives of different sizes, with the parity drive(s) needing to be at least as large as the biggest data drive. In catastrophic disk failures where more drives fail than parity would allow, only the data on the failed drives can be lost - all data disk filesystems can still be accessed individually even without the array running.

This provides redundancy against drive failures while allowing mixed drive sizes and filesystems. You can then present all the separate filesystems as a single mount point using tools like [mergerfs](https://github.com/trapexit/mergerfs), giving you a unified storage pool with parity protection.

### Driver implementation differences

Unlike in UnRAID, where the driver replaces the kernel's standard `md` driver, the NonRAID driver has been separated into it's own kernel module (`md_nonraid`). This allows it to be easily added as a DKMS module on supported kernel versions, without needing to patch the kernel or replace the standard `md` driver. Upstream UnRAID additionally patches system's standard `raid6_pq` module for RAID-6 parity calculations, NonRAID instead ships a separate `nonraid6_pq` module with the parity patches, which operates alongside the untouched `raid6_pq` module without potential conflicts.

While this is a fork, we try to keep the changes to driver minimal to make syncs with upstream easier. The driver currently has patches to rebrand and separate the module from `md` and from `raid6_pq`, and a couple of patches to prevent kernel crashes if starting the array without importing all disks first or importing in the "wrong" order.

> [!WARNING]
> :radioactive: This is an early-stage project, and while the driver and `nmdctl` management tool have been tested in both virtualized environments and some physical setups, data loss is still a possibility.
> This is mainly intended for DIY enthusiasts comfortable with Linux command line usage.
>
> Use at your own risk, and always have backups!

## Table of Contents

- [Kernel support matrix](#kernel-support-matrix)
- [Installation](#installation)
  - [Option 1: Install from PPA](#option-1-install-from-ppa)
  - [Option 2: Install from GitHub Releases](#option-2-install-from-github-releases)
  - [Option 3: Fully manual installation from repository source and 3rd party packages](#option-3-fully-manual-installation-from-repository-source-and-3rd-party-packages)
  - [Post-installation steps](#post-installation-steps)
- [Quick Start](#quick-start)
- [Array Management](#array-management)
  - [Display array status](#display-array-status)
  - [Create a new array (interactive)](#create-a-new-array-interactive)
  - [Start/stop the array](#startstop-the-array)
  - [Import all disks to the array without starting](#import-all-disks-to-the-array-without-starting)
  - [Add a new disk (interactive)](#add-a-new-disk-interactive)
  - [Replace a disk (interactive)](#replace-a-disk-interactive)
  - [Unassign a disk from a slot](#unassign-a-disk-from-a-slot)
  - [Mount all data disks](#mount-all-data-disks)
  - [Unmount all data disks](#unmount-all-data-disks)
  - [Start/stop a parity check](#startstop-a-parity-check)
  - [Set array settings](#set-array-settings)
  - [Reload the nonraid module](#reload-the-nonraid-module)
  - [Using a custom superblock file location](#using-a-custom-superblock-file-location)
- [Manual Management (Using Driver Interface)](#manual-management-using-driver-interface)
- [Caveats](#caveats)
- [Plans](#plans)
- [License](#license)
- [Disclaimer](#disclaimer)

## Kernel support matrix

| Kernel range  | NonRAID module branch | Upstream base | Tested distros | Notes |
| ------------- | --------------------- | ------------- | -------------- | ------ |
| 6.1 - 6.4 | [nonraid-6.1](https://github.com/qvr/nonraid/tree/nonraid-6.1) | unRAID 6.12.15 (6.1.126-Unraid) | Debian 12 | Contains fixes backported from 6.6 branch |
| 6.5 - 6.8 | [nonraid-6.6](https://github.com/qvr/nonraid/tree/nonraid-6.6) | unRAID 7.0.1 (6.6.78-Unraid) | Ubuntu 24.04 LTS GA kernel | No functional difference to 6.12 branch |
| 6.11 - 6.17 | [nonraid-6.12](https://github.com/qvr/nonraid/tree/nonraid-6.12) | unRAID 7.1.2 (6.12.24-Unraid) | Ubuntu 24.04 LTS HWE kernel, Debian 13, Arch, Proxmox VE 9 | unRAID 7.2.0 has no changes to the kernel driver |

The supported kernel version ranges might be inaccurate, the driver has been tested to work on **Ubuntu 24.04 LTS** GA kernel (6.8.0) and HWE kernels (6.11 and 6.14), on **Debian 12** (6.1), on **Debian 13** (6.12), on **Arch Linux** lts kernel (6.12) and stable kernels (6.16, 6.17) and on **Proxmox VE 9** (6.14). Note that kernel versions 6.9 and 6.10 are not supported. You can report other distributions and kernel versions that work in the [discussions](https://github.com/qvr/nonraid/discussions).

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

<details>
<summary>PPA setup for Debian</summary>

```bash
# Install gpg
sudo apt install gpg

# Add the PPA signing key
wget -qO- "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x0B1768BC3340D235F3A5CB25186129DABB062BFD" | sudo gpg --dearmor -o /usr/share/keyrings/nonraid-ppa.gpg

# Add the PPA repository
echo "deb [signed-by=/usr/share/keyrings/nonraid-ppa.gpg] https://ppa.launchpadcontent.net/qvr/nonraid/ubuntu noble main" | sudo tee /etc/apt/sources.list.d/nonraid-ppa.list
```

</details>

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

### Option 3: Fully manual installation from repository source and 3rd party packages
For **Arch Linux**, you can find NonRAID in an 3rd party maintained [AUR](https://aur.archlinux.org/packages/nonraid-git). Install the `nonraid-git` package from AUR using your preferred AUR helper (e.g. `yay -Syu nonraid-git`).

For other distributions, or if you want to build the DKMS module manually, you can also clone the repository and build the DKMS module from source, and copy the management tool from [tools/nmdctl](tools/nmdctl).

<details>
<summary>Manual DKMS installation steps</summary>

#### Prerequisites
Make sure you have git, build tools (gcc, make, etc.), DKMS, and kernel headers for your running kernel installed.

#### Installation Steps

1. **Clone the repository:**
```bash
git clone https://github.com/qvr/nonraid.git
cd nonraid
```

2. **Get the current NonRAID DKMS module version:**
```bash
DKMS_VERSION=$(grep "^PACKAGE_VERSION=" dkms.conf | cut -d= -f2)
echo "DKMS version: $DKMS_VERSION"
```

3. **Copy sources to DKMS source directory:**
```bash
DKMS_SRC_DIR="/usr/src/nonraid-dkms-$DKMS_VERSION"
sudo mkdir -p "$DKMS_SRC_DIR"
sudo cp -r md_nonraid/ raid6/ dkms.conf Makefile "$DKMS_SRC_DIR/"
```

4. **Build and install the module:**
```bash
KVERSION=$(uname -r)
sudo dkms install nonraid-dkms/$DKMS_VERSION -k "$KVERSION"
```

5. **Install the management tool:**
```bash
sudo cp tools/nmdctl /usr/local/bin/nmdctl
sudo chmod +x /usr/local/bin/nmdctl
# You might also want to copy the systemd services/timers and default config
# from tools/systemd/
```

6. **Verify installation:**
```bash
sudo dkms status
# Should show: nonraid-dkms/$DKMS_VERSION, $KVERSION, $(uname -m): installed

# Test the management tool
sudo nmdctl --help
```

#### Troubleshooting

If the build fails, check:
- Kernel headers are properly installed: `ls /lib/modules/$(uname -r)/build/`
- DKMS log files: `sudo dkms status -v` and `/var/lib/dkms/nonraid-dkms/$DKMS_VERSION/build/make.log`
- Your kernel version is supported (see [Kernel support matrix](#kernel-support-matrix))

</details>

### Post-installation steps

```bash
# Verify the DKMS module installation
sudo dkms status
# You should see the DKMS module installed for your kernel version:
# nonraid-dkms/1.3.0, 6.14.0-27-generic, x86_64: installed
```

> [!IMPORTANT]
> **Installing kernel headers meta-package**: To ensure DKMS can rebuild the NonRAID modules when your kernel gets updated, you should also install your distribution's kernel headers meta-package. This ensures that new kernel headers are automatically pulled in during kernel updates:
> - **Ubuntu**: `sudo apt install linux-headers-generic` (or `linux-headers-generic-hwe-24.04` for HWE kernels)
> - **Debian**: `sudo apt install linux-headers-amd64` (or the appropriate package for your kernel flavor)
>
> Without the kernel headers meta-package, new kernel updates might not install the corresponding headers, which would prevent DKMS from building the NonRAID modules for the new kernel.

That's it! The NonRAID kernel modules are installed and DKMS should take care of rebuilding them automatically when the kernel gets updated in the future. No reboot should be necessary, you can start using NonRAID by creating a new array with the command:

```bash
sudo nmdctl create
```

This [nmdctl](#array-management) command will load the NonRAID driver module and guide you through array creation. Once the array is created, the included [systemd service](tools/systemd/nonraid.service) will automatically start the array and mount the disks on subsequent system boots.

> [!TIP]
> `/nonraid.dat` is the default location for the superblock file used by the `nmdctl` tool. The superblock file contains the array configuration and is stored outside of the array disks. You can specify a different superblock file location with the `-s` option, as explained in the "Using a custom superblock file location" section below.

## Quick Start
After installing the NonRAID kernel modules and tools, here are the steps to bring a brand new NonRAID array online, format disks, and configure the included systemd services.

**Prepare and partition disks**

Identify the raw devices you want to use (e.g. `/dev/sdb`, `/dev/sdc`, `/dev/sdd`). They must NOT contain data you still need. Ensure each disk has a stable / unique ID in `/dev/disk/by-id/` (virtual environments sometimes need tuning for this).

Create a fresh partition table and a single aligned partition covering the disk - this will wipe any existing data on the disk:

```bash
# Check carefully for correct disk letters before running!
sudo sgdisk -o -a 8 -n 1:32K:0 /dev/sdX
```

This command creates a new GPT partition table (`-o`) with 8-sector alignment (`-a 8`), adds a single partition (`-n 1`) starting at 32KB (`32K`) and extending to the end of the disk (`0`).

**Create the array configuration**

Run the interactive wizard and assign 1–2 largest disks as parity. Assign remaining disks into data slots. Confirm when prompted.

```bash
sudo nmdctl create
```

**Start the array and run initial parity sync**

The create wizard can start it immediately when finished; if you skipped that option:

```bash
sudo nmdctl start
```

Then kick off the initial array parity sync:

```bash
sudo nmdctl check
```
This can take several hours depending on disk sizes. You can monitor progress with `sudo nmdctl status --monitor`, but you can also proceed to creating filesystems and mounting the disks while the parity sync runs in the background.

After start, new block devices appear for data disks as `/dev/nmd1p1`, `/dev/nmd2p1`, ...

**(Optional) Enable LUKS encryption BEFORE filesystem creation**

If you want encryption, do for each data device first, then create the filesystem on the opened mapper path:

```bash
sudo cryptsetup luksFormat /dev/nmd1p1
sudo cryptsetup open /dev/nmd1p1 nmd1p1 # Use the NonRAID disk name as the mapper name
# Then in next step format /dev/mapper/nmd1p1 instead of the raw /dev/nmd1p1
```

You can save the LUKS keyfile as `/etc/nonraid/luks-keyfile` so that `nmdctl mount` can open the disks automatically on boot.

**Create filesystems on the NonRAID block devices**

XFS:
```bash
sudo mkfs.xfs /dev/nmd1p1
sudo mkfs.xfs /dev/nmd2p1
# Repeat for all data slots
```

BTRFS also works (`sudo mkfs.btrfs /dev/nmd3p1`).

For ZFS, name the pools as `diskN` where N is the slot number so that `nmdctl mount` works properly:

```bash
sudo zpool create disk3 /dev/nmd3p1
```

You can use any combination of different filesystems based on your needs.

> [!IMPORTANT]
> Always use the NonRAID block devices (`/dev/nmdXp1`) for filesystem creation, never the underlying raw drives (`/dev/sdX`). Using raw drives bypasses parity protection and will invalidate the array parity.

**Mount the data disks**

Let `nmdctl` mount everything (creates `/mnt/diskN` by default; opens LUKS devices automatically if keyfile is present):

```bash
sudo nmdctl mount
```

Mount point prefix can be configured in [/etc/default/nonraid](tools/systemd/nonraid.default). You can optionally combine the mountpoints with [mergerfs](https://github.com/trapexit/mergerfs) (outside the scope of this quick start).

**Configure notifications and verify auto‑start**

Configure notifications for array events by editing [/etc/default/nonraid](tools/systemd/nonraid.default) and setting `NONRAID_NOTIFY_CMD` to your desired notification command.

Once the initial parity sync is complete, you should reboot once to verify the included systemd services / timers load and the array & disks auto‑start / mount:

```bash
sudo reboot
# After reboot verify the array has started and disks are mounted:
sudo nmdctl status
df -h | grep /mnt/disk
```

You now have a running NonRAID array with individually formatted disks, parity protection, and automatic startup. You can read on for detailed usage of the `nmdctl` commands below.

## Array Management

The command line [nmdctl tool](tools/nmdctl) handles common NonRAID array operations, making it easier to manage the array without using the [raw driver interface](#manual-management-using-driver-interface).

### Display array status

Displays the status of the array and individual disks. Displays detected filesystems, mountpoints and filesystem usage. Drive ID's are also displayed if `--verbose` option is set.

```bash
sudo nmdctl [--no-color] status [--verbose] [--no-fs] [-o OUTPUT] [--monitor [INTERVAL]]
```

Options:
- `--verbose` - Show detailed status information including drive IDs
- `--no-fs` - Skip displaying filesystem information (slightly faster)
- `-o, --output FORMAT` - Specify output format: `default`, `prometheus`, `json`, or `terse`
- `--monitor [INTERVAL]` - Enable monitor mode, refreshing every INTERVAL seconds (default: 2)
- `--no-color` (global option) - Disable colored output, suitable for cron emails

Exits with an error code if there are any issues with the array, so this can also be used as a simple array monitoring in a cronjob.

> [!TIP]
> NonRAID includes a systemd timer and service ([nonraid-notify.service](tools/systemd/nonraid-notify.service)) that uses the terse format to send alert notifications if the array health becomes degraded. Configure it by setting `NONRAID_NOTIFY_CMD` in [/etc/default/nonraid](tools/systemd/nonraid.default).
>
> Any command that accepts the notification message from standard input (pipe) will work, but [Apprise](https://github.com/caronc/apprise) is a great cli tool for sending push notifications to numerous services (Discord, Pushover, email, etc).

### Create a new array (interactive)

This assumes that the disks are already partitioned - the largest (unused) partition will be shown as an option to add to the array. The disks also need to have a unique disk ID that should be visible in `/dev/disk/by-id/` - some virtualization platforms may not expose this by default and `nmdctl` will refuse to use disks missing an ID.

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
> [!CAUTION]
> If you are trying to start/import an existing UnRAID array, and you get an warning about size mismatch between detected and configured partition size, do not continue the import, but open an issue with details.

### Import all disks to the array without starting

Useful if you want to add new disks, or other advanced operations which need to be done before starting the array normally.
```bash
sudo nmdctl import
```

### Add a new disk (interactive)

Disk must already be partitioned as with `create`, and the disk must not already be assigned to the array.
```bash
sudo nmdctl add
```
The disk does not need to be pre-cleared before running this command. Once the array is started after adding the new disk, the disk will not be taken into use until a clear operation is triggered with `nmdctl check`. The clearing operation does not affect parity - only the added new disk gets cleared and then taken into use.

If the disk has been pre-cleared (partition zeroed with `dd if=/dev/zero of=/dev/sdNEW1 bs=1M status=progress`), you can specify this during the interactive prompts, and the disk will be taken into use immediately when the array is started.

> [!WARNING]
> Marking a disk as pre-cleared when it has not actually been properly zeroed will lead to array parity becoming invalid, and a corrective parity check will be required to detect and fix the issue.

### Replace a disk (interactive)

**Important: You must unassign the disk first.** The slot must be explicitly unassigned before replacement to clear the old disk ID from the superblock. This is an intentional safety feature to prevent accidentally replacing the wrong disk.

```bash
sudo nmdctl replace SLOT
```

**Full disk replacement workflow:**
```bash
# 1. Unmount filesystems (if mounted)
sudo nmdctl unmount

# 2. Stop array (if running)
sudo nmdctl stop

# 3. Unassign the failed disk (clears disk ID from superblock)
sudo nmdctl unassign SLOT

# 4. Replace with new disk
sudo nmdctl replace SLOT

# 5. Start array (will enter RECON_DISK state)
sudo nmdctl start

# 6. CRITICAL: Mount filesystems BEFORE starting rebuild
sudo nmdctl mount

# 7. Start rebuild
sudo nmdctl check
```

> [!CAUTION]
> **Mount filesystems BEFORE starting rebuild.** If you start the rebuild before mounting, writes that occur after mounting will update parity but not the already-rebuilt sectors, resulting in stale data on the rebuilt disk.

> [!TIP]
> A special "[Parity Swap](https://docs.unraid.net/legacy/FAQ/parity-swap-procedure/)" operation can be started by replacing an existing parity disk (slot 0 or 29) with a larger disk, and then using the existing parity disk as a replacement for an already unassigned data disk slot. The parity data will then need to be **manually copied** from the old parity disk to the new parity disk, and the rest of the new parity disk must be zeroed **before** starting the array.
>
> Copying can be done with `dd`, for example:
> ```bash
> ( dd if=/dev/sdOLD1 bs=1M status=progress ; dd if=/dev/zero bs=1M status=progress ) > /dev/sdNEW1
> ```
> Where `/dev/sdOLD1` is the old parity disk, and `/dev/sdNEW1` is the new parity disk. Make sure to use the correct device names!

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

Starts or stops a parity check. This will also start reconstruction or clear operations depending on the array state, user confirmation is required if a normal parity check is not being started.

The NonRAID [systemd service](tools/systemd/nonraid.service) will trigger a corrective parity check, if it detects an unclean shutdown has happened.

In unattended mode (`-u`), the check will default to check only mode (`NOCORRECT`), this is recommended for scheduled parity checks and used by the included [quarterly systemd timer](tools/systemd/nonraid-parity-check.timer).

Note that if a parity check corrects any sync errors, the array status will still stay `DEGRADED` until the next parity check without errors.
```bash
sudo nmdctl check OPTION
```
Where `OPTION` can be:
- `CORRECT` - start a corrective parity check, this is the default if no option is given
- `NOCORRECT` - start a check-only parity check, this is the default in unattended mode
- `RESUME` - resume a previously paused parity check
- `CANCEL` - cancel a running parity check
- `PAUSE` - pause a running parity check

### Set array settings

Used to modify array settings, like enabling "turbo write mode" (`md_write_method`) or changing the debug level (`md_trace`). The command will display all available settings if no setting is specified. Empty value will change the setting to its default value.
```bash
sudo nmdctl set SETTING VALUE
```

### Reload the nonraid module

Reloads the driver module with the specified superblock path. This can be used to recover from error states or when changing superblock files.
```bash
sudo nmdctl reload
```
This command effectively does `modprobe -r nonraid && modprobe nonraid super=/nonraid.dat` and is sometimes necessary to reset the driver's internal state after operations like unassigning disks or initial array creation.

### Using a custom superblock file location
Commands will load the driver module automatically if it is not loaded already, and the tool defaults to using `/nonraid.dat` as the superblock file path. To use a different location:
```bash
sudo nmdctl -s /path/to/superblock.dat reload
```
> [!TIP]
> If you change the default superblock location, you should also change it in `/etc/default/nonraid`, as the systemd service will otherwise continue to use the default path when starting the array at boot.

## Manual Management (Using Driver Interface)
If you need to interact with the raw kernel driver (for troubleshooting, development, or to understand what `nmdctl` is doing under the hood), details on the driver interface can be found in: [docs/manual-management.md](docs/manual-management.md).

That document covers: superblock handling, procfs command/status interfaces (`/proc/nmdcmd` / `/proc/nmdstat`), creating and starting arrays purely via echo commands, importing existing arrays after boot, handling degraded / missing disks, and known driver quirks that `nmdctl` works around.

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
- Plan is to keep the driver in sync with upstream changes, and focus on ironing out any sharp edges in `nmdctl`
  - New features to `nmdctl` based on feedback
  - *Not planned* is adding major new features into the array driver (like offline parity, additional parity disks etc)
  - Feature contributions to the driver will be considered though
- **IF** we decide to diverge further from the upstream, the module should be fairly simple to modify to build on multiple kernel versions (with autoconf or similar), so that we dont have to ship multiple versions of the module code for different kernel versions (and we would be able to support 6.9 and 6.10 kernels too) - currently not planned though

## License
This project is licensed under the GNU General Public License v2.0 (GPL-2.0) - the same license as the Linux kernel, and the `md_unraid` driver itself. See [LICENSE](LICENSE) for the full license text.

Individual Linux kernel source files (under `raid6/`, `md_nonraid/`, or the upstream changes tracking branch `upstream`) may have a different license, or be provided under a dual license, but the overall Linux Kernel is GPL-2.0 licensed, with their syscall exception. (See Linux Kernel `Documentation/process/license-rules.rst` for details on kernel licensing rules.)

## Disclaimer
Unraid is a trademark of Lime Technology, Inc. This project is not affiliated with Lime Technology, Inc. in any way.
