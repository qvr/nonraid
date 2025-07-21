## Driver porting development notes
### Vendor's open-source kernel patches
Access the GPL'd kernel patches inside the UnRAID firmware, without installing the software:
```
wget https://unraid-dl.sfo2.cdn.digitaloceanspaces.com/stable/unRAIDServer-6.12.2-x86_64.zip
unzip unRAIDServer-6.12.2-x86_64.zip -d unRAIDServer
cd unRAIDServer
unsquashfs -d patches bzfirmware src
```

There are patches to multiple drivers, but we are only interested in the changes needed for md_unraid functionality:
1. adds md_unraid and unraid.c to drivers/md/
2. patches md Kconfig / Makefile
3. patches raid6 algos.c

- replaces normal md driver, md stays as module
- disables all other md raid support, and a bunch of other DM features
- otherwise md changes are pretty self-contained, patches should apply easily(?) to other kernel versions

#### release 7.1.4
- based on kernel 6.12.24
- patches apply to ubuntu lts 6.8.0 (with offsets), but:
  - `bdev_file_open_by_path` - requires 6.9+
  - `blk_alloc_disk / BLK_FEAT_WRITE_CACHE` - requires 6.11+
- HWE kernel possible? currently 6.11, soon 6.14

#### release 7.0.1
- based on kernel 6.6.78
- has no functional changes compared to 7.1.4, it is only rebased on newer kernel version
- patches apply to ubuntu lts 6.8.0 (with offsets)

#### Kernel issues
* Unlike unraid, Ubuntu has CONFIG_UBSAN=y enabled and this is causing array-index-out-of-bounds kernel errors/warnings in dmesg for all array operations.
  - we disable CONFIG_UBSAN when building the dkms module (though this is probably really a problem with unraid code)

